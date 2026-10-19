"""The export writer: #META rows + verbatim data rows + export manifest.

F28 D15: one CSV per source, data rows byte-identical to the session dir
(timestamps never rewritten, booleans still ``0``/``1``), annotated with the
#META schema block (``metadata.py``) on top, plus ``export_manifest.json``
(session metadata, counters, exported file list, absent-file flags).
Alignment / resampling is deliberately NOT done here — it stays visible in
the consuming ``ml/`` code, never a silent transform in the export layer.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path

from export.metadata import EXPORT_SCHEMA_VERSION, SOURCE_COLUMNS, ColumnDef, meta_lines
from export.session import CSV_SOURCES, OPTIONAL_SOURCES, Session, SourceTable

MANIFEST_NAME = "export_manifest.json"

# Columns read from the source file's own header (no invented types) for
# sources without a DATASET_FORMAT.md §3 column contract (labels.csv).
# [DERIVED] F28 D15 + spec §16 follow-up (c).
def _header_columns(table: SourceTable) -> list[ColumnDef]:
    return [ColumnDef(name, "-", "verbatim") for name in table.columns]


@dataclass
class ExportResult:
    """What one session export produced (CLI summary + tests)."""

    session_id: str
    out_dir: Path
    exported: list[str]
    absent: list[str]


def write_session_export(session: Session, out_dir: Path | str) -> ExportResult:
    """Write one validated session into ``out_dir/<session_id>/`` (D15)."""
    out_root = Path(out_dir)
    target = out_root / session.session_id
    target.mkdir(parents=True, exist_ok=True)

    exported: list[str] = []
    absent: list[str] = []
    for name in CSV_SOURCES + OPTIONAL_SOURCES:
        table = session.tables.get(name)
        file_name = f"{name}.csv"
        if table is None:
            if name in CSV_SOURCES:
                absent.append(file_name)
            continue
        columns = SOURCE_COLUMNS.get(name) or _header_columns(table)
        lines = meta_lines(file_name, columns, session.metadata)
        lines.append(",".join(table.columns))
        lines.extend(table.rows)
        (target / file_name).write_text("\n".join(lines) + "\n", encoding="utf-8")
        exported.append(file_name)

    manifest = {
        "export_schema_version": EXPORT_SCHEMA_VERSION,
        "session_id": session.session_id,
        "session": session.metadata,
        "exported_files": exported,
        # A source CSV that this session never had (e.g. raw_ppg.csv with
        # raw_tee=false) is recorded here and stays absent on disk — an
        # empty file would fabricate data the device never produced.
        "absent_files": absent,
    }
    (target / MANIFEST_NAME).write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    return ExportResult(
        session_id=session.session_id,
        out_dir=target,
        exported=exported,
        absent=absent,
    )
