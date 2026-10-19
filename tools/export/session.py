"""Read + validate a recorded session directory into an in-memory Session.

F28 D14: the writer consumes an in-memory ``Session``, never paths or DB
internals — a future F21 SQLite source is a second ``load_*`` function that
produces the same object and the writer does not change. Validation is
exhaustive BEFORE any output is written (validate-then-write): a truncated
or drifted source CSV fails here, so a failed export never leaves partial
datasets behind (F28 spec §16 test list 2).
"""

from __future__ import annotations

import csv
import io
import json
from dataclasses import dataclass, field
from pathlib import Path

from export.metadata import SOURCE_COLUMNS

# Only the dataset schema this exporter implements. A session.json with any
# other schema_version is refused with the version printed — same rule (and
# exit code, via the CLI) as tools/replay/compare.py. Bumping this requires
# a DATASET_FORMAT.md schema bump first (the contract is the source of
# truth, never this tool).
# [DERIVED] must equal kSessionSchemaVersion / DATASET_FORMAT.md §4.
DATASET_FORMAT_VERSION_SUPPORTED = 1

# Session CSV sources, in export order. Expected-but-absent files are
# recorded in the manifest as absent — never fabricated as empty (a
# raw_tee:false session has no raw streams and that is honest data, §3).
# [DERIVED] DATASET_FORMAT.md §3 session directory listing.
CSV_SOURCES = ("raw_imu", "raw_ppg", "features", "events", "markers")

# labels.csv (F14 reviewer workflow) has no §3 column contract yet — it is
# exported iff present, verbatim, with #META columns read from its own
# header (no invented types). Recorded as follow-up (c), spec §16.
# [DERIVED] research-data tooling guidance, DATASET_FORMAT.md §3.
OPTIONAL_SOURCES = ("labels",)


class SessionError(ValueError):
    """Any validation failure on a session directory (CLI exit 4)."""


class SchemaMismatch(SessionError):
    """session.json declares a schema_version this exporter does not implement."""


@dataclass
class SourceTable:
    """One source CSV: validated header columns + verbatim data rows."""

    name: str
    columns: list[str]
    rows: list[str] = field(default_factory=list)

    @property
    def file_name(self) -> str:
        return f"{self.name}.csv"


@dataclass
class Session:
    """An in-memory, validated session (D14 seam: the writer's only input)."""

    directory: Path
    metadata: dict[str, object]
    tables: dict[str, SourceTable]

    @property
    def session_id(self) -> str:
        return str(self.metadata["session_id"])


def _read_lines(path: Path) -> list[str]:
    """File content as lines, \\r stripped, trailing empty line dropped."""
    text = path.read_text(encoding="utf-8")
    lines = text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    return [line.rstrip("\r") for line in lines]


def _load_table(directory: Path, name: str, expected: list[str] | None) -> SourceTable:
    path = directory / f"{name}.csv"
    lines = _read_lines(path)
    if not lines:
        raise SessionError(f"{path.name}: file is empty (no header row)")
    header = next(csv.reader(io.StringIO(lines[0])))
    if expected is not None and header != expected:
        raise SessionError(
            f"{path.name}: header columns drifted from DATASET_FORMAT.md §3 — "
            f"got {header}, expected {expected}"
        )
    table = SourceTable(name=name, columns=header)
    for number, line in enumerate(lines[1:], start=2):
        fields = next(csv.reader(io.StringIO(line)))
        if len(fields) != len(header):
            raise SessionError(
                f"{path.name}:{number}: truncated row has {len(fields)} fields, "
                f"header has {len(header)} — refusing to export a partial dataset"
            )
        table.rows.append(line)
    return table


def load_session(directory: Path | str) -> Session:
    """Read + validate one session directory. Raises SessionError/SchemaMismatch."""
    directory = Path(directory)
    session_json = directory / "session.json"
    if not session_json.exists():
        raise SessionError(f"session.json not found in {directory}")
    try:
        metadata = json.loads(session_json.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise SessionError(f"session.json is not valid JSON: {exc}") from exc
    if not isinstance(metadata, dict):
        raise SessionError("session.json must contain a JSON object")
    schema = metadata.get("schema_version")
    if schema != DATASET_FORMAT_VERSION_SUPPORTED:
        raise SchemaMismatch(
            f"dataset schema {schema}, exporter implements "
            f"{DATASET_FORMAT_VERSION_SUPPORTED}"
        )
    if not isinstance(metadata.get("session_id"), str) or not metadata["session_id"]:
        raise SessionError("session.json has no session_id")

    tables: dict[str, SourceTable] = {}
    for name in CSV_SOURCES + OPTIONAL_SOURCES:
        path = directory / f"{name}.csv"
        if not path.exists():
            continue
        expected = [c.name for c in SOURCE_COLUMNS[name]] if name in SOURCE_COLUMNS else None
        tables[name] = _load_table(directory, name, expected)
    return Session(directory=directory, metadata=metadata, tables=tables)
