// F28 §9.5 — the tick sequence ResearchTask drives (spec §15 step 4), replayed
// here as pure logic: activate once, then per tick push a 100 ms batch of raw
// samples, runTick() and drain() - exactly the task body of wire_research.cpp,
// without FreeRTOS or a UART. The batch is deliberately larger than the
// device's per-tick load (kResearchTaskPeriodMs = 10 ms carries ~0.5 IMU +
// 1 PPG sample), so every row surviving here is a conservative superset of
// on-device behaviour. This is what makes the wiring logic host-testable even
// though the wiring file itself only builds for the device.

#include <cstdio>
#include <cstring>
#include <string>

#include <bw/containers/latest_value.h>
#include <bw/research/raw_tee.h>
#include <bw/research/research_config.h>
#include <bw/research/research_streamer.h>

#include <unity.h>

namespace {

// Sensor-side rates (F01 D15 / hw_config): 50 Hz IMU, 100 Hz PPG.
constexpr int kImuPerTick = 5;
constexpr int kPpgPerTick = 10;

struct TickLoop {
    bw::research::ResearchStreamer streamer{true};  // capture build-flag ON
    bw::research::RawTee tee;
    bw::LatestValue<bw::MotionFeatures> motion;
    std::string out;
    bw::TimeMs now = 0;

    TickLoop() {
        bw::research::StreamSources sources;
        sources.motion = &motion;
        sources.firmware_version = "0.1.0";
        sources.tee = &tee;
        streamer.setSources(sources);
    }

    // Sensor-side half of the cycle: samples arrive at the real rates.
    void push() {
        for (int i = 0; i < kImuPerTick; ++i) {
            bw::ImuSample sample{};
            sample.t_ms = now + static_cast<bw::TimeMs>(i * 20);
            sample.accel_g[2] = 1.0f;
            (void)tee.onImu(sample);
        }
        for (int i = 0; i < kPpgPerTick; ++i) {
            bw::PpgSample sample{};
            sample.t_ms = now + static_cast<bw::TimeMs>(i * 10);
            sample.red = 1000 + static_cast<uint32_t>(i);
            sample.ir = 2000 + static_cast<uint32_t>(i);
            (void)tee.onPpg(sample);
        }
    }

    // Research-task half: format this tick's rows into the ring.
    void tick() { streamer.runTick(now); }

    // Research-task half: hand the formatted bytes to the (fake) UART.
    void drainAll() {
        char chunk[256];
        size_t n = 0;
        while ((n = streamer.drain(chunk, sizeof(chunk))) > 0) {
            out.append(chunk, n);
        }
    }

    void advance() { now += 100; }

    // One activation tick with no samples: switches the tee on, emits #HDR
    // (then discarded) - exactly what happens on device at session start.
    void activate() {
        tick();
        drainAll();
        out.clear();
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

void test_tick_loop_captures_every_raw_sample(void) {
    // A drained-every-tick session at the real sensor rates must lose nothing:
    // 5 IMU + 10 PPG per 100 ms tick over 100 ticks.
    TickLoop f;
    f.activate();  // session start: tee switches on, #HDR goes out first
    for (int tick_index = 0; tick_index < 100; ++tick_index) {
        f.push();
        f.tick();
        f.drainAll();
        f.advance();
    }
    TEST_ASSERT_EQUAL_UINT(500, f.countLines("I,"));
    TEST_ASSERT_EQUAL_UINT(1000, f.countLines("P,"));
    TEST_ASSERT_EQUAL_UINT(0, f.tee.imuDrops());
    TEST_ASSERT_EQUAL_UINT(0, f.tee.ppgDrops());
    TEST_ASSERT_EQUAL_UINT(0, f.streamer.lineDrops());
    TEST_ASSERT_EQUAL_UINT(0, f.streamer.missedFeatureRows());
}

void test_tick_loop_raw_rows_are_time_ordered(void) {
    TickLoop f;
    f.activate();
    for (int tick_index = 0; tick_index < 20; ++tick_index) {
        f.push();
        f.tick();
        f.drainAll();
        f.advance();
    }
    // Within one kind, t_ms must strictly increase across the whole capture.
    bw::TimeMs previous = 0;
    size_t pos = 0;
    for (int row = 0; row < 100; ++row) {  // 20 ticks x 5 IMU
        pos = f.out.find("I,", pos);
        TEST_ASSERT_TRUE(pos != std::string::npos);
        unsigned long long t = 0;
        TEST_ASSERT_EQUAL_INT(1, std::sscanf(f.out.c_str() + pos, "I,%llu", &t));
        if (row == 0) {
            TEST_ASSERT_EQUAL_UINT64(0, t);  // first sample of the session
        } else {
            TEST_ASSERT_TRUE(t > previous);
        }
        previous = static_cast<bw::TimeMs>(t);
        pos += 2;
    }
}

void test_tick_loop_slow_drain_counts_drops_instead_of_stalling(void) {
    // A consumer that only drains every 5th tick cannot keep up (~2 KB per
    // 5 ticks vs 1 KB ring): the tee queues overflow, the loss is COUNTED and
    // the loop keeps running (F28 §10). Silent loss would fail this test.
    TickLoop f;
    for (int tick_index = 0; tick_index < 100; ++tick_index) {
        f.push();
        f.tick();
        if (tick_index % 5 == 0) {
            f.drainAll();
            f.out.clear();  // count rows since the last drain only
        }
        f.advance();
    }
    TEST_ASSERT_TRUE(f.tee.imuDrops() > 0);
    TEST_ASSERT_TRUE(f.tee.ppgDrops() > 0);
    TEST_ASSERT_TRUE(f.countLines("I,") < 500);         // fewer rows than samples: visible
    TEST_ASSERT_EQUAL_UINT(0, f.streamer.lineDrops());  // nothing half-written
}

void test_tick_loop_header_carries_final_drop_counters(void) {
    // After a slow-drain session, one more tick past the header interval
    // exposes the counters in #HDR (D10) so session.json can record them.
    TickLoop f;
    for (int tick_index = 0; tick_index < 100; ++tick_index) {
        f.push();
        f.tick();
        if (tick_index % 5 == 0) {
            f.drainAll();
        }
        f.advance();
    }
    f.drainAll();  // make room so the repeat header fits
    f.out.clear();

    f.streamer.runTick(f.now + bw::research::kHeaderRepeatMs);
    f.drainAll();

    char needle[64];
    std::snprintf(needle, sizeof(needle), ",%u,%u,", f.tee.imuDrops(), f.tee.ppgDrops());
    const size_t header_at = f.out.find("#HDR,");
    TEST_ASSERT_TRUE(header_at != std::string::npos);
    const size_t header_end = f.out.find('\n', header_at);
    TEST_ASSERT_TRUE(header_end != std::string::npos);
    TEST_ASSERT_TRUE(f.out.find(needle, header_at) < header_end);
}
