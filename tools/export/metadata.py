"""#META row model for exported CSVs (F28 spec §16, decisions D15/D16).

Every exported file starts with #META rows: the export-envelope version, the
session identity (so a single CSV stays self-describing even if separated
from its manifest) and one ``column`` definition per data column. Data rows
follow verbatim, header line first.

The prefix starts with ``#`` so ``pandas.read_csv(..., comment="#")`` skips
the metadata without special-casing, and stays distinct from the device's
``#HDR`` so an exported file can never be mistaken for a capture stream
(host side of the D6 prefix rule).

Column definitions are pinned to ``DATASET_FORMAT.md`` §3 by
``tests/test_metadata_rows.py`` in BOTH directions: a doc column missing
from the export fails, an exported column missing from the doc fails.
"""

from __future__ import annotations

# Envelope version of the #META rows and export_manifest.json. Independent
# of the dataset schema_version (kSessionSchemaVersion): a bump here means
# an ADDITIVE metadata change only (new #META kinds or manifest fields),
# never a data-row or data-column change.
# [DERIVED] F28 spec §16 constants table.
EXPORT_SCHEMA_VERSION = 1

# Row prefix for every metadata line. Starts with '#' (comment-skip claim,
# pinned by test_meta_prefix_starts_with_hash) and differs from the device's
# '#HDR' so exported metadata can never parse as a research row.
# [DERIVED] F28 spec §16 constants table, host side of the D6 prefix rule.
META_ROW_PREFIX = "#META"

# Scalar session.json fields repeated into every exported CSV. A key absent
# from session.json is written as an EMPTY cell, never as 0 — an absent
# value must stay distinguishable from a real zero (DATASET_FORMAT.md §3).
# [DERIVED] F28 D15: one file = self-describing slice of the session.
SESSION_META_KEYS = (
    "session_id",
    "schema_version",
    "boot_id",
    "firmware_version",
    "started_utc",
    "ended_utc",
    "end_reason",
    "raw_tee",
    "raw_drops",
    "imu_drops",
    "ppg_drops",
    "line_drops",
    "feature_missed",
    "malformed_rows",
)


class ColumnDef:
    """One exported column: name (contract §3), unit and storage type."""

    __slots__ = ("name", "type", "unit")

    def __init__(self, name: str, unit: str, type: str) -> None:
        self.name = name
        self.unit = unit
        self.type = type


# Per-source column definitions, in file order. Names must equal the §3
# per-file column lists (drift-guarded both directions); unit/type document
# the §2.3 number formats for consumers that read only the #META rows.
SOURCE_COLUMNS: dict[str, list[ColumnDef]] = {
    "raw_imu": [
        ColumnDef("t_ms", "ms", "uint64"),
        ColumnDef("boot_id", "-", "uint32"),
        ColumnDef("ax_g", "g", "float"),
        ColumnDef("ay_g", "g", "float"),
        ColumnDef("az_g", "g", "float"),
        ColumnDef("gx_dps", "dps", "float"),
        ColumnDef("gy_dps", "dps", "float"),
        ColumnDef("gz_dps", "dps", "float"),
    ],
    "raw_ppg": [
        ColumnDef("t_ms", "ms", "uint64"),
        ColumnDef("boot_id", "-", "uint32"),
        ColumnDef("red", "count", "uint32"),
        ColumnDef("ir", "count", "uint32"),
    ],
    "features": [
        ColumnDef("t_ms", "ms", "uint64"),
        ColumnDef("boot_id", "-", "uint32"),
        ColumnDef("kind", "-", "enum"),
        ColumnDef("quality", "-", "enum"),
        ColumnDef("enmo_g", "g", "float"),
        ColumnDef("mag_std_g", "g", "float"),
        ColumnDef("jerk_rms_g_per_s", "g/s", "float"),
        ColumnDef("gyro_rms_dps", "dps", "float"),
        ColumnDef("gravity_tilt_deg", "deg", "float"),
        ColumnDef("dominant_freq_hz", "Hz", "float"),
        ColumnDef("stillness_ms", "ms", "uint32"),
        ColumnDef("hr_valid", "-", "bool"),
        ColumnDef("hr_bpm", "bpm", "float"),
        ColumnDef("hr_confidence", "-", "float"),
        ColumnDef("perfusion_index", "-", "float"),
        ColumnDef("ir_dc_level", "count", "float"),
        ColumnDef("sqi", "-", "float"),
        ColumnDef("motion_corrupted", "-", "bool"),
        ColumnDef("hrv_valid", "-", "bool"),
        ColumnDef("rmssd_ms", "ms", "float"),
        ColumnDef("activity", "-", "enum"),
        ColumnDef("confidence", "-", "float"),
    ],
    "events": [
        ColumnDef("seq", "-", "uint32"),
        ColumnDef("t_start_ms", "ms", "uint64"),
        ColumnDef("boot_id", "-", "uint32"),
        ColumnDef("duration_ms", "ms", "uint32"),
        ColumnDef("t_start_unix", "s", "uint32"),
        ColumnDef("confidence", "-", "float"),
        ColumnDef("reasons", "-", "bitmask"),
        ColumnDef("payload_hex", "-", "hex8"),
        ColumnDef("quality_imu", "-", "enum"),
        ColumnDef("quality_ppg", "-", "enum"),
        ColumnDef("type", "-", "enum"),
        ColumnDef("label", "-", "enum"),
        ColumnDef("activity", "-", "enum"),
        ColumnDef("alertness", "-", "enum"),
        ColumnDef("detector_id", "-", "uint32"),
        ColumnDef("schema_version", "-", "uint32"),
    ],
    "markers": [
        ColumnDef("t_ms", "ms", "uint64"),
        ColumnDef("boot_id", "-", "uint32"),
        ColumnDef("marker", "-", "str"),
        ColumnDef("source", "-", "enum"),
    ],
}


def _cell(value: object) -> str:
    """Session meta value -> one #META CSV cell (commas/newlines neutralised)."""
    if isinstance(value, bool):
        return "1" if value else "0"
    if value is None:
        return ""
    return str(value).replace(",", "_").replace("\n", "_").replace("\r", "_")


def meta_lines(
    source_file: str,
    columns: list[ColumnDef],
    session_meta: dict[str, object],
) -> list[str]:
    """The complete #META block for one exported file (D15/D16 grammar).

    Grammar (CR-022, raised not applied — DATASET_FORMAT.md gains §7 only
    when the integrator accepts it):
        #META,export,<key>,<value>                 envelope facts
        #META,session,<key>,<value>                one per SESSION_META_KEYS
        #META,column,<idx>,<name>,<unit>,<type>    0-based column definitions
    """
    lines = [
        f"{META_ROW_PREFIX},export,export_schema_version,{EXPORT_SCHEMA_VERSION}",
        f"{META_ROW_PREFIX},export,source_file,{source_file}",
    ]
    for key in SESSION_META_KEYS:
        lines.append(f"{META_ROW_PREFIX},session,{key},{_cell(session_meta.get(key))}")
    for idx, column in enumerate(columns):
        lines.append(
            f"{META_ROW_PREFIX},column,{idx},{column.name},{column.unit},{column.type}"
        )
    return lines
