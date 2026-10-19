"""F22 explainable event information — companion half (spec §6 D6-D9).

Stable entrypoints for the composition root (wired into `app.py` by the
integrator at P4'; this package is never imported from app.py yet):

    from companion.features.explain import build_explanation, ExplanationViewModel, main

- `build_explanation(record)`  pure record -> EventExplanation (no I/O)
- `ExplanationViewModel(repo)` read-only view over the local synced DB
- `main(argv) -> int`          standalone CLI: --seq / --limit / --device / --db

Read-only over F21's EventRepository; detections render as "possible"/"suspected"
with a confidence value; unknown reason bits render as `reason #N`; the catalog
is pinned to `bw/state.h` by `tests/test_catalog_parity.py` (D6/D9). An exact-seq
lookup that is ambiguous across devices surfaces every candidate as
`AmbiguousSeq` instead of picking one (F22-AUD-1). Not a
medical device: this view explains what the device recorded, it never diagnoses.
"""

from __future__ import annotations

import pathlib
import sys
import types

# Source-layout bootstrap - the same guarded `companion` -> ./lib mapping as
# tests/conftest.py and F21's tests/conftest.py. pytest's prepend import mode
# walks up into this package as `lib.features.explain` (tests/ became a package
# to fix audit F22-AUD-4, the same class as F16-AUD-1), executing this __init__
# *before* any conftest body could map the package, so the package maps it
# itself. The installed layout already has `companion` imported with a matching
# __path__ - the guard keeps it.
_LIB = pathlib.Path(__file__).resolve().parents[2]
_existing = sys.modules.get("companion")
if _existing is None or list(getattr(_existing, "__path__", [])) != [str(_LIB)]:
    _pkg = types.ModuleType("companion")
    _pkg.__path__ = [str(_LIB)]  # namespace-style package rooted at the source tree
    _pkg.__doc__ = "anarolepsy companion package (source layout, used by explain)"
    sys.modules["companion"] = _pkg

from companion.features.explain.builder import (  # noqa: E402
    AmbiguousSeq,
    EventExplanation,
    ExplanationViewModel,
    ReasonItem,
    build_explanation,
    main,
    render_ambiguous,
    render_explanation,
)
from companion.features.explain.catalog import (  # noqa: E402
    REASON_CATALOG_VERSION,
    REASON_EXPLANATIONS,
)

__all__ = [
    "REASON_CATALOG_VERSION",
    "REASON_EXPLANATIONS",
    "AmbiguousSeq",
    "EventExplanation",
    "ExplanationViewModel",
    "ReasonItem",
    "build_explanation",
    "main",
    "render_ambiguous",
    "render_explanation",
]
