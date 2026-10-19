// F02 CPU budget evidence (spec §11: "CPU < 3 % of one core at 240 MHz —
// measure and record"). A host benchmark, deliberately NOT a Unity test:
// the host-testing guidance forbids wall-clock in tests, so this is a manual,
// re-runnable tool like design_ppg_filter.py.
//
// Build (any C++17 compiler; from the repo root):
//   g++ -std=c++17 -O2 -Wall -Wextra -Ifirmware/lib/bw_signal/include
//       -Ifirmware/lib/bw_core/include firmware/lib/bw_signal/tools/bench_signal_processor.cpp
//       firmware/lib/bw_signal/src/*.cpp -o bench_signal_processor.exe
// (paste onto one line, or use a response file / shell continuation).
//
// What it measures: the per-frame cost the 1 s ProcessingTask pays — feeding
// one window of raw samples (100 PPG + 50 IMU) plus the scheduler catch-up
// step() loop — over a fixed deterministic 10-minute workload. The ESP32
// figure is derived in the F02 status log from this host number; it is not
// measured here (no device attached to CI).

#include <chrono>
#include <cmath>
#include <cstdio>

#include <bw/signal/signal_processor.h>

namespace signal = bw::signal;
using bw::ImuSample;
using bw::PpgSample;
using bw::TimeMs;

namespace {

constexpr int kBenchFrames = 600;                // [DERIVED] 10 simulated minutes
constexpr float kBenchFreqHz = 70.0f / 60.0f;    // [DERIVED] 70 bpm fixture (§9)
constexpr float kBenchDcIr = 50000.0f;           // [HW] plausible MAX30102 DC baseline
constexpr float kBenchPulseAmplitude = 1000.0f;  // [DERIVED] PI ≈ 1.4 % after filtering
constexpr float kBenchPi = 3.14159265f;

PpgSample mkPpg(TimeMs t) {
    PpgSample s{};
    s.t_ms = t;
    const float x =
        kBenchDcIr +
        kBenchPulseAmplitude * std::sin(2.0f * kBenchPi * kBenchFreqHz * static_cast<float>(t) /
                                        static_cast<float>(signal::kMsPerSecond));
    s.ir = static_cast<uint32_t>(x);
    s.red = s.ir;
    return s;
}

ImuSample mkQuietImu(TimeMs t) {
    ImuSample s{};
    s.t_ms = t;
    s.accel_g[0] = 0.0f;
    s.accel_g[1] = 0.0f;
    s.accel_g[2] = 1.0f;
    return s;
}

}  // namespace

int main() {
    signal::SignalProcessor processor;
    signal::SignalWindowOutput out{};

    const auto t0 = std::chrono::steady_clock::now();
    for (int frame = 0; frame < kBenchFrames; ++frame) {
        const TimeMs base = static_cast<TimeMs>(frame) * signal::kMotionWindowMs;
        PpgSample ppg[100];
        for (uint32_t i = 0; i < 100; ++i) {
            ppg[i] = mkPpg(base + i * signal::kPpgSamplePeriodMs);
        }
        processor.feedPpg(ppg, 100);

        ImuSample imu[50];
        for (uint32_t i = 0; i < 50; ++i) {
            imu[i] = mkQuietImu(base + i * signal::kImuSamplePeriodMs);
        }
        processor.feedImu(imu, 50);

        // Same catch-up loop the 1 s ProcessingTask runs after feeding.
        const TimeMs now = base + signal::kMotionWindowMs + signal::kMaxSampleGapMs;
        while (processor.step(now, out)) {
        }
    }
    const auto t1 = std::chrono::steady_clock::now();

    const double total_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    const double us_per_frame = total_us / static_cast<double>(kBenchFrames);
    std::printf("[F02 S11] frames %d | host total %.1f ms | %.1f us/frame "
                "(feed 100 PPG + 50 IMU + step catch-up)\n",
                kBenchFrames, total_us / 1000.0, us_per_frame);
    std::printf("[F02 S11] host frame cost = %.4f %% of the 1 s window period\n",
                us_per_frame / 10000.0);
    std::printf("[F02 S11] last frame: motion quality %d, ppg quality %d, hr_valid %d\n",
                static_cast<int>(out.motion.quality), static_cast<int>(out.ppg.quality),
                static_cast<int>(out.ppg.hr_valid));
    return 0;
}
