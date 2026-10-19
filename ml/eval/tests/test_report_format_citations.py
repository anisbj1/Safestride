"""Meta-test: every code id cited by REPORT_FORMAT.md §7 must resolve (audit Low-2).

The drift guard (F1) is only falsifiable while its cited ids stay real: if the
dedicated guard test is deleted, renamed or re-stranded inside another test,
the citation in REPORT_FORMAT §7 must fail loudly instead of rotting silently.
This test parses §7, extracts every ``<file>.py::<name>`` citation, and asserts
each file exists and defines a top-level ``def <name>``.
"""

import re
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[3]
_REPORT_FORMAT = _REPO_ROOT / "docs" / "evaluation" / "REPORT_FORMAT.md"
_TESTS_DIR = _REPO_ROOT / "ml" / "eval" / "tests"

_CITATION_RE = re.compile(r"([\w/]+\.py)::(\w+)")

# §7 must keep citing the drift guard and its fixture writer: deleting the
# citations would re-strand the F1 failure mode, so absence also fails here.
_REQUIRED_CITATIONS = {
    ("ml/eval/tests/fixtures/synthetic.py", "write_canonical_sessions"),
    ("test_end_to_end.py", "test_example_report_matches_committed_files"),
}


def _section_seven_text() -> str:
    text = _REPORT_FORMAT.read_text(encoding="utf-8")
    start = text.index("## 7.")
    rest = text[start + len("## 7.") :]
    end = rest.find("\n## ")
    return rest if end == -1 else rest[:end]


def test_report_format_seven_citations_resolve() -> None:
    citations = _CITATION_RE.findall(_section_seven_text())
    found = set(citations)
    assert _REQUIRED_CITATIONS <= found, (
        "REPORT_FORMAT §7 must cite the drift guard and its fixture writer; "
        f"missing: {_REQUIRED_CITATIONS - found}"
    )
    assert citations, "REPORT_FORMAT §7 cites no code ids at all"
    for file_ref, name in citations:
        # A bare filename (no directories) is cited relative to ml/eval/tests;
        # a path with directories is repo-root-relative, as written in §7.
        path = Path(file_ref)
        file_path = (
            _REPO_ROOT / path
            if path.parent != Path(".")
            else _TESTS_DIR / path
        )
        assert file_path.is_file(), f"REPORT_FORMAT §7 cites missing file {file_ref}"
        source = file_path.read_text(encoding="utf-8")
        assert re.search(rf"^def {re.escape(name)}\b", source, re.M), (
            f"REPORT_FORMAT §7 cites {file_ref}::{name}, but no top-level "
            f"def {name} exists there"
        )
