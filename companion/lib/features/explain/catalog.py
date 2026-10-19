"""F22 companion reason catalog: bit index -> full explanation sentence (spec §6 D1/D6).

The codes themselves live in `bw_core` (`bw/state.h`, append-only); F21's
`domain/wording.py` owns the *names*; this module adds exactly one human-readable
sentence per code so the companion can render WHY an event was raised. Keyed by
bit index (= enumerator order), pinned to the contract by `tests/test_catalog_parity.py`
(D6: the enum header is parsed at test time; appending a firmware code without a
companion sentence fails that test — never silently skipped).

Wording rules (docs/00_PROJECT_CONTEXT.md, spec §5/§12): sentences name observable factors only,
never a diagnosis; ASCII printable; no banned diagnostic terms (enforced by test).
"""

from __future__ import annotations

# [DERIVED] Python twin of the firmware's `kReasonCatalogVersion`
# (firmware/lib/bw_explain/include/bw/explain/reason_catalog.h): bumped on any
# code<->text change so a mismatch with the device is detectable; the parity test
# pins it to the firmware value parsed from that header (D9).
REASON_CATALOG_VERSION = 1

# [DERIVED] `ReasonMask = uint32_t` (data_decision.md §4.5): the builder walks bits
# 0..31; bits >= catalog size render "reason #N" — unknown stays surfaced, never
# hidden, exactly like the firmware half (D4/D8).
EXPLAIN_MAX_REASON_BITS = 32

# [DERIVED] Per-sentence bound: one console line with the name prefix on typical
# terminals; enforced by the parity test, not by convention (spec §8).
EXPLAIN_MAX_SENTENCE_CHARS = 120

# [HW] ReasonCode catalog bits 0..17 (data_decision.md §4.5, append-only enum;
# CR-008 bits 12-14, CR-009 bits 15-17). Order = enum order; completeness and
# lockstep with `wording.REASON_NAMES` and `bw/state.h` are test-enforced (D6).
REASON_EXPLANATIONS: dict[int, str] = {
    0: "motion stayed low for longer than the device's sustained-motion window",
    1: "no meaningful body movement for a prolonged stretch",
    2: "heart rate sat below this user's recent baseline",
    3: "heart rate trend moved noticeably away from the recent baseline",
    4: "motion dropped abruptly compared with the moment before",
    5: "the time of day has matched this user's typical drowsy periods",
    6: "the pattern resembles past events the user confirmed",
    7: "the user marked this moment themselves",
    8: "this falls inside a planned nap window",
    9: "sensor signal quality was low, so readings were partly unreliable",
    10: "a needed sensor was unavailable during this window",
    11: "the device was not being worn during this window",
    12: "low alertness probability held above the state threshold for its minimum duration",
    13: "calm wakefulness evidence was sustained through the recovery dwell",
    14: "inputs were valid but no state was sustained, kept as insufficient evidence",
    15: "the descent looked uncontrolled (jerk/impact spike): a possible fall, not confirmed",
    16: "the wrist re-oriented during the drop, e.g. sitting or lying down",
    17: "a short motion burst, shorter than the look-back window (possible gesture)",
}
