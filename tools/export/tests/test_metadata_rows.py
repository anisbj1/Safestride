"""#META schema rows: the doc<->export drift guard (spec §16 test list 1, H9).

The column definitions the exporter annotates every file with live in
``DATASET_FORMAT.md`` §3. This suite parses §3 (pinned to exact markers, the
way ``test_decode_events.py`` pins §5) and asserts BOTH directions against
``export.metadata.SOURCE_COLUMNS``: a doc column dropped from the export
fails, an exported column missing from the doc fails. The #META row grammar
itself is pinned here until CR-022 adds §7 to the contract doc.
"""

from __future__ import annotations

import re
from pathlib import Path

from export import cli, csv_export, metadata
from export.session import load_session

_DATASET_FORMAT = Path(__file__).resolve().parents[3] / "docs" / "contracts" / "DATASET_FORMAT.md"
_SECTION3_MARKER = "## 3. Session directory"
_SECTION21_MARKER = "### 2.1 Row types"


def _doc_text() -> str:
    text = _DATASET_FORMAT.read_text(encoding="utf-8")
    assert text.count(_SECTION3_MARKER) == 1, "DATASET_FORMAT §3 heading moved: update this guard"
    return text


def _doc_file_columns() -> dict[str, list[str]]:
    """Per-file column lists as written in §3 (directory block + code block)."""
    text = _doc_text()
    section = text.split(_SECTION3_MARKER, 1)[1].split("## 4.", 1)[0]
    columns: dict[str, list[str]] = {}
    # Directory-block comments: `  raw_imu.csv   # t_ms,boot_id,...`
    for name, comment in re.findall(r"^\s{2}([a-z_]+\.csv)\s{2,}# (.+)$", section, re.MULTILINE):
        base = name.removesuffix(".csv")
        if comment.startswith("union columns") or comment.startswith("one row per Event"):
            continue  # features/events resolve below (their columns live elsewhere)
        # markers.csv carries a trailing "(source ∈ device|host)" note.
        columns[base] = comment.split(" (")[0].split(",")
    # features.csv union-column code block.
    block = re.search(r"`features\.csv` columns.*?```\n(.*?)```", section, re.DOTALL)
    assert block, "features.csv column block missing from §3"
    columns["features"] = [
        c.strip() for c in block.group(1).replace("\n", "").split(",") if c.strip()
    ]
    # events.csv: "same fields as the E line" -> §2.1 grammar, minus the `E` tag.
    assert text.count(_SECTION21_MARKER) == 1
    grammar = text.split(_SECTION21_MARKER, 1)[1].split("###", 1)[0]
    e_line = re.search(r"^E,(.+)$", grammar, re.MULTILINE)
    assert e_line, "E line missing from §2.1"
    columns["events"] = [field.strip("<>") for field in e_line.group(1).split(",")]
    return columns


def test_meta_column_definitions_equal_the_contract_doc() -> None:
    """Both directions: doc -> export and export -> doc must agree exactly."""
    doc_columns = _doc_file_columns()
    assert set(doc_columns) == set(metadata.SOURCE_COLUMNS), "sources drifted from §3"
    for source, doc in doc_columns.items():
        exported = [column.name for column in metadata.SOURCE_COLUMNS[source]]
        assert exported == doc, f"{source}.csv column drift: export {exported} vs doc {doc}"


def test_meta_row_prefix_starts_with_hash_and_is_not_a_device_header() -> None:
    assert metadata.META_ROW_PREFIX.startswith("#")  # read_csv(comment="#") skips it
    assert metadata.META_ROW_PREFIX != "#HDR"
    assert not metadata.META_ROW_PREFIX.startswith("#HDR")  # never a capture line


def test_every_exported_file_starts_with_meta_rows(fixture_session, out_dir) -> None:
    result = csv_export.write_session_export(load_session(fixture_session), out_dir)
    assert result.exported  # fixture has sources
    for file_name in result.exported:
        lines = (result.out_dir / file_name).read_text(encoding="utf-8").splitlines()
        meta = [line for line in lines if line.startswith(metadata.META_ROW_PREFIX + ",")]
        assert meta, f"{file_name} has no #META rows"
        # The data header line is the first non-#META line.
        header_index = next(i for i, line in enumerate(lines) if not line.startswith("#"))
        assert lines[header_index].startswith("t_ms,") or file_name == "events.csv"
        # Column #META rows, in order, one per header column.
        column_prefix = f"{metadata.META_ROW_PREFIX},column,"
        columns_meta = [line for line in meta if line.startswith(column_prefix)]
        header = lines[header_index].split(",")
        assert len(columns_meta) == len(header)
        for idx, line in enumerate(columns_meta):
            _, kind, position, name, unit, cell_type = line.split(",")
            assert (kind, int(position)) == ("column", idx)
            assert name == header[idx]
            assert unit and cell_type


def test_meta_session_rows_carry_the_loss_counters(fixture_session, out_dir) -> None:
    session = load_session(fixture_session)
    result = csv_export.write_session_export(session, out_dir)
    imu_meta = (result.out_dir / "raw_imu.csv").read_text(encoding="utf-8")
    assert f"{metadata.META_ROW_PREFIX},session,session_id,{session.session_id}" in imu_meta
    assert f"{metadata.META_ROW_PREFIX},session,raw_drops," in imu_meta
    assert f"{metadata.META_ROW_PREFIX},session,malformed_rows," in imu_meta
    version_row = (
        f"{metadata.META_ROW_PREFIX},export,export_schema_version,"
        f"{metadata.EXPORT_SCHEMA_VERSION}"
    )
    assert version_row in imu_meta


def test_export_schema_version_is_independent_of_dataset_schema() -> None:
    # EXPORT_SCHEMA_VERSION pins the #META envelope, not the dataset: the
    # fixture session.json still declares the dataset schema_version=1.
    assert metadata.EXPORT_SCHEMA_VERSION == 1
    assert cli.EXIT_BAD_SESSION == 4
