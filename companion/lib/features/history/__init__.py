"""F16 companion half — sleep/nap history over the local synced DB (spec §15).

Read-only view: full recompute from F21's `EventRepository` (D12/D14); no schema,
no writer, no duplicated detection logic — the events say what happened, this
package only attributes and renders it. Unknown wall time stays a per-boot session
summary; nothing is ever fabricated (spec §10).

Stable entry seam for the integrator's P4' composition (plan §15.3; app.py and
README are never edited by F16):

    build_history_view_model(repository, *, device_id=None, days=...)
        -> HistoryViewModel     # view() -> list[DayView], render-ready rows
    main(argv) -> int           # console entrypoint, opens the local DB read-only
"""

from __future__ import annotations

import pathlib
import sys
import types

# Source-layout bootstrap - the same guarded `companion` -> ./lib mapping as
# tests/conftest.py and F21's tests/conftest.py. pytest's prepend import mode
# walks up into this package as `lib.features.history` (tests/ became a package
# to fix audit F16-AUD-1), executing this __init__ *before* any conftest body
# could map the package, so the package maps it itself. The installed layout
# already has `companion` imported with a matching __path__ - the guard keeps it.
_LIB = pathlib.Path(__file__).resolve().parents[2]
_existing = sys.modules.get("companion")
if _existing is None or list(getattr(_existing, "__path__", [])) != [str(_LIB)]:
    _pkg = types.ModuleType("companion")
    _pkg.__path__ = [str(_LIB)]  # namespace-style package rooted at the source tree
    _pkg.__doc__ = "anarolepsy companion package (source layout, used by history)"
    sys.modules["companion"] = _pkg

from companion.features.history.aggregate import (  # noqa: E402
    DayFacts,
    EpisodeFacts,
    NapFacts,
    SleepPayload,
    aggregate,
    decode_sleep_payload,
)
from companion.features.history.constants import DEFAULT_HISTORY_DAYS  # noqa: E402
from companion.features.history.view_model import (  # noqa: E402
    DayView,
    EpisodeView,
    HistoryViewModel,
    NapView,
    build_history_view_model,
    main,
)

__all__ = [
    "DEFAULT_HISTORY_DAYS",
    "DayFacts",
    "DayView",
    "EpisodeFacts",
    "EpisodeView",
    "HistoryViewModel",
    "NapFacts",
    "NapView",
    "SleepPayload",
    "aggregate",
    "build_history_view_model",
    "decode_sleep_payload",
    "main",
]
