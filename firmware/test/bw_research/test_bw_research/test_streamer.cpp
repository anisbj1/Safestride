// F28 §9.2 — ResearchStreamer: gating (§5/D4), de-duplication, missed-value
// accounting, #HDR cadence and back-pressure (§10). No hardware, no FreeRTOS:
// the streamer is pure logic driven by explicit tick times.

#include <cstdio>
#include <cstring>
#include <string>

#include <bw/containers/latest_value.h>
#include <bw/context.h>
#include <bw/features.h>
#include <bw/research/research_config.h>
#include <bw/research/research_streamer.h>
#include <bw/state.h>

#include <unity.h>

namespace {

// Minimal IStateProvider fake (private test helper; bw_testkit has none yet).
struct FakeStateProvider final : bw::IStateProvider {
    bool have = false;
    bw::StateSnapshot snapshot{};

    bool latest(bw::StateSnapshot& out) const override {
        if (!have) {
            return false;
        }
        out = snapshot;
        return true;
    }

    void setMode(bw::OperatingMode mode) {
        snapshot = bw::StateSnapshot{};
        snapshot.mode = mode;
        have = true;
    }
};

bw::MotionFeatures motionAt(bw::TimeMs t_ms) {
    bw::MotionFeatures features{};
    features.t_ms = t_ms;
    features.quality = bw::DataQuality::Good;
    features.enmo_g = 0.01f;
    features.mag_std_g = 0.02f;
    features.jerk_rms_g_per_s = 0.03f;
    features.gyro_rms_dps = 4.0f;
    features.gravity_tilt_deg = 10.0f;
    features.dominant_freq_hz = 1.5f;
    features.stillness_ms = 100;
    return features;
}

bw::PpgFeatures ppgAt(bw::TimeMs t_ms) {
    bw::PpgFeatures features{};
    features.t_ms = t_ms;
    features.quality = bw::DataQuality::Good;
    features.hr_valid = true;
    features.hr_bpm = 60.0f;
    features.hr_confidence = 0.9f;
    features.perfusion_index = 0.05f;
    features.ir_dc_level = 1000.0f;
    features.sqi = 0.8f;
    return features;
}

struct Fixture {
    bw::research::ResearchStreamer streamer{};  // default: kResearchCaptureDefaultEnabled = false
    bw::LatestValue<bw::MotionFeatures> motion;
    bw::LatestValue<bw::PpgFeatures> ppg;
    bw::LatestValue<bw::ActivityEstimate> activity;
    FakeStateProvider state;
    std::string out;

    void bind(bool with_state = true) {
        bw::research::StreamSources sources;
        sources.motion = &motion;
        sources.ppg = &ppg;
        sources.activity = &activity;
        sources.state = with_state ? &state : nullptr;
        sources.firmware_version = "0.1.0";
        sources.boot_id = 42;
        streamer.setSources(sources);
    }

    void drainAll() {
        char chunk[256];
        size_t n = 0;
        while ((n = streamer.drain(chunk, sizeof(chunk))) > 0) {
            out.append(chunk, n);
        }
    }

    void clear() { out.clear(); }

    // Counts complete lines that start with `prefix`.
    size_t countLines(const char* prefix) const {
        size_t count = 0;
        size_t pos = out.find(prefix, 0);
        while (pos != std::string::npos) {
            ++count;
            pos = out.find(prefix, pos + 1);
        }
        return count;
    }
};

}  // namespace

void test_gate_default_off_produces_nothing(void) {
    Fixture f;
    f.bind();
    TEST_ASSERT_FALSE(f.streamer.active());
    f.streamer.runTick(0);
    f.drainAll();
    TEST_ASSERT_EQUAL_STRING("", f.out.c_str());
    TEST_ASSERT_EQUAL_UINT(0, f.streamer.rowsEmitted());
    TEST_ASSERT_FALSE(f.streamer.headerEmitted());
}

void test_gate_config_flag_on_streams(void) {
    Fixture f;
    f.streamer.setEnabled(true);  // the explicit enable of §5/D4
    f.bind();
    TEST_ASSERT_TRUE(f.streamer.active());
    f.streamer.runTick(0);
    f.drainAll();
    TEST_ASSERT_TRUE(f.streamer.headerEmitted());
}

void test_gate_research_mode_enables_without_flag(void) {
    Fixture f;
    f.bind();
    f.state.setMode(bw::OperatingMode::Research);
    TEST_ASSERT_TRUE(f.streamer.active());
    f.streamer.runTick(0);
    f.drainAll();
    TEST_ASSERT_TRUE(f.out.rfind("#HDR,", 0) == 0);
}

void test_gate_day_mode_stays_off(void) {
    Fixture f;
    f.bind();
    f.state.setMode(bw::OperatingMode::Day);
    TEST_ASSERT_FALSE(f.streamer.active());
    f.streamer.runTick(0);
    f.drainAll();
    TEST_ASSERT_EQUAL_STRING("", f.out.c_str());
}

void test_gate_no_state_yet_stays_off(void) {
    Fixture f;
    f.bind();  // provider present but has produced no snapshot
    TEST_ASSERT_FALSE(f.streamer.active());
    f.streamer.runTick(0);
    f.drainAll();
    TEST_ASSERT_EQUAL_STRING("", f.out.c_str());
}

void test_gate_disable_stops_rows(void) {
    Fixture f;
    f.streamer.setEnabled(true);
    f.bind();
    f.motion.set(motionAt(1000));
    f.streamer.runTick(0);
    f.drainAll();
    f.clear();

    f.streamer.setEnabled(false);
    f.motion.set(motionAt(2000));
    f.streamer.runTick(1000);
    f.drainAll();
    TEST_ASSERT_EQUAL_STRING("", f.out.c_str());
}

void test_header_is_first_row_and_carries_identity(void) {
    Fixture f;
    f.streamer.setEnabled(true);
    f.bind();
    f.streamer.runTick(0);
    f.drainAll();
    const size_t end = f.out.find('\n');
    TEST_ASSERT_TRUE(end != std::string::npos);
    TEST_ASSERT_EQUAL_STRING("#HDR,1,42,0.1.0,0,0,0,0", f.out.substr(0, end).c_str());
}

void test_header_repeats_at_interval(void) {
    Fixture f;
    f.streamer.setEnabled(true);
    f.bind();
    f.streamer.runTick(0);
    f.drainAll();
    f.clear();

    f.streamer.runTick(bw::research::kHeaderRepeatMs - 1);
    f.drainAll();
    TEST_ASSERT_EQUAL_STRING("", f.out.c_str());

    f.streamer.runTick(bw::research::kHeaderRepeatMs);
    f.drainAll();
    TEST_ASSERT_EQUAL_UINT(1, f.countLines("#HDR,"));
}

void test_motion_row_emitted_once_per_value(void) {
    Fixture f;
    f.streamer.setEnabled(true);
    f.bind();
    f.motion.set(motionAt(1000));
    f.streamer.runTick(0);
    f.drainAll();
    TEST_ASSERT_EQUAL_UINT(1, f.countLines("F,M,"));

    f.streamer.runTick(100);
    f.drainAll();
    TEST_ASSERT_EQUAL_UINT(1, f.countLines("F,M,"));  // no duplicate without new data
}

void test_motion_row_updated_on_new_value(void) {
    Fixture f;
    f.streamer.setEnabled(true);
    f.bind();
    f.motion.set(motionAt(1000));
    f.streamer.runTick(0);
    f.motion.set(motionAt(2000));
    f.streamer.runTick(100);
    f.drainAll();
    TEST_ASSERT_EQUAL_UINT(2, f.countLines("F,M,"));
    TEST_ASSERT_TRUE(f.out.find("F,M,2000,") != std::string::npos);
}

void test_same_timestamp_not_emitted_twice(void) {
    // sequence() moved on but the window did not: a raced set() must not turn
    // into a duplicate row nor into a "missed" count.
    Fixture f;
    f.streamer.setEnabled(true);
    f.bind();
    f.motion.set(motionAt(1000));
    f.streamer.runTick(0);
    const uint64_t after_first = f.streamer.rowsEmitted();

    f.motion.set(motionAt(1000));  // same t_ms, new sequence
    f.streamer.runTick(100);
    TEST_ASSERT_EQUAL_UINT(after_first, f.streamer.rowsEmitted());
    TEST_ASSERT_EQUAL_UINT(0, f.streamer.missedFeatureRows());
}

void test_all_three_streams_produce_rows(void) {
    Fixture f;
    f.streamer.setEnabled(true);
    f.bind();
    f.motion.set(motionAt(1000));
    f.ppg.set(ppgAt(1000));
    bw::ActivityEstimate activity{};
    activity.t_ms = 1000;
    activity.context = bw::ActivityContext::Walking;
    activity.confidence = 0.7f;
    f.activity.set(activity);

    f.streamer.runTick(0);
    f.drainAll();
    TEST_ASSERT_EQUAL_UINT(1, f.countLines("F,M,"));
    TEST_ASSERT_EQUAL_UINT(1, f.countLines("F,P,"));
    TEST_ASSERT_EQUAL_UINT(1, f.countLines("F,C,"));
    TEST_ASSERT_TRUE(f.out.find("F,C,1000,Walking,0.700") != std::string::npos);
}

void test_empty_sources_yield_header_only(void) {
    // First call / empty state: no mailbox has a value yet — "no data" is an
    // ordinary state, never a fabricated row (00 §1).
    bw::research::ResearchStreamer streamer(true);
    bw::research::StreamSources sources;
    sources.firmware_version = "0.1.0";
    streamer.setSources(sources);

    streamer.runTick(0);
    char out[1024];
    const size_t n = streamer.drain(out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_UINT(1, streamer.rowsEmitted());  // the header only
    out[n] = '\0';
    TEST_ASSERT_TRUE(std::strncmp(out, "#HDR,", 5) == 0);
}

void test_missed_rows_counted_on_sequence_jump(void) {
    // Two windows overwrite each other while we do not tick: the loss must be
    // counted (§10 — every drop has a counter), never silently absorbed.
    Fixture f;
    f.streamer.setEnabled(true);
    f.bind();
    f.motion.set(motionAt(1000));
    f.streamer.runTick(0);
    f.drainAll();
    TEST_ASSERT_EQUAL_UINT(0, f.streamer.missedFeatureRows());

    f.motion.set(motionAt(2000));
    f.motion.set(motionAt(3000));  // 2000 is gone before we read it
    f.streamer.runTick(100);
    f.drainAll();
    TEST_ASSERT_EQUAL_UINT(1, f.streamer.missedFeatureRows());
    TEST_ASSERT_TRUE(f.out.find("F,M,3000,") != std::string::npos);
}

void test_backpressure_defers_rows_and_counts_misses(void) {
    // Never draining must never stall runTick (§10): production pauses at the
    // ring reserve, values lost to mailbox overwrites are counted, and once we
    // drain again the stream resumes.
    Fixture f;
    f.streamer.setEnabled(true);
    f.bind();

    bw::TimeMs t = 0;
    for (int i = 0; i < 200; ++i) {
        t += 1000;
        f.motion.set(motionAt(t));
        f.streamer.runTick(t);  // no drain: ring fills up
    }
    const uint64_t emitted_while_full = f.streamer.rowsEmitted();
    TEST_ASSERT_TRUE(emitted_while_full < 200);  // paused, not unbounded
    TEST_ASSERT_TRUE(emitted_while_full > 0);

    f.drainAll();
    f.motion.set(motionAt(t + 1000));
    f.streamer.runTick(t + 1000);
    f.drainAll();
    // Now the loss made while the ring was full surfaces as a counted delta:
    // §10 wants every drop to have a counter, never silent absorption.
    TEST_ASSERT_TRUE(f.streamer.missedFeatureRows() > 0);
    TEST_ASSERT_TRUE(f.out.find("F,M,") != std::string::npos);  // resumed
    TEST_ASSERT_TRUE(f.countLines("F,M,") >= 1);
}

void test_drain_preserves_order_across_many_ticks(void) {
    // Drain after every tick (the wiring task's pattern): the ring wraps and
    // the rows must still come out complete and in order.
    Fixture f;
    f.streamer.setEnabled(true);
    f.bind();

    bw::TimeMs t = 0;
    for (int i = 1; i <= 40; ++i) {
        t += 1000;
        f.motion.set(motionAt(t));
        f.streamer.runTick(t);
        f.drainAll();
    }
    TEST_ASSERT_EQUAL_UINT(40, f.countLines("F,M,"));

    // Same inputs drained in one go must contain the same rows in order.
    size_t pos = 0;
    for (int i = 1; i <= 40; ++i) {
        char needle[32];
        const bw::TimeMs want = static_cast<bw::TimeMs>(i) * 1000;
        std::snprintf(needle, sizeof(needle), "F,M,%llu,", static_cast<unsigned long long>(want));
        const size_t at = f.out.find(needle, pos);
        TEST_ASSERT_TRUE(at != std::string::npos);
        pos = at;
    }
}
