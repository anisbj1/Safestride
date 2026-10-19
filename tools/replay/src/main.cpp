// tools/replay/src/main.cpp — F28 host replay (spec §9.6).
//
// Reads a session directory produced by tools/capture (raw_imu.csv,
// raw_ppg.csv), feeds the samples through the SAME bw::signal::SignalProcessor
// the device runs, and prints the regenerated F,M / F,P rows on stdout in the
// DATASET_FORMAT.md grammar. tools/replay/compare.py diffs them against the
// session's recorded features.csv with float tolerances (host-vs-xtensa libm
// rounding is expected; a wrong/lost/extra window or a value beyond tolerance
// is a real mismatch → exit 3).
//
// Host tool: heap and iostream-free C++ is not required here, but the code
// stays in the firmware house style (no exceptions, explicit error exits).

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <bw/hardware.h>
#include <bw/research/line_format.h>
#include <bw/signal/signal_processor.h>

namespace {

// [DERIVED] Stream-end drain cap: after the last raw row a few catch-up
// step() calls may still be owed (same pattern as wire_f02's `while (step)`).
// The hard cap turns a scheduler bug into exit 3 instead of an infinite loop.
constexpr int kMaxFlushSteps = 100000;

// [DERIVED] Same row cap as the firmware formatter (research_config.h
// kMaxLineLen): replay only ever prints single lines.
constexpr size_t kLineCap = 256;

struct ImuRow {
    bw::TimeMs t_ms = 0;
    bw::ImuSample sample{};
};

struct PpgRow {
    bw::TimeMs t_ms = 0;
    bw::PpgSample sample{};
};

// CSV layout is DATASET_FORMAT.md §3 (header line skipped). Files come from
// tools/capture and are time-ordered by construction — no sorting here.
bool loadImu(const std::string& path, std::vector<ImuRow>& rows) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    char line[256];
    bool first = true;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (first) {
            first = false;
            continue;
        }
        ImuRow row{};
        double t_ms = 0.0;
        unsigned int boot_id = 0;
        const int got =
            std::sscanf(line, "%lf,%u,%f,%f,%f,%f,%f,%f", &t_ms, &boot_id, &row.sample.accel_g[0],
                        &row.sample.accel_g[1], &row.sample.accel_g[2], &row.sample.gyro_dps[0],
                        &row.sample.gyro_dps[1], &row.sample.gyro_dps[2]);
        (void)boot_id;  // session-level identity; not part of the sample stream
        if (got == 8) {
            row.t_ms = static_cast<bw::TimeMs>(t_ms);
            row.sample.t_ms = row.t_ms;
            rows.push_back(row);
        }
    }
    std::fclose(file);
    return !rows.empty();
}

bool loadPpg(const std::string& path, std::vector<PpgRow>& rows) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    char line[256];
    bool first = true;
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (first) {
            first = false;
            continue;
        }
        PpgRow row{};
        double t_ms = 0.0;
        unsigned int boot_id = 0;
        unsigned int red = 0;
        unsigned int ir = 0;
        const int got = std::sscanf(line, "%lf,%u,%u,%u", &t_ms, &boot_id, &red, &ir);
        (void)boot_id;
        if (got == 4) {
            row.t_ms = static_cast<bw::TimeMs>(t_ms);
            row.sample.t_ms = row.t_ms;
            row.sample.red = red;
            row.sample.ir = ir;
            rows.push_back(row);
        }
    }
    std::fclose(file);
    return !rows.empty();
}

// Prints one closed window in the firmware's own row grammar — reusing
// bw_research's formatters means the replay output and a device capture are
// byte-comparable, not "equivalent modulo formatting".
void emit(const bw::signal::SignalWindowOutput& out) {
    char line[kLineCap];
    if (out.has_motion) {
        const size_t n = bw::research::formatMotionFeatures(line, sizeof(line), out.motion);
        if (n > 0) {
            std::fwrite(line, 1, n, stdout);
        }
    }
    if (out.has_ppg) {
        const size_t n = bw::research::formatPpgFeatures(line, sizeof(line), out.ppg);
        if (n > 0) {
            std::fwrite(line, 1, n, stdout);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: replay <session_dir>\n");
        return 4;
    }
    const std::string dir = argv[1];
    std::vector<ImuRow> imu;
    std::vector<PpgRow> ppg;
    const bool have_imu = loadImu(dir + "/raw_imu.csv", imu);
    const bool have_ppg = loadPpg(dir + "/raw_ppg.csv", ppg);
    if (!have_imu && !have_ppg) {
        std::fprintf(stderr, "replay: no readable raw rows in %s\n", dir.c_str());
        return 4;
    }

    bw::signal::SignalProcessor processor;
    bw::signal::SignalWindowOutput out{};
    size_t i = 0;
    size_t j = 0;
    bw::TimeMs now = 0;
    while (i < imu.size() || j < ppg.size()) {
        const bool take_imu = (j >= ppg.size()) || (i < imu.size() && imu[i].t_ms <= ppg[j].t_ms);
        if (take_imu) {
            processor.feedImu(&imu[i].sample, 1);
            now = imu[i].t_ms;
            ++i;
        } else {
            processor.feedPpg(&ppg[j].sample, 1);
            now = ppg[j].t_ms;
            ++j;
        }
        while (processor.step(now, out)) {  // same catch-up pattern as wire_f02
            emit(out);
        }
    }

    // Stream end: drain any window the final samples already crossed. NO
    // synthetic time advance — a replay must close exactly the windows this
    // data can close; manufacturing rows past the recording would fabricate
    // features that never existed (00 §1: never fake data).
    for (int step_count = 0; step_count < kMaxFlushSteps; ++step_count) {
        if (!processor.step(now, out)) {
            return 0;  // settled
        }
        emit(out);
    }
    std::fprintf(stderr, "replay: flush did not settle after %d steps\n", kMaxFlushSteps);
    return 3;
}
