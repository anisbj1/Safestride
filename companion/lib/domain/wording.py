"""Presentation wording rules (spec §5, companion specification behavior rules).

- Detections read "possible"/"suspected" **with a confidence value** (spec §14 DoD).
- Unknown enum values render `unknown (#n)`; unknown reason bits render `reason #N`
  (BLE_GATT §1, SYNC §4.7: never coerced, never hidden).
- Cached values always carry their age; stale values are labeled (§10).
- No diagnostic/medical claim wording anywhere: the project is not a medical device
  and never diagnoses (docs/00_PROJECT_CONTEXT.md).
"""

from __future__ import annotations

from datetime import UTC, datetime

# [EXPERIMENTAL] Beyond this age a cached value is labeled "(stale)" (spec §8):
# values are always shown with age anyway; the label starts at 5 s, tune later.
STALE_LABEL_MS = 5000

# [DERIVED] Age-label step for minutes (60 s), shown after the seconds range.
SECONDS_PER_MINUTE = 60
# [DERIVED] Age-label step for hours (60 min), shown after the minutes range.
MINUTES_PER_HOUR = 60

# --- enum display names ([HW]: data_decision.md §4.5 / BLE_GATT §4.1, append-only) ---

ALERTNESS_NAMES = {
    0: "Unknown",
    1: "NotWorn",
    2: "Awake",
    3: "Drowsy",
    4: "HighDrowsinessRisk",
    5: "PossibleSleep",
}
WEAR_NAMES = {0: "Uncertain", 1: "Worn", 2: "NotWorn"}
ACTIVITY_NAMES = {
    0: "Unknown",
    1: "Stationary",
    2: "Sitting",
    3: "Standing",
    4: "Walking",
    5: "Running",
    6: "LyingResting",
    7: "InactiveProlonged",
}
MODE_NAMES = {0: "Day", 1: "Night", 2: "Nap", 3: "Calibration", 4: "Research"}
POWER_NAMES = {0: "Normal", 1: "LowPower"}

# EventType (data_decision.md §4.5, append-only enum, indices 0..11).
EVENT_TYPE_NAMES = {
    0: "SleepEpisode",
    1: "DrowsinessEvent",
    2: "SuspectedCataplexy",
    3: "UserMarked",
    4: "AlertRaised",
    5: "SensorFault",
    6: "DeviceNotWorn",
    7: "BatteryWarning",
    8: "NapPlanned",
    9: "NapCompleted",
    10: "TimeSync",
    11: "Boot",
}

# ReasonCode catalog known so far (data_decision.md §4.5, append-only, bits 0-17).
# Anything beyond this list renders as "reason #N" — surfaced, never hidden.
REASON_NAMES = (
    "SustainedLowMotion",
    "ProlongedInactivity",
    "HrBelowBaseline",
    "HrTrendChange",
    "AbruptMotionDrop",
    "TimeOfDayTypicalDrowsy",
    "MatchesPastConfirmedEvent",
    "UserMarked",
    "PlannedNap",
    "LowSignalQuality",
    "SensorUnavailable",
    "DeviceNotWorn",
    "SustainedLowAlertnessProbability",
    "SustainedWakefulnessProbability",
    "InsufficientEvidence",
    "PossibleFall",
    "PostureChange",
    "ShortMotionBurst",
)

# [HW] Detection-type wording (spec §14 / companion specification): EventType index →
# the only labels allowed for detections; each is rendered with its confidence.
DETECTION_WORDS = {
    0: "possible sleep episode",
    1: "possible drowsiness",
    2: "suspected cataplexy-like event",
}

# [DERIVED] Terms the rendered UI must never contain (spec §5 "must not": no
# diagnosis wording; detections are possible/suspected + confidence, nothing more).
# Checked by tests over rendered output.
BANNED_DIAGNOSTIC_TERMS = (
    "diagnos",
    "disorder",
    "patient",
    "symptom",
    "disease",
    "clinical",
    "treatment",
    "prescri",
    "narcolepsy",
    "apnea",
)

# [DERIVED] Unknown-enum placeholder pattern (BLE_GATT §1): value shown, never coerced.
UNKNOWN_PREFIX = "unknown (#"
# [DERIVED] Unknown reason-bit placeholder (companion specification): index shown, not hidden.
UNKNOWN_REASON_PREFIX = "reason #"


def format_enum(names: dict[int, str], value: int) -> str:
    """Known value → name; anything else → `unknown (#n)`, never coerced (BLE_GATT §1)."""
    return names.get(value, f"{UNKNOWN_PREFIX}{value})")


def format_confidence(confidence: float) -> str:
    """Confidence 0..1 → two-decimal text; it is a model/heuristic value, never a
    clinical certainty (data_decision.md §4.5)."""
    return f"{confidence:.2f}"


def detection_label(event_type: int, confidence: float) -> str | None:
    """Detection types get 'possible'/'suspected' + confidence (spec §14);
    None for non-detection events, which render by plain type name."""
    word = DETECTION_WORDS.get(event_type)
    if word is None:
        return None
    return f"{word} (confidence {format_confidence(confidence)})"


def format_reasons(reasons: int) -> str:
    """ReasonMask → named reasons; unknown bits become 'reason #N' (never hidden)."""
    if reasons == 0:
        return "none"
    parts = []
    bit = 0
    mask = reasons
    while mask:
        if mask & 1:
            if bit < len(REASON_NAMES):
                parts.append(REASON_NAMES[bit])
            else:
                # "reason #N" — visible, never hidden, never guessed at.
                parts.append(f"{UNKNOWN_REASON_PREFIX}{bit}")
        mask >>= 1
        bit += 1
    return ", ".join(parts)


def age_text(age_ms: int, *, stale_after_ms: int = STALE_LABEL_MS) -> str:
    """Human age for a cached value — always shown (§10); stale past the threshold."""
    age_ms = max(0, age_ms)
    seconds = age_ms // 1000
    if seconds < SECONDS_PER_MINUTE:
        text = f"{seconds}s ago"
    elif seconds < SECONDS_PER_MINUTE * MINUTES_PER_HOUR:
        text = f"{seconds // SECONDS_PER_MINUTE}m ago"
    else:
        text = f"{seconds // (SECONDS_PER_MINUTE * MINUTES_PER_HOUR)}h ago"
    if age_ms > stale_after_ms:
        text += " (stale)"
    return text


def battery_text(percent: int | None) -> str:
    """0xFF → 'unknown' — bad data is never a percentage (BLE_GATT §4.5)."""
    return "unknown" if percent is None else f"{percent}%"


def event_time_text(wall_unix: int) -> str:
    """Wall time → UTC text; 0 = unknown (SYNC §4.5 step 3) — flagged, never faked."""
    if wall_unix == 0:
        return "unknown"
    return datetime.fromtimestamp(wall_unix, tz=UTC).strftime("%Y-%m-%d %H:%M:%S UTC")
