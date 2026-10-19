"""SQLite open + forward-only migrations, schema v2 (spec §7.3 + audit F21-AUD-04).

F21 owns the schema; other companion halves (F15/F16/F20/F22) go through
repositories, never raw SQL. Migrations are numbered and forward-only: a DB written
by a newer app version is refused, never downgraded (honest failure, spec §10).
"""

from __future__ import annotations

import os
import sqlite3
from pathlib import Path

# [DERIVED] Single local-first DB file name (spec §8): one DB under the OS user data
# dir; override with the ANAROLEPSY_COMPANION_DB env var (flag/CI injection path).
DB_FILE_NAME = "companion.sqlite3"

# [HW] Schema version of the DDL below (spec §7.3, frozen at PLAN). Bumped only by
# appending a migration to MIGRATIONS — never by editing an applied one.
# v2 (audit F21-AUD-04): events.wall_attempted marks a row whose §4.5 resolution
# definitively landed on 0 (collides with the unknown sentinel) so it is never
# retried forever; 0 stays unknown/flagged, never fabricated.
SCHEMA_VERSION = 2

ENV_DB_PATH = "ANAROLEPSY_COMPANION_DB"

# Schema v1, verbatim from spec §7.3:
#   events  — decoded columns + raw 48 B blob, PRIMARY KEY(device_id, seq) (D5, SYNC §4.4)
#   sync_state — high_water for companion-reboot resume (SYNC §4.6)
#   devices — DIS identity, last_seen
_SCHEMA_V1 = """
CREATE TABLE IF NOT EXISTS schema_meta(key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE IF NOT EXISTS devices(
    device_id TEXT PRIMARY KEY,
    fw TEXT,
    hw TEXT,
    sw TEXT,
    last_seen_unix INTEGER
);
CREATE TABLE IF NOT EXISTS events(
    device_id TEXT,
    seq INTEGER,
    boot_id INTEGER,
    t_start_ms INTEGER,
    duration_ms INTEGER,
    t_start_unix INTEGER,
    wall_unix INTEGER NOT NULL DEFAULT 0,
    confidence REAL,
    reasons INTEGER,
    payload BLOB,
    quality INTEGER,
    type INTEGER,
    label INTEGER,
    context INTEGER,
    alertness_at_start INTEGER,
    detector_id INTEGER,
    schema_version INTEGER,
    raw BLOB NOT NULL,
    PRIMARY KEY(device_id, seq)
);
CREATE TABLE IF NOT EXISTS sync_state(
    device_id TEXT PRIMARY KEY,
    high_water INTEGER NOT NULL DEFAULT 0
);
"""

# Forward-only, append-only: (version, DDL). Version N runs exactly once, in order.
# v2 (audit F21-AUD-04): bookkeeping column only — no decoded column, raw blob or
# key changes, so v1 rows need no backfill (default 0 = still eligible for resolve).
_SCHEMA_V2 = """
ALTER TABLE events ADD COLUMN wall_attempted INTEGER NOT NULL DEFAULT 0;
"""

MIGRATIONS: list[tuple[int, str]] = [
    (1, _SCHEMA_V1),
    (2, _SCHEMA_V2),
]


def default_db_path() -> Path:
    """Resolve the local-first DB path: env override, else under the OS user data dir."""
    env = os.environ.get(ENV_DB_PATH)
    if env:
        return Path(env)
    base = os.environ.get("APPDATA") or os.environ.get("XDG_DATA_HOME")
    root = Path(base) if base else Path.home() / ".local" / "share"
    return root / "anarolepsy" / DB_FILE_NAME


def open_db(path: Path | str) -> sqlite3.Connection:
    """Open (creating if needed) the companion DB and apply pending migrations.

    Raises sqlite3.Error on locked/corrupt storage — callers report it and stay
    responsive; device-side data is never at risk (spec §10, it only reads).
    """
    db_path = Path(path)
    db_path.parent.mkdir(parents=True, exist_ok=True)  # no-op for a bare file name
    conn = sqlite3.connect(str(db_path))
    conn.row_factory = sqlite3.Row
    try:
        _apply_migrations(conn)
    except Exception:
        conn.close()
        raise
    return conn


def _apply_migrations(conn: sqlite3.Connection) -> None:
    conn.execute(
        "CREATE TABLE IF NOT EXISTS schema_meta(key TEXT PRIMARY KEY, value TEXT)"
    )
    row = conn.execute("SELECT value FROM schema_meta WHERE key='schema_version'").fetchone()
    current = int(row["value"]) if row is not None else 0
    if current > SCHEMA_VERSION:
        raise RuntimeError(
            f"database schema v{current} is newer than this app (v{SCHEMA_VERSION}); "
            "refusing to touch it (forward-only migrations, spec §7.3)"
        )
    for version, ddl in MIGRATIONS:
        if version <= current:
            continue
        conn.executescript(ddl)
        conn.execute(
            "INSERT OR REPLACE INTO schema_meta(key, value) VALUES('schema_version', ?)",
            (str(version),),
        )
        conn.execute(
            "INSERT OR REPLACE INTO schema_meta(key, value) VALUES(?, ?)",
            (f"migration_{version}", "applied"),
        )
        current = version
    conn.commit()
