// bw_activity — CadenceDetector tests (F03 §9: boundary/hysteresis around
// the cadence thresholds). Every scenario starts from a fresh detector unless
// a transition is exactly what is under test.

#include <cmath>
#include <limits>

#include <unity.h>

#include <bw/activity/cadence_detector.h>

using bw::activity::CadenceDetector;
using bw::activity::CadenceState;

namespace {

void assertState(CadenceState expected, CadenceState actual, const char* msg) {
    TEST_ASSERT_EQUAL_INT_MESSAGE(static_cast<int>(expected), static_cast<int>(actual), msg);
}

}  // namespace

void test_cadence_none_at_zero_frequency(void) {
    // Contract (02 §4.2): dominant_freq_hz is 0 when there is none.
    CadenceDetector detector;
    assertState(CadenceState::None, detector.step(0.0f, 0.0f), "zero frequency => None");
    assertState(CadenceState::None, detector.state(), "state() agrees with step()");
}

void test_cadence_none_on_nan_frequency(void) {
    // Defect guard: a NaN would fail every comparison and freeze the state.
    CadenceDetector detector;
    assertState(CadenceState::None, detector.step(std::nanf(""), 0.1f), "NaN from fresh => None");

    detector.step(1.6f, 0.1f);  // Walking first
    assertState(CadenceState::None, detector.step(std::nanf(""), 0.1f),
                "NaN clears an active state");
}

void test_cadence_band_boundaries(void) {
    struct Row {
        float freq_hz;
        float enmo_g;
        CadenceState expected;
        const char* name;
    };
    // First-match boundaries of the table (F03 spec §6, D3): exactly-on-threshold
    // values enter, one step outside does not (fresh detector => no hysteresis yet).
    const Row rows[] = {
        {1.19f, 0.05f, CadenceState::None, "below walking band"},
        {1.20f, 0.05f, CadenceState::Walking, "walking min inclusive"},
        {2.80f, 0.05f, CadenceState::Walking, "walking max inclusive"},
        {2.81f, 0.05f, CadenceState::None, "just above walking band, low energy"},
        {4.00f, 0.40f, CadenceState::Running, "running max inclusive"},
        {4.01f, 0.40f, CadenceState::None, "above human cadence range"},
        {1.50f, 0.01f, CadenceState::None, "cadence without motion is not walking"},
    };
    for (const Row& row : rows) {
        CadenceDetector detector;
        assertState(row.expected, detector.step(row.freq_hz, row.enmo_g), row.name);
    }
}

void test_cadence_walk_rejects_low_energy(void) {
    CadenceDetector detector;
    // Tapping/fidget: step-frequency oscillation without whole-body motion
    // (F03 D3) — the ENMO floor keeps it out of Walking.
    assertState(CadenceState::None, detector.step(1.6f, 0.02f),
                "walking cadence with sub-floor energy => None");
}

void test_cadence_overlap_low_energy_is_walking(void) {
    CadenceDetector detector;
    // 2.6–2.8 Hz overlap with walking-level impact: fast walk, not running (D3).
    assertState(CadenceState::Walking, detector.step(2.7f, 0.10f),
                "overlap + low energy => Walking");
}

void test_cadence_overlap_high_energy_is_running(void) {
    CadenceDetector detector;
    assertState(CadenceState::Running, detector.step(2.7f, 0.35f),
                "overlap + running energy => Running");
}

void test_cadence_walk_hysteresis_holds_below_min(void) {
    CadenceDetector detector;
    detector.step(1.5f, 0.05f);
    // 1.1 Hz is below the band but inside the widened exit band (1.2 - 0.2):
    // the state must hold instead of toggling every window (F03 D4).
    assertState(CadenceState::Walking, detector.step(1.1f, 0.05f),
                "1.1 Hz held by exit hysteresis");
    // 0.9 Hz is outside the widened band: exit.
    assertState(CadenceState::None, detector.step(0.9f, 0.05f), "0.9 Hz exits Walking");
}

void test_cadence_walk_hysteresis_holds_above_max(void) {
    CadenceDetector detector;
    detector.step(2.8f, 0.05f);
    assertState(CadenceState::Walking, detector.step(2.9f, 0.05f),
                "2.9 Hz held by exit hysteresis");
    // 3.05 Hz exceeds 2.8 + 0.2 and cannot enter Running at this energy.
    assertState(CadenceState::None, detector.step(3.05f, 0.05f), "3.05 Hz exits Walking");
}

void test_cadence_run_falls_back_to_walk(void) {
    CadenceDetector detector;
    detector.step(3.0f, 0.40f);
    // Below the running minimum but inside the walking band with walking
    // energy: Running must degrade to Walking, not stall in Running.
    assertState(CadenceState::Walking, detector.step(2.5f, 0.40f), "2.5 Hz => Walking");
}

void test_cadence_run_exits_when_energy_drops(void) {
    CadenceDetector detector;
    detector.step(3.0f, 0.40f);
    // In-band frequency but impact energy gone: neither enter condition
    // holds, so the state exits (energy gates apply on exit too).
    assertState(CadenceState::None, detector.step(3.0f, 0.10f), "low energy exits Running");
}

void test_cadence_energy_exit_from_walk(void) {
    CadenceDetector detector;
    detector.step(1.6f, 0.05f);
    assertState(CadenceState::None, detector.step(1.6f, 0.01f), "low energy exits Walking");
}

void test_cadence_overlap_hysteresis_holds_walking(void) {
    // Class-to-class edge (F03 D4/§8): inside the 2.6–2.8 Hz overlap with
    // running-level impact the raw priority would pick Running, but a state
    // that still matches the Walking row must be held — otherwise the state
    // toggles every window while the frequency hovers there.
    CadenceDetector detector;
    detector.step(2.55f, 0.30f);  // fresh entry below running_min => Walking
    assertState(CadenceState::Walking, detector.step(2.65f, 0.30f),
                "2.65 Hz held: no Walking -> Running flip inside the overlap");
    assertState(CadenceState::Walking, detector.step(2.75f, 0.30f),
                "2.75 Hz still matches the walking row => held");
    // Above walking_max the Walking row no longer matches, so D3's priority
    // takes over — the state changes only once the current row has left.
    assertState(CadenceState::Running, detector.step(2.90f, 0.30f),
                "2.90 Hz leaves the walking row => Running");
}

void test_cadence_overlap_alternation_does_not_toggle(void) {
    // F03 §8 kCadenceHysteresisHz rationale: "a frequency hovering at a band
    // edge cannot toggle CadenceState every window". High ENMO (0.30 g),
    // frequency alternating 2.55/2.65 Hz every window across the ~2.6 Hz
    // class-to-class edge.
    CadenceDetector from_walk;
    from_walk.step(2.55f, 0.30f);  // enters Walking
    for (int i = 0; i < 4; ++i) {
        assertState(CadenceState::Walking, from_walk.step(2.65f, 0.30f),
                    "alternating 2.65 Hz must not flip Walking -> Running");
        assertState(CadenceState::Walking, from_walk.step(2.55f, 0.30f),
                    "alternating 2.55 Hz stays Walking");
    }

    // Same alternation from a fresh Running entry: 2.55 Hz is below
    // running_min, so the documented fall-back fires exactly once (see
    // test_cadence_run_falls_back_to_walk) — after that the state settles
    // and never flips back, i.e. no per-window toggling either way.
    CadenceDetector from_run;
    from_run.step(2.65f, 0.30f);  // fresh entry in the overlap => Running
    assertState(CadenceState::Walking, from_run.step(2.55f, 0.30f),
                "2.55 Hz below running_min falls back to Walking");
    assertState(CadenceState::Walking, from_run.step(2.65f, 0.30f),
                "2.65 Hz must not flip Walking -> Running again");
    assertState(CadenceState::Walking, from_run.step(2.55f, 0.30f),
                "settled: still Walking on the next window");

    // Hovering entirely inside the overlap keeps the entered class too.
    CadenceDetector in_overlap;
    in_overlap.step(2.65f, 0.30f);  // fresh entry => Running
    assertState(CadenceState::Running, in_overlap.step(2.75f, 0.30f), "2.75 Hz held Running");
    assertState(CadenceState::Running, in_overlap.step(2.65f, 0.30f),
                "2.65 Hz held Running: no flip to Walking");
    assertState(CadenceState::Running, in_overlap.step(2.75f, 0.30f),
                "still Running after the next window");
}

void test_cadence_run_hysteresis_holds_above_max(void) {
    // Running-band exit hysteresis (§8 kCadenceHysteresisHz, exit-band
    // widening): 4.10 Hz is above the band but inside 4.0 + 0.2, and no
    // other row matches, so the state holds instead of dropping out.
    CadenceDetector detector;
    detector.step(3.0f, 0.40f);
    assertState(CadenceState::Running, detector.step(4.10f, 0.40f),
                "4.10 Hz held by exit hysteresis");
    // 4.21 Hz exceeds 4.0 + 0.2: out of the widened band, no row matches.
    assertState(CadenceState::None, detector.step(4.21f, 0.40f), "4.21 Hz exits Running");
}

void test_cadence_none_on_non_finite_enmo(void) {
    // Defect guard (F03 §10): ENMO is the gate that decides the overlap, so
    // a NaN/±Inf value is garbage input — it must never enter nor keep a
    // state, exactly like a non-finite frequency.
    CadenceDetector nan_enmo;
    assertState(CadenceState::None, nan_enmo.step(1.6f, std::nanf("")),
                "NaN ENMO with in-band frequency stays None");
    nan_enmo.step(1.6f, 0.05f);  // Walking first
    assertState(CadenceState::None, nan_enmo.step(1.6f, std::nanf("")),
                "NaN ENMO clears an active Walking state");

    const float inf = std::numeric_limits<float>::infinity();
    CadenceDetector inf_enmo;
    assertState(CadenceState::None, inf_enmo.step(3.0f, inf),
                "+Inf ENMO in the running band => no Running entry");
    inf_enmo.step(3.0f, 0.40f);  // Running first
    assertState(CadenceState::None, inf_enmo.step(3.0f, inf),
                "+Inf ENMO clears an active Running state");
}
