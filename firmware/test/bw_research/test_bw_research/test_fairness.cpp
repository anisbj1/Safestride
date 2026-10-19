// F28 FIX (2026-10-13), F2 fairness finding — runTick's producer loop must
// not let a saturated raw tee starve the feature/context rows. The fixture
// reproduces the bench failure mode: the tee is filled to capacity BEFORE
// every tick so produceRaw() would win an unbounded number of passes under
// the old strict-priority order (0 F rows in the 10-minute capture), while
// property two pins that round-robin costs raw nothing.

#include <cstddef>
#include <cstdint>
#include <string>

#include <bw/containers/latest_value.h>
#include <bw/context.h>
#include <bw/features.h>
#include <bw/research/raw_tee.h>
#include <bw/research/research_config.h>
#include <bw/research/research_streamer.h>

#include <unity.h>

namespace {

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

bw::ActivityEstimate activityAt(bw::TimeMs t_ms) {
    bw::ActivityEstimate estimate{};
    estimate.t_ms = t_ms;
    estimate.context = bw::ActivityContext::Walking;
    estimate.confidence = 0.7f;
    return estimate;
}

struct FairFixture {
    bw::research::ResearchStreamer streamer{true};  // capture build-flag ON
    bw::research::RawTee tee;
    bw::LatestValue<bw::MotionFeatures> motion;
    bw::LatestValue<bw::ActivityEstimate> activity;
    std::string out;
    bw::TimeMs now = 0;

    FairFixture() {
        bw::research::StreamSources sources;
        sources.motion = &motion;
        sources.activity = &activity;
        sources.tee = &tee;
        sources.firmware_version = "0.1.0";
        streamer.setSources(sources);
    }

    // Session start: one tick switches the tee on and emits #HDR (discarded),
    // exactly as on device — same convention as test_run_tick.cpp.
    void activate() {
        streamer.runTick(now);
        drainAll();
        out.clear();
    }

    void pushRaw(size_t imu_count, size_t ppg_count) {
        bw::ImuSample imu{};
        imu.accel_g[2] = 1.0f;
        for (size_t i = 0; i < imu_count; ++i) {
            imu.t_ms = now + static_cast<bw::TimeMs>(i * 20);
            (void)tee.onImu(imu);
        }
        bw::PpgSample ppg{};
        for (size_t i = 0; i < ppg_count; ++i) {
            ppg.t_ms = now + static_cast<bw::TimeMs>(i * 10);
            ppg.red = 1000 + static_cast<uint32_t>(i);
            ppg.ir = 2000 + static_cast<uint32_t>(i);
            (void)tee.onPpg(ppg);
        }
    }

    // Fill both tee queues to capacity (kImuTeeDepth + kPpgTeeDepth): raw can
    // never empty during the tick that follows — the F1 saturation state.
    void fillRawBacklog() { pushRaw(bw::research::kImuTeeDepth, bw::research::kPpgTeeDepth); }

    void tick() { streamer.runTick(now); }

    void drainAll() {
        char chunk[256];
        size_t n = 0;
        while ((n = streamer.drain(chunk, sizeof(chunk))) > 0) {
            out.append(chunk, n);
        }
    }

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

void test_fairness_features_emit_while_raw_backlog_never_empties(void) {
    // Reverting to strict raw priority makes both F assertions fail: the ring
    // reserve stops the pass loop after ~15 raw rows with the tee still full,
    // so produceMotion()/produceActivity() are never reached (the F1/F2 bench
    // behaviour: 0 F rows, feature_missed 0 because nothing was attempted).
    FairFixture f;
    f.activate();
    f.fillRawBacklog();
    f.motion.set(motionAt(500));
    f.activity.set(activityAt(500));
    f.tick();
    f.drainAll();

    TEST_ASSERT_EQUAL_UINT(1, f.countLines("F,M,"));
    TEST_ASSERT_EQUAL_UINT(1, f.countLines("F,C,"));
    TEST_ASSERT_TRUE(f.countLines("I,") + f.countLines("P,") > 0);  // raw keeps flowing
    TEST_ASSERT_EQUAL_UINT(0, f.streamer.lineDrops());              // rows deferred, never rejected
    TEST_ASSERT_TRUE(f.tee.imuDrops() == 0);  // tee full but nothing overfilled
}

void test_fairness_rotation_emits_all_pending_raw_rows(void) {
    // No-regression guard for the round-robin change: one device tick's worth
    // of raw production plus a pending motion update must ALL come out in one
    // runTick() — fairness may not displace or defer a single raw row.
    FairFixture f;
    f.activate();
    f.pushRaw(5, 10);  // 50 Hz IMU + 100 Hz PPG per 100 ms of data
    f.motion.set(motionAt(500));
    f.tick();
    f.drainAll();

    TEST_ASSERT_EQUAL_UINT(5, f.countLines("I,"));
    TEST_ASSERT_EQUAL_UINT(10, f.countLines("P,"));
    TEST_ASSERT_EQUAL_UINT(1, f.countLines("F,M,"));
    TEST_ASSERT_EQUAL_UINT(0, f.tee.imuDrops());
    TEST_ASSERT_EQUAL_UINT(0, f.tee.ppgDrops());
    TEST_ASSERT_EQUAL_UINT(0, f.streamer.lineDrops());
    TEST_ASSERT_EQUAL_UINT(0, f.streamer.missedFeatureRows());
}

void test_fairness_every_tick_emits_pending_features_under_sustained_raw(void) {
    // Cursor state persists across ticks: with the tee refilled to capacity
    // every tick (raw never empties), the pending motion update must still get
    // a row in EVERY tick, for the whole session. Strict priority produces 0
    // of the expected kTicks rows here.
    FairFixture f;
    f.activate();
    constexpr int kTicks = 10;
    for (int i = 0; i < kTicks; ++i) {
        f.fillRawBacklog();
        f.motion.set(motionAt(f.now + 500));
        f.tick();
        f.drainAll();
        f.now += 100;
    }

    TEST_ASSERT_EQUAL_UINT(kTicks, f.countLines("F,M,"));
    TEST_ASSERT_TRUE(f.countLines("I,") + f.countLines("P,") > 0);
    TEST_ASSERT_EQUAL_UINT(0, f.streamer.lineDrops());
}
