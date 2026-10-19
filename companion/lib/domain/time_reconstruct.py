"""Wall-time reconstruction per SYNC_PROTOCOL.md §4.5 (rule owned by F20, F16 D6).

Pure function over (event fields, TimeSync points of its boot):
  1. `t_start_unix != 0` → that value.
  2. Else the TimeSync T of the same boot_id with unix_s != 0 minimizing
     |t_start_ms - T.t_mono_ms| (tie → the earlier TimeSync); wall =
     T.unix_s + (t_start_ms - T.t_mono_ms)/1000 with signed ms math.
  3. Else 0 = unknown — flagged in the UI, never fabricated.

Handoff note (spec D6): this resolver belongs to F21; if F20's companion half also
ships one, F21 swaps it in through this callable seam — either way this contract
text is the single source.
"""

from __future__ import annotations

from collections.abc import Sequence

from companion.data.models import TimeSyncPoint

# [DERIVED] ms→s conversion (SYNC §4.5 divides by 1000); named per the constants rule.
MS_PER_SECOND = 1000


def _trunc_div(numerator: int, denominator: int) -> int:
    """Integer division truncated toward zero — matches the contract's C-side signed
    math (C++ int division truncates; Python // floors, which would drift negative)."""
    quotient = abs(numerator) // denominator
    return quotient if numerator >= 0 else -quotient


def resolve_wall_unix(
    *,
    t_start_ms: int,
    t_start_unix: int,
    boot_id: int,
    timesyncs: Sequence[TimeSyncPoint],
) -> int:
    """Return the event's wall time in unix seconds; 0 = unknown (flagged, never guessed)."""
    if t_start_unix != 0:
        return t_start_unix
    candidates = [
        point for point in timesyncs if point.boot_id == boot_id and point.unix_s != 0
    ]
    if not candidates:
        return 0
    # Nearest |delta|; tie → the earlier TimeSync (smaller t_mono_ms), per §4.5 step 2.
    best = min(candidates, key=lambda p: (abs(t_start_ms - p.t_mono_ms), p.t_mono_ms))
    delta_ms = t_start_ms - best.t_mono_ms
    return best.unix_s + _trunc_div(delta_ms, MS_PER_SECOND)
