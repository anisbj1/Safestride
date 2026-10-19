"""Section 7 drift guard: `tools/export` output <-> the normative §7 text.

CR-022 (accepted, applied on master 24e1f88) made §7 of
``DATASET_FORMAT.md`` the contract for the export envelope. This suite
parses §7 the same way suite 1 parses §3 — pinned to exact headings, a doc
reformat fails loudly — and asserts BOTH directions:

- doc -> tool: every ``#META`` kind, session key, column type and manifest
  field named in §7 must be produced by the tool;
- tool -> doc: every kind/key/type/field the tool emits must be named in §7.

H10: mutating §7's grammar or the tool's output alone fails this suite.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

from export import cli, csv_export, metadata
from export.session import load_session

_DATASET_FORMAT = Path(__file__).resolve().parents[3] / "docs" / "contracts" / "DATASET_FORMAT.md"
_SECTION6_ROW = "| 1 | ADD export half 2026-10-17 (CR-022,"
_SECTION7 = "## 7. Export output (host, `tools/export`)"
_SECTION71 = "### 7.1 `#META` rows"
_SECTION72 = "### 7.2 `export_manifest.json`"
_SECTION73 = "### 7.3 Exit-code map and lossy gate"
_SECTION74 = "### 7.4 `EXPORT_SCHEMA_VERSION` semantics"


def _doc_text() -> str:
    text = _DATASET_FORMAT.read_text(encoding="utf-8")
    for marker in (_SECTION7, _SECTION71, _SECTION72, _SECTION73, _SECTION74):
        assert text.count(marker) == 1, f"DATASET_FORMAT marker moved/missing: {marker}"
    return text


def _section(text: str, start: str, end: str) -> str:
    return text.split(start, 1)[1].split(end, 1)[0]


def _grammar_templates(text: str) -> dict[str, list[list[str]]]:
    """§7.1 code block -> {kind: [template field-lists]} (comments stripped)."""
    block = _section(text, _SECTION71, _SECTION72)
    templates: dict[str, list[list[str]]] = {}
    for raw in block.splitlines():
        line = raw.strip()
        if not line.startswith(metadata.META_ROW_PREFIX + ","):
            continue
        template = re.split(r"\s+#", line, maxsplit=1)[0]
        fields = template.split(",")
        templates.setdefault(fields[1], []).append(fields)
    assert templates, "no #META grammar lines parsed from §7.1"
    return templates


def _section_session_keys(text: str) -> list[str]:
    """§7.1 comment continuation -> the normative SESSION_META_KEYS list."""
    block = _section(text, _SECTION71, _SECTION72)
    comments: list[str] = []
    for line in block.splitlines():
        stripped = line.strip()
        if stripped.startswith(metadata.META_ROW_PREFIX + ","):
            parts = re.split(r"\s+#", stripped, maxsplit=1)
            if len(parts) == 2:
                comments.append(parts[1])
        elif stripped.startswith("#"):  # continuation of a comment block
            comments.append(stripped.lstrip("#").strip())
    comment = " ".join(comments)
    match = re.search(r"fields:\s*(.*?)\s*\(absent key", comment, re.DOTALL)
    assert match, "session field list not found in §7.1 comments"
    return [key.strip() for key in match.group(1).split(",")]


def _section_types(text: str) -> list[str]:
    match = re.search(r"`<type>` is one of\s*`([^`]+)`", text)
    assert match, "type vocabulary not found in §7.1"
    return match.group(1).split("|")


def _depth0_backticks(text: str) -> list[str]:
    tokens: list[str] = []
    depth = 0
    index = 0
    while index < len(text):
        char = text[index]
        if char == "(":
            depth += 1
        elif char == ")":
            depth = max(0, depth - 1)
        elif char == "`":
            end = text.find("`", index + 1)
            if end == -1:
                break
            if depth == 0:
                tokens.append(text[index + 1 : end])
            index = end
        index += 1
    return tokens


def _section_manifest_fields(text: str) -> list[str]:
    """§7.2 paragraph -> top-level manifest field names (paren notes skipped)."""
    paragraph = _section(text, _SECTION72, _SECTION73).strip()
    return _depth0_backticks(paragraph)


def _section_exit_codes(text: str) -> dict[str, int]:
    paragraph = _section(text, _SECTION73, _SECTION74)
    codes = {
        "ok": int(re.search(r"`(\d)` ok", paragraph).group(1)),
        "lossy": int(re.search(r"`(\d)` lossy recording refused", paragraph).group(1)),
        "bad": int(re.search(r"`(\d)` missing `session.json`", paragraph).group(1)),
        "compare_only": int(re.search(r"`(\d)` stays compare-only", paragraph).group(1)),
    }
    return codes


def _collect(
    fixture_session: Path, out_dir: Path
) -> tuple[dict[str, list[str]], dict[str, object]]:
    (fixture_session / "labels.csv").write_text(
        "t_ms,boot_id,label\n5000,7,Confirmed\n", encoding="utf-8"
    )
    result = csv_export.write_session_export(load_session(fixture_session), out_dir)
    rows_by_file: dict[str, list[str]] = {}
    for file_name in result.exported:
        rows_by_file[file_name] = [
            line
            for line in (result.out_dir / file_name).read_text(encoding="utf-8").splitlines()
            if line.startswith(metadata.META_ROW_PREFIX + ",")
        ]
    manifest = json.loads(
        (result.out_dir / "export_manifest.json").read_text(encoding="utf-8")
    )
    return rows_by_file, manifest


def test_meta_grammar_kinds_and_arity_match_the_tool(fixture_session, out_dir) -> None:
    text = _doc_text()
    templates = _grammar_templates(text)
    rows_by_file, _ = _collect(fixture_session, out_dir)
    rows = [row for rows in rows_by_file.values() for row in rows]
    emitted_kinds = {row.split(",")[1] for row in rows}
    # doc -> tool: every normative kind is produced; tool -> doc: none is foreign.
    assert set(templates) == emitted_kinds
    arity = {kind: {len(fields) for fields in lines} for kind, lines in templates.items()}
    for row in rows:
        fields = row.split(",")
        assert len(fields) in arity[fields[1]], f"row arity drifts from §7.1: {row}"


def test_session_key_list_matches_the_metadata_model(fixture_session, out_dir) -> None:
    rows_by_file, _ = _collect(fixture_session, out_dir)
    doc_keys = _section_session_keys(_doc_text())
    assert doc_keys == list(metadata.SESSION_META_KEYS)  # names and order
    # Every exported file repeats exactly that key list, in order.
    for file_name, rows in rows_by_file.items():
        session_keys = [row.split(",")[2] for row in rows if ",session," in row]
        assert session_keys == doc_keys, f"{file_name} session-key drift from §7.1"


def test_type_vocabulary_matches_the_tool_in_both_directions(fixture_session, out_dir) -> None:
    rows_by_file, _ = _collect(fixture_session, out_dir)
    doc_types = set(_section_types(_doc_text()))
    # #META,column rows are: prefix,kind,idx,name,unit,type -> type is index 5.
    emitted_types = {
        row.split(",")[5]
        for rows in rows_by_file.values()
        for row in rows
        if row.split(",")[1] == "column"
    }
    assert emitted_types <= doc_types, "tool emits a column type §7.1 does not name"
    assert doc_types <= emitted_types, "§7.1 names a type the tool never produces"


def test_meta_prefix_matches_the_section7_grammar() -> None:
    text = _doc_text()
    block = _section(text, _SECTION71, _SECTION72)
    grammar_lines = [
        line.strip() for line in block.splitlines() if line.strip().startswith("#META,")
    ]
    assert grammar_lines, "no grammar lines to check the prefix against"
    # doc -> tool: the normative prefix literal equals the tool constant.
    assert all(line.startswith(metadata.META_ROW_PREFIX + ",") for line in grammar_lines)
    # Comment-skip claim, restated in §7.1 (whitespace-normalised: the doc
    # wraps the sentence across lines).
    normalised = " ".join(block.split())
    assert "Rows start with `#`" in normalised


def test_manifest_fields_match_section7_in_both_directions(fixture_session, out_dir) -> None:
    _, manifest = _collect(fixture_session, out_dir)
    doc_fields = _section_manifest_fields(_doc_text())
    assert doc_fields == list(manifest), "manifest fields drifted from §7.2"


def test_exit_code_map_matches_section7() -> None:
    codes = _section_exit_codes(_doc_text())
    assert codes["ok"] == cli.EXIT_OK == 0
    assert codes["lossy"] == cli.EXIT_REFUSED_LOSSY == 2
    assert codes["bad"] == cli.EXIT_BAD_SESSION == 4
    # 3 stays compare-only: export never claims a comparison.
    assert codes["compare_only"] == 3
    assert 3 not in {cli.EXIT_OK, cli.EXIT_REFUSED_LOSSY, cli.EXIT_BAD_SESSION}
    assert "validate-then-write" in _doc_text()


def test_export_schema_version_semantics(fixture_session, out_dir) -> None:
    rows_by_file, manifest = _collect(fixture_session, out_dir)
    version_rows = {
        row
        for rows in rows_by_file.values()
        for row in rows
        if ",export,export_schema_version," in row
    }
    expected = {
        f"{metadata.META_ROW_PREFIX},export,export_schema_version,"
        f"{metadata.EXPORT_SCHEMA_VERSION}"
    }
    # Every exported file carries exactly the manifest's envelope version.
    assert version_rows == expected
    assert manifest["export_schema_version"] == metadata.EXPORT_SCHEMA_VERSION
    text = _doc_text()
    # §7.4: envelope version, independent of the dataset schema_version.
    section74 = text.split(_SECTION74, 1)[1]
    assert "independent of the dataset" in section74
    assert _SECTION6_ROW in text  # §6 version-history row for the export half
