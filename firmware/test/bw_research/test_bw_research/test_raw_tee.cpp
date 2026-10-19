// F28 §9.3 — raw tee (D3): gated producer-side taps on F28-owned queues, drop
// counting on overflow (CR-004 Err::Full) and the streamer-side sync that keeps
// the tee behind the §5 gate. The single-consumer hazard (D5) is enforced by
// review, not by a test: nothing here touches ctx.imu_samples / ctx.ppg_samples.

#include <cstdio>
#include <cstring>
#include <string>

#include <bw/containers/latest_value.h>
#include <bw/hardware.h>
#include <bw/research/raw_tee.h>
#include <bw/research/research_config.h>
#include <bw/research/research_streamer.h>

#include <unity.h>

namespace {

bw::ImuSample imuAt(bw::TimeMs t_ms, float ax = 0.1f) {
    bw::ImuSample sample{};
    sample.t_ms = t_ms;
    sample.accel_g[0] = ax;
    sample.accel_g[1] = 0.2f;
    sample.accel_g[2] = 0.9f;
    sample.gyro_dps[0] = 1.0f;
    sample.gyro_dps[1] = 2.0f;
    sample.gyro_dps[2] = 3.0f;
    return sample;
}

bw::PpgSample ppgAt(bw::TimeMs t_ms, uint32_t red = 1000) {
    bw::PpgSample sample{};
    sample.t_ms = t_ms;
    sample.red = red;
    sample.ir = red * 2;
    return sample;
}

struct TeeFixture {
    bw::research::ResearchStreamer streamer{true};
    bw::research::RawTee tee;
    bw::LatestValue<bw::MotionFeatures> motion;
    std::string out;

    TeeFixture() {
        bw::research::StreamSources sources;
        sources.motion = &motion;
        sources.firmware_version = "0.1.0";
        sources.tee = &tee;
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

void test_tee_disabled_refuses_and_stores_nothing(void) {
    bw::research::RawTee tee;
    TEST_ASSERT_FALSE(tee.enabled());  // §5 default: OFF
    TEST_ASSERT_EQUAL(static_cast<int>(bw::Err::Unsupported),
                      static_cast<int>(tee.onImu(imuAt(0))));
    TEST_ASSERT_EQUAL(static_cast<int>(bw::Err::Unsupported),
                      static_cast<int>(tee.onPpg(ppgAt(0))));
    // Refused-while-off is a deliberate switch, not data loss (§10).
    TEST_ASSERT_EQUAL_UINT(0, tee.imuDrops());
    TEST_ASSERT_EQUAL_UINT(0, tee.ppgDrops());

    bw::ImuSample out{};
    TEST_ASSERT_FALSE(tee.popImu(out));
}

void test_tee_enabled_stores_in_order(void) {
    bw::research::RawTee tee;
    tee.setEnabled(true);
    for (bw::TimeMs t = 0; t < 5; ++t) {
        TEST_ASSERT_EQUAL(static_cast<int>(bw::Err::Ok),
                          static_cast<int>(tee.onImu(imuAt(t * 20))));
    }
    bw::ImuSample got{};
    for (bw::TimeMs t = 0; t < 5; ++t) {
        TEST_ASSERT_TRUE(tee.popImu(got));
        TEST_ASSERT_EQUAL_UINT64(t * 20, got.t_ms);
    }
    TEST_ASSERT_FALSE(tee.popImu(got));
    TEST_ASSERT_EQUAL_UINT(0, tee.imuDrops());
}

void test_tee_full_refuses_and_counts_drops(void) {
    // CR-004 / F00 §9: a full queue refuses (Err::Full), counts the drop and
    // keeps the stored items — never a silent overwrite.
    bw::research::RawTee tee;
    tee.setEnabled(true);
    const size_t depth = bw::research::kImuTeeDepth;
    for (size_t i = 0; i < depth; ++i) {
        TEST_ASSERT_EQUAL(static_cast<int>(bw::Err::Ok),
                          static_cast<int>(tee.onImu(imuAt(static_cast<bw::TimeMs>(i * 20)))));
    }
    TEST_ASSERT_EQUAL(static_cast<int>(bw::Err::Full), static_cast<int>(tee.onImu(imuAt(9999))));
    TEST_ASSERT_EQUAL_UINT(1, tee.imuDrops());

    bw::ImuSample got{};
    TEST_ASSERT_TRUE(tee.popImu(got));
    TEST_ASSERT_EQUAL_UINT64(0, got.t_ms);  // first item intact, order preserved
}

void test_tee_gate_synced_from_streamer_activity(void) {
    // The producer-side push (A1) reads tee.enabled(), which runTick() mirrors
    // from ResearchStreamer::active() — one place implements the D4 gate, for
    // the default-off streamer as well as for an enabled one.
    bw::research::ResearchStreamer streamer;  // default: kResearchCaptureDefaultEnabled
    bw::research::RawTee tee;
    bw::research::StreamSources sources;
    sources.tee = &tee;
    streamer.setSources(sources);

    TEST_ASSERT_FALSE(tee.enabled());
    streamer.runTick(0);
    TEST_ASSERT_FALSE(tee.enabled());  // flag off, no Research mode source

    streamer.setEnabled(true);
    streamer.runTick(100);
    TEST_ASSERT_TRUE(tee.enabled());

    streamer.setEnabled(false);
    streamer.runTick(200);
    TEST_ASSERT_FALSE(tee.enabled());
}

void test_raw_rows_emitted_when_tee_enabled(void) {
    TeeFixture f;
    f.streamer.runTick(0);  // activates and switches the tee on
    f.clear();              // discard the #HDR row
    TEST_ASSERT_TRUE(f.tee.enabled());

    TEST_ASSERT_EQUAL(static_cast<int>(bw::Err::Ok), static_cast<int>(f.tee.onImu(imuAt(100))));
    TEST_ASSERT_EQUAL(static_cast<int>(bw::Err::Ok), static_cast<int>(f.tee.onPpg(ppgAt(120))));
    f.streamer.runTick(100);
    f.drainAll();

    TEST_ASSERT_EQUAL_UINT(1, f.countLines("I,100,"));
    TEST_ASSERT_EQUAL_UINT(1, f.countLines("P,120,1000,2000"));
}

void test_raw_rows_absent_while_gate_off(void) {
    TeeFixture f;
    // Default gate off: pushes are refused and no I/P rows exist even after a
    // tick (§5 gate-default-off for the raw path).
    TEST_ASSERT_EQUAL(static_cast<int>(bw::Err::Unsupported),
                      static_cast<int>(f.tee.onImu(imuAt(100))));
    f.streamer.runTick(0);
    f.drainAll();
    TEST_ASSERT_EQUAL_UINT(0, f.countLines("I,"));
    TEST_ASSERT_EQUAL_UINT(0, f.countLines("P,"));
}

void test_raw_backpressure_counts_drops_and_reports_in_header(void) {
    // Not draining must not stall the producer: the tee queue overflows, the
    // drops are counted, and the next #HDR carries the counters (D10) so the
    // capture tool can write raw_drops into session.json.
    TeeFixture f;
    f.streamer.runTick(0);
    TEST_ASSERT_TRUE(f.tee.enabled());

    for (bw::TimeMs t = 0; t < 400; ++t) {
        (void)f.tee.onImu(imuAt(t * 20));  // no drain anywhere: ring + queue fill
    }
    TEST_ASSERT_TRUE(f.tee.imuDrops() > 0);

    f.drainAll();  // the wiring task's drain makes room again
    f.clear();
    f.streamer.runTick(bw::research::kHeaderRepeatMs);
    f.drainAll();

    char needle[64];
    std::snprintf(needle, sizeof(needle), ",%u,%u,", f.tee.imuDrops(), f.tee.ppgDrops());
    const size_t header_at = f.out.find("#HDR,");
    TEST_ASSERT_TRUE(header_at != std::string::npos);
    const size_t header_end = f.out.find('\n', header_at);
    TEST_ASSERT_TRUE(header_end != std::string::npos);
    // Inside the header line only — the counters are part of #HDR (D10).
    TEST_ASSERT_TRUE(f.out.find(needle, header_at) < header_end);
}
