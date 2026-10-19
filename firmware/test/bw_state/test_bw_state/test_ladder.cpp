// F07 §9 test_ladder.cpp — rules E1..E4 of the normative table (spec §7),
// table-driven: one case per rule row. Constant inputs make the EMA land on
// the input itself after the seed, so dwell boundaries are exact; tests that
// need instant evidence jumps inject ema_alpha = 1.0 (F04-style config
// injection) instead of doing EMA arithmetic in the expectation.

#include <bw/state/state_config.h>
#include <bw/state/state_engine.h>
#include <unity.h>

#include "state_test_support.h"

using bw::AlertnessState;
using bw::state::StateEngine;
using bw::state::StateEngineConfig;
using bw::state::StateInputs;
using bw::state::StateOutputs;
using f07_test::bitOf;
using f07_test::freshInputs;

namespace {

// An input row whose drowsy+sleep evidence equals `evidence` (sleep holds a
// fixed 0.1 share; the sleep CHANNEL alone feeds PossibleSleep, D7).
StateInputs atEvidence(bw::TimeMs t, float evidence) {
    StateInputs in = freshInputs(t);
    in.alertness.sleep = 0.1f;
    in.alertness.drowsy = evidence - 0.1f;
    return in;
}

}  // namespace

// (E1) 0.65 held: Drowsy commits EXACTLY at kDrowsyMinDurationMs, not one
// tick before; the state before is Unknown + InsufficientEvidence (E4),
// never a guessed Awake.
void test_ladder_drowsy_enters_at_dwell_not_before(void) {
    constexpr bw::TimeMs kD = bw::state::kDrowsyMinDurationMs;
    StateEngine e;
    for (bw::TimeMs t = 0; t < kD; t += 1000) {
        const StateOutputs out = e.step(atEvidence(t, 0.65f));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                              static_cast<int>(out.snapshot.alertness));
    }
    const StateOutputs out = e.step(atEvidence(kD, 0.65f));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy), static_cast<int>(e.state()));
    TEST_ASSERT_TRUE(out.transitioned);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                          static_cast<int>(out.previous));
}

// (E1) 0.59 — just below the enter threshold — never enters, any duration.
void test_ladder_below_enter_never_enters(void) {
    StateEngine e;
    for (bw::TimeMs t = 0; t <= 600000; t += 1000) {
        e.step(atEvidence(t, 0.59f));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                              static_cast<int>(e.state()));
    }
}

// (E1) The enter threshold itself counts (>=).
void test_ladder_enter_boundary_counts(void) {
    constexpr bw::TimeMs kD = bw::state::kDrowsyMinDurationMs;
    StateEngine e;
    for (bw::TimeMs t = 0; t < kD; t += 1000) {
        StateInputs in = freshInputs(t);  // stamps follow t: inputs stay fresh
        in.alertness.sleep = 0.0f;
        in.alertness.drowsy = bw::state::kDrowsyEnterProb;  // evidence == enter
        e.step(in);
    }
    StateInputs in = freshInputs(kD);
    in.alertness.sleep = 0.0f;
    in.alertness.drowsy = bw::state::kDrowsyEnterProb;
    e.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy), static_cast<int>(e.state()));
}

// (E1) A dip below enter RESETS the dwell (never pauses): 59 s of evidence,
// one dip, then evidence again — Drowsy lands a full dwell after the dip.
void test_ladder_dip_resets_dwell(void) {
    constexpr bw::TimeMs kD = bw::state::kDrowsyMinDurationMs;
    StateEngine e;
    for (bw::TimeMs t = 0; t < kD; t += 1000) {
        e.step(atEvidence(t, 0.65f));  // run-up just short of a full dwell
    }
    e.step(atEvidence(kD, 0.39f));  // one dip: smoothed below enter
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown), static_cast<int>(e.state()));
    for (bw::TimeMs t = kD + 1000; t < 2 * kD + 1000; t += 1000) {
        e.step(atEvidence(t, 0.65f));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                              static_cast<int>(e.state()));
    }
    e.step(atEvidence(2 * kD + 1000, 0.65f));  // full dwell AFTER the dip
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy), static_cast<int>(e.state()));
}

// (E1) 0.80: Drowsy at kDrowsyMinDurationMs, then HighDrowsinessRisk at
// kHighRiskMinDurationMs — escalation pays each rung's dwell in turn.
void test_ladder_high_risk_after_second_dwell(void) {
    constexpr bw::TimeMs kD = bw::state::kDrowsyMinDurationMs;
    constexpr bw::TimeMs kH = bw::state::kHighRiskMinDurationMs;
    StateEngine e;
    for (bw::TimeMs t = 0; t < kH; t += 1000) {
        const StateOutputs out = e.step(atEvidence(t, 0.80f));
        if (t < kD) {
            TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                                  static_cast<int>(out.snapshot.alertness));
        } else {
            TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy),
                                  static_cast<int>(out.snapshot.alertness));
        }
    }
    const StateOutputs out = e.step(atEvidence(kH, 0.80f));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::HighDrowsinessRisk),
                          static_cast<int>(e.state()));
    TEST_ASSERT_TRUE(out.transitioned);
}

// (E1) 0.70 held for 10 min stays Drowsy: between HighRisk enter (0.75) and
// Drowsy enter (0.60) the ladder holds, it never guesses higher.
void test_ladder_mid_band_stays_drowsy(void) {
    StateEngine e;
    for (bw::TimeMs t = 0; t <= 600000; t += 1000) {
        e.step(atEvidence(t, 0.70f));
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy), static_cast<int>(e.state()));
}

// (E1/D7) High drowsy ALONE never claims PossibleSleep: the sleep channel
// must carry it. 0.9 drowsy + 0 sleep reaches HighRisk and stops.
void test_ladder_drowsy_alone_never_possible_sleep(void) {
    StateEngine e;
    for (bw::TimeMs t = 0; t <= 600000; t += 1000) {
        StateInputs in = freshInputs(t);
        in.alertness.drowsy = 0.9f;
        in.alertness.sleep = 0.0f;
        e.step(in);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::HighDrowsinessRisk),
                          static_cast<int>(e.state()));
}

// (E1/D7) sleep >= 0.85 held: HighRisk at 120 s (the drowsy channel implies
// it), PossibleSleep at 180 s — the strongest claim needs the longest dwell.
void test_ladder_possible_sleep_after_third_dwell(void) {
    StateEngine e;
    for (bw::TimeMs t = 0; t < 180000; t += 1000) {
        StateInputs in = freshInputs(t);
        in.alertness.drowsy = 0.0f;
        in.alertness.sleep = 0.85f;
        const StateOutputs out = e.step(in);
        TEST_ASSERT_TRUE(out.snapshot.alertness != AlertnessState::PossibleSleep);
    }
    StateInputs in = freshInputs(180000);
    in.alertness.drowsy = 0.0f;
    in.alertness.sleep = 0.85f;
    e.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::PossibleSleep),
                          static_cast<int>(e.state()));
}

// (E2) De-escalation is IMMEDIATE, no dwell: HighRisk at 0.80, evidence to
// 0.50 (below the 0.55 exit, inside Drowsy's band) -> Drowsy on that very
// step (ema_alpha = 1.0 so the jump is exact).
void test_ladder_e2_deescalates_immediately(void) {
    StateEngineConfig cfg;
    cfg.ema_alpha = 1.0f;
    StateEngine e{cfg};
    for (bw::TimeMs t = 0; t <= 120000; t += 1000) {
        e.step(atEvidence(t, 0.80f));
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::HighDrowsinessRisk),
                          static_cast<int>(e.state()));
    const StateOutputs out = e.step(atEvidence(121000, 0.50f));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy), static_cast<int>(e.state()));
    TEST_ASSERT_TRUE(out.transitioned);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::HighDrowsinessRisk),
                          static_cast<int>(out.previous));
}

// (E2) PossibleSleep with sleep evidence 0.60 (below the 0.65 exit, inside
// HighRisk's support) -> HighDrowsinessRisk immediately. NOTE: this corrects
// the §9 row's "0.70" — 0.70 is INSIDE the hysteresis band and holds
// PossibleSleep (next case); the exit-threshold semantics of D8/E2 are
// normative (status log 2026-10-11).
void test_ladder_e2_possible_sleep_to_high_risk(void) {
    StateEngineConfig cfg;
    cfg.ema_alpha = 1.0f;
    StateEngine e{cfg};
    for (bw::TimeMs t = 0; t <= 180000; t += 1000) {
        StateInputs in = freshInputs(t);
        in.alertness.drowsy = 0.0f;
        in.alertness.sleep = 0.85f;
        e.step(in);
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::PossibleSleep),
                          static_cast<int>(e.state()));
    StateInputs in = freshInputs(181000);
    in.alertness.drowsy = 0.0f;
    in.alertness.sleep = 0.60f;
    const StateOutputs out = e.step(in);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::HighDrowsinessRisk),
                          static_cast<int>(e.state()));
    TEST_ASSERT_TRUE(out.transitioned);
}

// (E2, corrected row) sleep 0.70 sits between the Sleep exit (0.65) and
// enter (0.85): the hysteresis band HOLDS PossibleSleep — thresholds alone
// would flap here (why D8 has enter/exit pairs).
void test_ladder_e2_sleep_070_holds_possible_sleep(void) {
    StateEngineConfig cfg;
    cfg.ema_alpha = 1.0f;
    StateEngine e{cfg};
    for (bw::TimeMs t = 0; t <= 180000; t += 1000) {
        StateInputs in = freshInputs(t);
        in.alertness.drowsy = 0.0f;
        in.alertness.sleep = 0.85f;
        e.step(in);
    }
    for (bw::TimeMs t = 181000; t <= 300000; t += 1000) {
        StateInputs in = freshInputs(t);
        in.alertness.drowsy = 0.0f;
        in.alertness.sleep = 0.70f;
        e.step(in);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::PossibleSleep),
                              static_cast<int>(e.state()));
    }
}

// (E3) Recovery must be SUSTAINED: Drowsy, then calm 0.39 (below the 0.40
// exit) — still Drowsy before the first full recovery interval, Awake exactly
// at kAwakeRecoveryMs.
void test_ladder_e3_calm_boundary(void) {
    StateEngineConfig cfg;
    cfg.ema_alpha = 1.0f;
    StateEngine e{cfg};
    for (bw::TimeMs t = 0; t <= 60000; t += 1000) {
        e.step(atEvidence(t, 0.65f));
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy), static_cast<int>(e.state()));
    e.step(atEvidence(61000, 0.39f));  // first calm sample starts recovery
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy), static_cast<int>(e.state()));
    e.step(atEvidence(61000 + bw::state::kAwakeRecoveryMs - 1, 0.39f));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy), static_cast<int>(e.state()));
    e.step(atEvidence(61000 + bw::state::kAwakeRecoveryMs, 0.39f));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake), static_cast<int>(e.state()));
}

// A valid F06 ShortMotionBurst marker marks the first stillness estimate.
// Reseeding from it means the existing 15 s rung dwell starts at that exact
// onset; the dwell is not bypassed or shortened.
void test_ladder_short_motion_marker_seeds_at_stillness_onset(void) {
    StateEngine e;
    StateInputs awake = freshInputs(0);
    awake.alertness.drowsy = 0.0f;
    awake.alertness.sleep = 0.0f;
    e.step(awake);  // seed prior EMA from awake-dominant evidence

    constexpr bw::TimeMs kStillnessOnsetMs = 1000;
    for (bw::TimeMs t = kStillnessOnsetMs; t < kStillnessOnsetMs + bw::state::kDrowsyMinDurationMs;
         t += 1000) {
        StateInputs in = atEvidence(t, 0.80f);
        if (t == kStillnessOnsetMs) {
            in.alertness.reasons = bitOf(bw::ReasonCode::ShortMotionBurst);
            in.short_motion_burst = true;
        }
        const StateOutputs out = e.step(in);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                              static_cast<int>(out.snapshot.alertness));
    }
    StateInputs boundary = atEvidence(kStillnessOnsetMs + bw::state::kDrowsyMinDurationMs, 0.80f);
    const StateOutputs out = e.step(boundary);
    TEST_ASSERT_TRUE(out.transitioned);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy),
                          static_cast<int>(out.snapshot.alertness));
}

// Without the one-shot marker, the ordinary EMA still smooths a sudden
// probability change and delays starting the rung dwell.
void test_ladder_ordinary_estimate_keeps_ema_smoothing(void) {
    StateEngine e;
    StateInputs awake = freshInputs(0);
    awake.alertness.drowsy = 0.0f;
    awake.alertness.sleep = 0.0f;
    e.step(awake);
    for (bw::TimeMs t = 1000; t < bw::state::kDrowsyMinDurationMs + 7000; t += 1000) {
        const StateOutputs out = e.step(atEvidence(t, 0.80f));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Awake),
                              static_cast<int>(out.snapshot.alertness));
    }
    const StateOutputs out = e.step(atEvidence(bw::state::kDrowsyMinDurationMs + 7000, 0.80f));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy),
                          static_cast<int>(out.snapshot.alertness));
}

void test_ladder_active_motion_recovers_from_drowsy_within_five_seconds(void) {
    StateEngine e;
    for (bw::TimeMs t = 0; t <= bw::state::kDrowsyMinDurationMs; t += 1000) {
        (void)e.step(atEvidence(t, 0.65f));
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy), static_cast<int>(e.state()));

    const bw::TimeMs kRecoveryStartMs = bw::state::kDrowsyMinDurationMs + 1000;
    bool recovered = false;
    for (bw::TimeMs elapsed = 0; elapsed <= 5000; elapsed += 1000) {
        StateInputs in = freshInputs(kRecoveryStartMs + elapsed);
        in.alertness.drowsy = 0.0f;
        in.alertness.sleep = 0.0f;
        const StateOutputs out = e.step(in);
        if (out.snapshot.alertness == AlertnessState::Awake) {
            recovered = true;
            break;
        }
    }
    TEST_ASSERT_TRUE(recovered);
}

// (E4/hysteresis) Enter Drowsy first (0.65 for a full dwell), then feed
// 0.45 — inside Drowsy's band [0.40, 0.60): ABOVE the exit, BELOW the enter
// — and Drowsy holds indefinitely (thresholds alone would flap here; the
// asymmetric enter/exit bounds every excursion).
void test_ladder_hysteresis_band_holds_drowsy(void) {
    StateEngineConfig cfg;
    cfg.ema_alpha = 1.0f;
    StateEngine e{cfg};
    for (bw::TimeMs t = 0; t <= 60000; t += 1000) {
        e.step(atEvidence(t, 0.65f));  // establish Drowsy
    }
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy), static_cast<int>(e.state()));
    for (bw::TimeMs t = 61000; t <= 600000; t += 1000) {
        e.step(atEvidence(t, 0.45f));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy),
                              static_cast<int>(e.state()));
    }
}

// (E4) Ambiguous-but-stable evidence (0.50: not calm, no rung entered) after
// a gate period stays Unknown + InsufficientEvidence — never a fabricated
// Awake (D8/E4, 00 §1).
void test_ladder_e4_ambiguous_stays_unknown(void) {
    StateEngine e;
    StateInputs in = freshInputs(0);
    in.alertness.valid = false;  // gate period
    for (bw::TimeMs t = 0; t <= 5000; t += 1000) {
        in.t_ms = t;
        e.step(in);
    }
    for (bw::TimeMs t = 6000; t <= 600000; t += 1000) {
        const StateOutputs out = e.step(atEvidence(t, 0.50f));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                              static_cast<int>(out.snapshot.alertness));
        TEST_ASSERT_EQUAL_UINT32(bitOf(bw::ReasonCode::InsufficientEvidence), out.snapshot.reasons);
    }
}

// (E1 from a gate) Clearing a gate does NOT shorten the rung dwell: the
// counters were reset at gate entry, so Drowsy lands a full
// kDrowsyMinDurationMs after the gate clears.
void test_ladder_e1_from_gate_pays_full_dwell(void) {
    constexpr bw::TimeMs kD = bw::state::kDrowsyMinDurationMs;
    constexpr bw::TimeMs kEvidenceStart = 10000;
    StateEngine e;
    StateInputs in = freshInputs(0);
    in.alertness.valid = false;
    for (bw::TimeMs t = 0; t <= 9000; t += 1000) {
        in.t_ms = t;
        e.step(in);
    }
    for (bw::TimeMs t = kEvidenceStart; t < kEvidenceStart + kD; t += 1000) {
        e.step(atEvidence(t, 0.65f));
        TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Unknown),
                              static_cast<int>(e.state()));
    }
    e.step(atEvidence(kEvidenceStart + kD, 0.65f));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(AlertnessState::Drowsy), static_cast<int>(e.state()));
}

// Flapping noise (§9): a fixed-seed walk +/-0.05 around the 0.60 enter line
// for 30 simulated minutes yields at most one transition per dwell period —
// the EMA + continuous-evidence rule bounds the damage of noise.
void test_ladder_flapping_noise_bounded_transitions(void) {
    StateEngine e;
    uint32_t lcg = 20261011u;  // fixed seed: deterministic
    uint32_t transitions = 0;
    for (bw::TimeMs t = 0; t <= 1800000; t += 1000) {
        lcg = lcg * 1103515245u + 12345u;
        const float v = 0.55f + 0.05f * static_cast<float>((lcg >> 16) % 3u);
        const StateOutputs out = e.step(atEvidence(t, v));
        if (out.transitioned) {
            ++transitions;
            // Only the first rung is even reachable at these amplitudes.
            TEST_ASSERT_TRUE(out.snapshot.alertness == AlertnessState::Unknown ||
                             out.snapshot.alertness == AlertnessState::Drowsy);
        }
    }
    // 30 min / 60 s dwell = 30 transitions max; the walk may never flap
    // faster than the dwell.
    TEST_ASSERT_TRUE(transitions <= 30);
}

// Property (§9): monotonicity — raising the sleep probability never lowers
// the reported alertness rung, at any step of an identical timeline.
void test_ladder_monotonic_in_sleep_probability(void) {
    StateEngine low;
    StateEngine high;
    for (bw::TimeMs t = 0; t <= 1200000; t += 1000) {
        StateInputs a = freshInputs(t);
        a.alertness.drowsy = 0.30f;
        a.alertness.sleep = 0.25f;
        StateInputs b = freshInputs(t);
        b.alertness.drowsy = 0.30f;
        b.alertness.sleep = 0.55f;  // same timeline, strictly more sleep
        low.step(a);
        high.step(b);
        TEST_ASSERT_TRUE(static_cast<int>(high.state()) >= static_cast<int>(low.state()));
    }
}
