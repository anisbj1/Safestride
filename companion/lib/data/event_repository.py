"""EventRepository: idempotent upsert, high-water resume, timeline, TimeSync rows (D5).

Seam (spec §7, frozen at IMPLEMENT, additive optional kwargs only):
    upsert(device_id, records) -> int      new rows inserted (duplicates are no-ops)
    high_water(device_id) -> int           SYNC §4.6 resume point, monotonic
    timeline(limit, since_seq=None)        seq DESC page of decoded rows
    time_syncs(boot_id)                    TimeSync points for SYNC §4.5 reconstruction
"""

from __future__ import annotations

import sqlite3
from collections.abc import Sequence

from companion.data.models import (
    EVENT_TYPE_TIME_SYNC,
    DeviceInfo,
    EventRecord,
    TimeSyncPoint,
)
from companion.domain.time_reconstruct import resolve_wall_unix
from companion.transport.codec import decode_event48

_EVENT_COLUMNS = (
    "device_id, seq, boot_id, t_start_ms, duration_ms, t_start_unix, wall_unix, "
    "confidence, reasons, payload, quality, type, label, context, "
    "alertness_at_start, detector_id, schema_version, raw"
)


class EventRepository:
    def __init__(self, conn: sqlite3.Connection) -> None:
        self._conn = conn

    # --- writes -------------------------------------------------------------

    def upsert(self, device_id: str, records: Sequence[bytes]) -> int:
        """Upsert raw 48 B records under PRIMARY KEY(device_id, seq) (SYNC §4.4).

        Returns the number of *newly inserted* rows: duplicate or backward seqs are
        no-ops, so retransmission is always safe. Wall times are set at insert for
        events that carry `t_start_unix`, then unresolved rows (unix 0) are re-tried
        against the TimeSync rows known so far (SYNC §4.5).
        """
        inserted = 0
        for raw in records:
            rec = decode_event48(raw)  # wrong length = caller bug; CodecError propagates
            wall = rec.t_start_unix if rec.t_start_unix != 0 else 0
            cur = self._conn.execute(
                f"INSERT OR IGNORE INTO events ({_EVENT_COLUMNS}) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                (
                    device_id,
                    rec.seq,
                    rec.boot_id,
                    rec.t_start_ms,
                    rec.duration_ms,
                    rec.t_start_unix,
                    wall,
                    rec.confidence,
                    rec.reasons,
                    rec.payload,
                    rec.quality,
                    rec.type,
                    rec.label,
                    rec.context,
                    rec.alertness_at_start,
                    rec.detector_id,
                    rec.schema_version,
                    rec.raw,
                ),
            )
            if cur.rowcount:
                inserted += 1
        if inserted:
            self._recompute_wall_times(device_id)
            self._advance_sync_state(device_id)
        self._conn.commit()
        return inserted

    def record_device(self, info: DeviceInfo, now_unix: int) -> None:
        """Upsert the devices row from a connection's DIS reads (BLE_GATT §3)."""
        self._conn.execute(
            "INSERT INTO devices(device_id, fw, hw, sw, last_seen_unix) VALUES(?, ?, ?, ?, ?) "
            "ON CONFLICT(device_id) DO UPDATE SET fw=excluded.fw, hw=excluded.hw, "
            "sw=excluded.sw, last_seen_unix=excluded.last_seen_unix",
            (info.device_id, info.fw, info.hw, info.sw, now_unix),
        )
        self._conn.commit()

    # --- reads --------------------------------------------------------------

    def high_water(self, device_id: str) -> int:
        """Companion-reboot resume point: max committed seq (SYNC §4.6), 0 = nothing."""
        row = self._conn.execute(
            "SELECT high_water FROM sync_state WHERE device_id=?", (device_id,)
        ).fetchone()
        return int(row["high_water"]) if row is not None else 0

    def timeline(
        self,
        limit: int,
        since_seq: int | None = None,
        *,
        device_id: str | None = None,
    ) -> list[EventRecord]:
        """Newest-first page (seq DESC). `since_seq` is an exclusive upper bound:
        pass the last row's seq to fetch the next *older* page."""
        sql = "SELECT * FROM events WHERE 1=1"
        params: list[object] = []
        if device_id is not None:
            sql += " AND device_id=?"
            params.append(device_id)
        if since_seq is not None:
            sql += " AND seq < ?"
            params.append(since_seq)
        sql += " ORDER BY seq DESC LIMIT ?"
        params.append(limit)
        rows = self._conn.execute(sql, params).fetchall()
        return [_row_to_record(row) for row in rows]

    def time_syncs(self, boot_id: int, *, device_id: str | None = None) -> list[TimeSyncPoint]:
        """All TimeSync events of one boot (unknown unix_s included — the domain rule
        filters them; never pre-filtered away so the caller can see what exists)."""
        sql = "SELECT boot_id, t_start_ms, t_start_unix FROM events WHERE type=? AND boot_id=?"
        params: list[object] = [EVENT_TYPE_TIME_SYNC, boot_id]
        if device_id is not None:
            sql += " AND device_id=?"
            params.append(device_id)
        rows = self._conn.execute(sql, params).fetchall()
        return [
            TimeSyncPoint(
                boot_id=int(row["boot_id"]),
                t_mono_ms=int(row["t_start_ms"]),
                unix_s=int(row["t_start_unix"]),
            )
            for row in rows
        ]

    def event_count(self, device_id: str | None = None) -> int:
        """Row count (tests + CLI progress display)."""
        if device_id is None:
            row = self._conn.execute("SELECT COUNT(*) AS n FROM events").fetchone()
        else:
            row = self._conn.execute(
                "SELECT COUNT(*) AS n FROM events WHERE device_id=?", (device_id,)
            ).fetchone()
        return int(row["n"])

    # --- internals ----------------------------------------------------------

    def _advance_sync_state(self, device_id: str) -> None:
        """high_water = max stored seq; monotonically non-decreasing by construction
        (MAX() of old value and current store head — never moves backward, SYNC §4.3)."""
        row = self._conn.execute(
            "SELECT COALESCE(MAX(seq), 0) AS head FROM events WHERE device_id=?",
            (device_id,),
        ).fetchone()
        head = int(row["head"])
        self._conn.execute(
            "INSERT INTO sync_state(device_id, high_water) VALUES(?, ?) "
            "ON CONFLICT(device_id) DO UPDATE SET "
            "high_water = MAX(sync_state.high_water, excluded.high_water)",
            (device_id, head),
        )

    def _recompute_wall_times(self, device_id: str) -> None:
        """Resolve rows still at wall_unix=0 (t_start_unix=0) against their boot's
        TimeSync events; a row with no usable TimeSync stays 0 = unknown, flagged
        in the UI and never fabricated (SYNC §4.5 step 3).

        A resolution that *definitively* lands on 0 (usable TimeSync, math result 0)
        collides with the unknown sentinel: it is marked `wall_attempted` and never
        re-tried, so it is not resolved again on every later upsert (audit
        F21-AUD-04). Still never fabricated — the stored value stays 0."""
        unresolved = self._conn.execute(
            "SELECT device_id, seq, boot_id, t_start_ms, t_start_unix FROM events "
            "WHERE device_id=? AND wall_unix=0 AND t_start_unix=0 AND wall_attempted=0",
            (device_id,),
        ).fetchall()
        if not unresolved:
            return
        cache: dict[int, list[TimeSyncPoint]] = {}
        for row in unresolved:
            boot_id = int(row["boot_id"])
            if boot_id not in cache:
                cache[boot_id] = self.time_syncs(boot_id, device_id=device_id)
            wall = resolve_wall_unix(
                t_start_ms=int(row["t_start_ms"]),
                t_start_unix=int(row["t_start_unix"]),
                boot_id=boot_id,
                timesyncs=cache[boot_id],
            )
            if wall != 0:
                self._conn.execute(
                    "UPDATE events SET wall_unix=? WHERE device_id=? AND seq=?",
                    (wall, row["device_id"], row["seq"]),
                )
            elif any(point.unix_s != 0 for point in cache[boot_id]):
                # Usable TimeSync existed yet the math returned exactly 0: a
                # definitive answer that collides with "unknown" — mark attempted
                # so the retry loop terminates (audit F21-AUD-04); value stays 0.
                self._conn.execute(
                    "UPDATE events SET wall_attempted=1 WHERE device_id=? AND seq=?",
                    (row["device_id"], row["seq"]),
                )


def _row_to_record(row: sqlite3.Row) -> EventRecord:
    return EventRecord(
        t_start_ms=int(row["t_start_ms"]),
        seq=int(row["seq"]),
        boot_id=int(row["boot_id"]),
        duration_ms=int(row["duration_ms"]),
        t_start_unix=int(row["t_start_unix"]),
        confidence=float(row["confidence"]),
        reasons=int(row["reasons"]),
        payload=bytes(row["payload"]),
        quality=int(row["quality"]),
        type=int(row["type"]),
        label=int(row["label"]),
        context=int(row["context"]),
        alertness_at_start=int(row["alertness_at_start"]),
        detector_id=int(row["detector_id"]),
        schema_version=int(row["schema_version"]),
        raw=bytes(row["raw"]),
        wall_unix=int(row["wall_unix"]),
    )
