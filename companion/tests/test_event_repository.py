"""§9 test 6 — EventRepository: idempotent (device_id, seq) upsert, monotonic
high_water surviving reopen (SYNC §4.4/§4.6), seq DESC timeline, upsert counts,
TimeSync-driven wall-time recompute (D5/D6)."""

from __future__ import annotations

import sqlite3

import pytest
from companion.data import db
from companion.data.event_repository import EventRepository
from vectors import make_event_bytes


def test_same_device_seq_upserts_once(repo):
    raw = make_event_bytes(seq=1)
    assert repo.upsert("dev1", [raw]) == 1
    assert repo.upsert("dev1", [raw]) == 0  # duplicate = no-op (SYNC §4.4)
    assert repo.event_count("dev1") == 1


def test_upsert_count_returns_new_rows(repo):
    records = [make_event_bytes(seq=n) for n in (1, 2, 3)]
    assert repo.upsert("dev1", records) == 3
    assert repo.upsert("dev1", records) == 0  # retransmission cannot duplicate
    assert repo.event_count("dev1") == 3


def test_backward_and_duplicate_seq_are_no_ops(repo):
    assert repo.upsert("dev1", [make_event_bytes(seq=5)]) == 1
    assert repo.upsert("dev1", [make_event_bytes(seq=3)]) == 1  # older but new: stored
    assert repo.upsert("dev1", [make_event_bytes(seq=5), make_event_bytes(seq=3)]) == 0
    assert repo.event_count("dev1") == 2


def test_high_water_is_monotonic_and_survives_reopen(tmp_path):
    path = tmp_path / "hw.sqlite3"
    conn = db.open_db(path)
    repo = EventRepository(conn)
    assert repo.high_water("dev1") == 0
    repo.upsert("dev1", [make_event_bytes(seq=7), make_event_bytes(seq=2)])
    assert repo.high_water("dev1") == 7
    repo.upsert("dev1", [make_event_bytes(seq=4)])  # older insert does not move it back
    assert repo.high_water("dev1") == 7
    conn.close()
    conn2 = db.open_db(path)  # companion reboot: hw is the resume point (SYNC §4.6)
    try:
        repo2 = EventRepository(conn2)
        assert repo2.high_water("dev1") == 7
    finally:
        conn2.close()


def test_devices_are_isolated(repo):
    repo.upsert("dev1", [make_event_bytes(seq=1)])
    repo.upsert("dev2", [make_event_bytes(seq=9)])
    assert repo.high_water("dev1") == 1
    assert repo.high_water("dev2") == 9
    assert repo.event_count("dev1") == 1


def test_timeline_is_seq_desc_with_limit(repo):
    repo.upsert("dev1", [make_event_bytes(seq=n) for n in (1, 2, 3, 4, 5)])
    rows = repo.timeline(3, device_id="dev1")
    assert [r.seq for r in rows] == [5, 4, 3]
    older = repo.timeline(3, since_seq=3, device_id="dev1")  # exclusive bound: next page
    assert [r.seq for r in older] == [2, 1]


def test_wall_time_set_at_insert_from_t_start_unix(repo):
    repo.upsert("dev1", [make_event_bytes(seq=1, t_start_unix=1_600_000_000)])
    row = repo.timeline(10, device_id="dev1")[0]
    assert row.wall_unix == 1_600_000_000


def test_unresolved_wall_time_is_recomputed_when_timesync_arrives(repo):
    # Event before any TimeSync stays unknown (0); a later TimeSync of the same boot
    # resolves it (SYNC §4.5) — no re-sync needed, the raw blob was already stored.
    repo.upsert("dev1", [make_event_bytes(seq=1, t_start_ms=1_000, t_start_unix=0)])
    assert repo.timeline(10, device_id="dev1")[0].wall_unix == 0
    repo.upsert(
        "dev1",
        [
            make_event_bytes(
                seq=2, type=10, t_start_ms=5_000, t_start_unix=1_600_000_000, boot_id=3
            )
        ],
    )
    rows = {row.seq: row for row in repo.timeline(10, device_id="dev1")}
    assert rows[1].wall_unix == 1_600_000_000 - 4  # event 4 s before the TimeSync
    assert rows[2].wall_unix == 1_600_000_000


def test_zero_wall_resolution_is_attempted_once(repo, monkeypatch):
    # Audit F21-AUD-04: a §4.5 resolution that lands exactly on 0 collides with the
    # unknown sentinel. The row is marked attempted - resolved exactly once, never
    # retried on every later upsert - and the value stays 0 (unknown, never fabricated).
    from companion.data import event_repository

    attempts: list[int] = []
    real_resolve = event_repository.resolve_wall_unix

    def counting_resolve(**kwargs):
        attempts.append(kwargs["t_start_ms"])
        return real_resolve(**kwargs)

    monkeypatch.setattr(event_repository, "resolve_wall_unix", counting_resolve)
    # TimeSync (t=6000 ms, unix 5) + an event 5 s earlier: 5 + (1000-6000)/1000 = 0.
    repo.upsert(
        "dev1",
        [
            make_event_bytes(seq=1, type=10, t_start_ms=6_000, t_start_unix=5, boot_id=3),
            make_event_bytes(seq=2, t_start_ms=1_000, t_start_unix=0, boot_id=3),
        ],
    )
    assert attempts.count(1_000) == 1  # one resolve attempt for the zero row
    rows = {row.seq: row for row in repo.timeline(10, device_id="dev1")}
    assert rows[2].wall_unix == 0  # 0 stays unknown/flagged - never fabricated
    # A later insert re-runs the device's recompute; the zero row must not be retried
    # (reverted fix: attempts would be [1000, 1000, 2000] and this test FAILS).
    repo.upsert(
        "dev1", [make_event_bytes(seq=3, t_start_ms=2_000, t_start_unix=0, boot_id=3)]
    )
    assert attempts.count(1_000) == 1
    assert attempts.count(2_000) == 1  # the new row resolves normally (to 1)
    rows = {row.seq: row for row in repo.timeline(10, device_id="dev1")}
    assert rows[3].wall_unix == 1


def test_schema_v1_database_upgrades_in_place_and_newer_is_refused(tmp_path):
    # Audit F21-AUD-04 (schema v2): a DB written by the v1 app is migrated forward
    # in place (ALTER adds wall_attempted, default 0 = still eligible); a DB from a
    # *newer* app is refused, never downgraded (forward-only, spec §7.3).
    path = tmp_path / "legacy.sqlite3"
    legacy = sqlite3.connect(str(path))
    legacy.executescript(db._SCHEMA_V1)
    legacy.execute(
        "INSERT OR REPLACE INTO schema_meta(key, value) VALUES('schema_version', '1')"
    )
    legacy.commit()
    legacy.close()
    upgraded = db.open_db(path)
    try:
        # WHERE 1=0: SQLite still resolves the column list, so this fails loudly
        # if the migration did not add wall_attempted.
        assert (
            upgraded.execute("SELECT wall_attempted FROM events WHERE 1=0").fetchall()
            == []
        )
        version = upgraded.execute(
            "SELECT value FROM schema_meta WHERE key='schema_version'"
        ).fetchone()
        assert int(version["value"]) == db.SCHEMA_VERSION
    finally:
        upgraded.close()
    future = sqlite3.connect(str(path))
    future.execute(
        "INSERT OR REPLACE INTO schema_meta(key, value) VALUES('schema_version', ?)",
        (str(db.SCHEMA_VERSION + 1),),
    )
    future.commit()
    future.close()
    with pytest.raises(RuntimeError):
        db.open_db(path)  # newer DB: honest refusal, no downgrade (spec §10)


def test_time_syncs_query_returns_boot_points(repo):
    repo.upsert(
        "dev1",
        [
            make_event_bytes(seq=1, type=10, t_start_ms=5_000, t_start_unix=1_600_000_000),
            make_event_bytes(seq=2, type=10, t_start_ms=9_000, t_start_unix=0),
            make_event_bytes(seq=3, type=1, t_start_ms=1_000, t_start_unix=1),
        ],
    )
    points = repo.time_syncs(3)
    assert [(p.t_mono_ms, p.unix_s) for p in points] == [(5_000, 1_600_000_000), (9_000, 0)]
    assert repo.time_syncs(99) == []


def test_record_device_upserts_identity(repo):
    from companion.data.models import DeviceInfo

    repo.record_device(DeviceInfo(device_id="dev1", fw="fw1", hw="hw1", sw="sw1"), 100)
    repo.record_device(DeviceInfo(device_id="dev1", fw="fw2", hw="hw1", sw="sw1"), 200)
    row = repo._conn.execute("SELECT * FROM devices WHERE device_id='dev1'").fetchone()
    assert row["fw"] == "fw2"
    assert row["last_seen_unix"] == 200
