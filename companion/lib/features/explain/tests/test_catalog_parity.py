"""Catalog parity (spec §9 items 17-21, D6/D9): the companion catalog is pinned to
the contract, never to a stale hand copy.

Parses `enum class ReasonCode` out of `bw/state.h` at run time (append-only enum,
source order = numeric values — the same technique as the firmware's D2 test) and
asserts: catalog keys == parsed count and order, names == F21's `wording.REASON_NAMES`,
`REASON_CATALOG_VERSION` == the firmware's `kReasonCatalogVersion` parsed from
`reason_catalog.h`. A missing header FAILS loudly (a skipped check is a failed check).
"""

from __future__ import annotations

import pathlib
import re

import pytest
from companion.domain import wording
from companion.features.explain import catalog

# [DERIVED] Locate the firmware headers from the test file and the repo root
# (spec §8): tests run from `companion/`, so CWD-relative alone would break;
# first hit wins, none => FAIL (D2 discipline).
_REPO_ROOT = pathlib.Path(__file__).resolve().parents[5]
REASON_HEADER_CANDIDATES = (
    "../../../firmware/lib/bw_core/include/bw/state.h",
    "firmware/lib/bw_core/include/bw/state.h",
    "../firmware/lib/bw_core/include/bw/state.h",
)
CATALOG_HEADER_CANDIDATES = tuple(
    path.replace("bw_core/include/bw/state.h", "bw_explain/include/bw/explain/reason_catalog.h")
    for path in REASON_HEADER_CANDIDATES
)


def _locate(candidates: tuple[str, ...]) -> pathlib.Path:
    tried: list[str] = []
    for candidate in candidates:
        for base in (_REPO_ROOT, pathlib.Path()):
            path = (base / candidate).resolve()
            tried.append(str(path))
            if path.is_file():
                return path
    pytest.fail(
        "F22 parity test cannot locate the firmware header; a skipped check is a "
        f"failed check (D2). Tried: {tried}",
        pytrace=False,
    )


def _parse_enumerators(header_text: str, enum_name: str) -> list[str]:
    match = re.search(rf"enum class {enum_name} : uint8_t \{{(.*?)\}};", header_text, re.S)
    assert match is not None, f"enum class {enum_name} block not found in header"
    names: list[str] = []
    for line in match.group(1).splitlines():
        code = line.split("//", 1)[0].strip().rstrip(",").strip()
        if code:
            names.append(code)
    return names


def test_catalog_covers_every_reason_code_in_order():
    """Spec §9 item 17: appending a firmware code without a companion sentence fails here."""
    header = _locate(REASON_HEADER_CANDIDATES).read_text(encoding="utf-8")
    names = _parse_enumerators(header, "ReasonCode")
    assert len(catalog.REASON_EXPLANATIONS) == len(names)
    assert list(catalog.REASON_EXPLANATIONS) == list(range(len(names)))
    assert all(catalog.REASON_EXPLANATIONS[bit] for bit in range(len(names)))


def test_names_lockstep_with_f21_wording():
    """Spec §9 item 18: parsed enum names == F21's REASON_NAMES (one name list, no second copy)."""
    header = _locate(REASON_HEADER_CANDIDATES).read_text(encoding="utf-8")
    names = tuple(_parse_enumerators(header, "ReasonCode"))
    assert tuple(wording.REASON_NAMES) == names


def test_catalog_version_pinned_to_firmware():
    """Spec §9 item 19 (D9): the Python twin constant equals the firmware value."""
    header = _locate(CATALOG_HEADER_CANDIDATES).read_text(encoding="utf-8")
    match = re.search(r"constexpr uint16_t kReasonCatalogVersion = (\d+);", header)
    assert match is not None, "kReasonCatalogVersion not found in reason_catalog.h"
    assert catalog.REASON_CATALOG_VERSION == int(match.group(1))


def test_catalog_sentences_wording():
    """Spec §9 item 20: non-empty, bounded, ASCII printable, no banned diagnostic terms."""
    for bit, sentence in catalog.REASON_EXPLANATIONS.items():
        assert sentence, f"empty sentence for bit {bit}"
        assert len(sentence) <= catalog.EXPLAIN_MAX_SENTENCE_CHARS, f"bit {bit} too long"
        assert sentence.isascii(), f"bit {bit} not ASCII: {sentence!r}"
        lowered = sentence.lower()
        for term in wording.BANNED_DIAGNOSTIC_TERMS:
            assert term not in lowered, f"bit {bit} uses banned term {term!r}"


def test_appended_code_would_fail_the_count_equality():
    """Spec §9 item 21 (fault injection): a doctored in-memory enum block with one
    appended enumerator (exactly what a future append-only CR looks like) makes the
    item-17 equality fail; no repo file is edited."""
    header = _locate(REASON_HEADER_CANDIDATES).read_text(encoding="utf-8")
    doctored = header.replace(
        "ShortMotionBurst,  // F09", "ShortMotionBurst,\n    FutureAppendedCode,  // F09"
    )
    names = _parse_enumerators(doctored, "ReasonCode")
    assert len(names) == len(catalog.REASON_EXPLANATIONS) + 1
    assert len(names) != len(catalog.REASON_EXPLANATIONS)
