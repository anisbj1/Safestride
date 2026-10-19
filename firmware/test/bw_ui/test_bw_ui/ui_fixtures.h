#pragma once

// test/bw_ui/test_bw_ui/ui_fixtures.h — shared fixtures for the F12 suite.
//
// Host-side plumbing only: a battery fake (02 §3 IBatteryMonitor is a
// contract interface, so a private test fake is legal — host-testing guidance:
// "private helpers may live under your own test/ folder"), builders for the
// bw_core value types, and a golden-file loader that resolves the path
// relative to the PlatformIO project directory (firmware/).

#include <cstdio>
#include <cstring>

#include <unity.h>

#include <bw/alerts.h>
#include <bw/clock.h>
#include <bw/containers/latest_value.h>
#include <bw/features.h>
#include <bw/hardware.h>
#include <bw/providers.h>
#include <bw/quality.h>
#include <bw/state.h>
#include <bw/testkit/fake_clock.h>

namespace bw_ui_test {

// Fake battery: tests flip `reading` between a valid 87 % value and the
// A-04 bench case (valid == false, no divider seen).
struct FakeBattery final : bw::IBatteryMonitor {
    bw::BatteryReading reading{false, 0.0f, 0.0f, false, false};

    bw::BatteryReading read() override { return reading; }

    static FakeBattery atPercent(float percent) {
        FakeBattery battery;
        battery.reading = bw::BatteryReading{true, 3.9f, percent, true, false};
        return battery;
    }

    static FakeBattery absent() { return FakeBattery{}; }
};

// Fake confirm-prompt provider (CR-016, backlog 27): implements the bw_core
// contract directly, NOT F14's manager — F12 must stay ignorant of F14's
// internals (01 §1 rule 1), and IConfirmPromptProvider is the whole surface it
// is allowed to see. Tests flip `state` and `has_state` to script any phase.
struct FakePromptProvider final : bw::IConfirmPromptProvider {
    bw::ConfirmPromptState state{};
    bool has_state = false;

    bool latest(bw::ConfirmPromptState& out) const override {
        if (!has_state) {
            return false;
        }
        out = state;
        return true;
    }

    // An Offered session with a deadline `seconds_left` from `now`.
    static FakePromptProvider offered(uint32_t seq, bw::TimeMs now, bw::TimeMs seconds_left,
                                      uint8_t queue_depth = 0) {
        FakePromptProvider provider;
        provider.has_state = true;
        provider.state.seq = seq;
        provider.state.phase = bw::ConfirmPromptPhase::Offered;
        provider.state.offer_deadline_ms = now + seconds_left * 1000;
        provider.state.queue_depth = queue_depth;
        return provider;
    }

    // A Deferred session: no deadline (the contract says 0 unless Offered).
    static FakePromptProvider deferred(uint32_t seq, uint8_t queue_depth = 0) {
        FakePromptProvider provider;
        provider.has_state = true;
        provider.state.seq = seq;
        provider.state.phase = bw::ConfirmPromptPhase::Deferred;
        provider.state.offer_deadline_ms = 0;
        provider.state.queue_depth = queue_depth;
        return provider;
    }

    // Phase Idle: a session that ended (withdrawn, timed out or answered).
    static FakePromptProvider idle() {
        FakePromptProvider provider;
        provider.has_state = true;
        provider.state.phase = bw::ConfirmPromptPhase::Idle;
        return provider;
    }

    // No state ever published (latest() returns false) — the W2 default.
    static FakePromptProvider empty() { return FakePromptProvider{}; }
};

// Builders — every field a UI screen reads is explicit; the rest defaults to
// zero, which is a tested "honest default" state, not an accident.
inline bw::StateSnapshot snapshot(bw::AlertnessState alertness, bw::WearState wear) {
    bw::StateSnapshot out{};
    out.t_ms = 1000;
    out.alertness = alertness;
    out.wear = wear;
    out.activity = bw::ActivityContext::Unknown;
    out.quality = bw::SourceQuality{bw::DataQuality::Good, bw::DataQuality::Good};
    out.mode = bw::OperatingMode::Day;
    out.power = bw::PowerProfile::Normal;
    out.in_state_ms = 0;
    out.confidence = 0.8f;
    out.reasons = 0;
    return out;
}

// quality is not part of the home rendering rule since the 2026-10-15
// developer reversal (plain number for any fresh plausible rhythm), but
// the parameter stays so tests can PROVE a non-Good quality renders
// identically — quality must never change the home HR row (§10).
inline bw::PpgFeatures ppgHr(float bpm, bw::TimeMs t_ms,
                             bw::DataQuality quality = bw::DataQuality::Good) {
    bw::PpgFeatures out{};
    out.t_ms = t_ms;
    out.quality = quality;
    out.hr_valid = true;
    out.hr_bpm = bpm;
    out.hr_confidence = 0.9f;
    return out;
}

inline bw::PpgFeatures ppgHrInvalid(bw::TimeMs t_ms) {
    bw::PpgFeatures out{};
    out.t_ms = t_ms;
    out.quality = bw::DataQuality::Insufficient;
    out.hr_valid = false;
    return out;
}

inline bw::AlertStatus alert(bw::AlertLevel level, bool acknowledged = false) {
    bw::AlertStatus out{};
    out.t_ms = 1000;
    out.level = level;
    out.acknowledged = acknowledged;
    out.reasons = 0;
    out.since_ms = 0;
    return out;
}

inline bw::HealthSnapshot health(bw::DataQuality imu, bw::DataQuality ppg_quality) {
    bw::HealthSnapshot out{};
    out.t_ms = 1000;
    out.quality = bw::SourceQuality{imu, ppg_quality};
    out.battery_percent = 87;
    out.battery_valid = true;
    out.min_free_heap = 123456;
    out.worst_task_lateness_ms = 0;
    out.ble_connected = false;
    out.fault_bits = 0x5;
    out.degraded_mode = false;
    return out;
}

inline bw::Command command(bw::CommandType type) {
    bw::Command out{};
    out.type = type;
    out.t_ms = 0;
    out.source = 0;
    out.arg0 = 0;
    out.arg1 = 0;
    return out;
}

// Golden loader. PlatformIO runs the native test binary with the project
// directory (firmware/) as CWD; a couple of fallbacks keep the suite robust
// if a runner ever changes that. Missing golden = loud failure, never a
// silently skipped comparison. '\r' is stripped on load because this repo
// checks out with core.autocrlf=true and no .gitattributes: the COMPARISON
// stays exact byte-for-byte modulo line endings, so a golden is identical on
// Windows and Linux checkouts.
inline void loadGolden(const char* name, char* buf, size_t cap) {
    static const char* const kCandidates[] = {
        "test/bw_ui/golden/",
        "../test/bw_ui/golden/",
        "../../test/bw_ui/golden/",
        "golden/",
    };
    for (size_t i = 0; i < sizeof(kCandidates) / sizeof(kCandidates[0]); ++i) {
        char path[256];
        std::snprintf(path, sizeof(path), "%s%s.log", kCandidates[i], name);
        if (std::FILE* file = std::fopen(path, "rb")) {
            size_t read = std::fread(buf, 1, cap - 1, file);
            std::fclose(file);
            buf[read] = '\0';
            size_t kept = 0;  // in-place CRLF -> LF normalization
            for (size_t k = 0; k < read; ++k) {
                if (buf[k] != '\r') {
                    buf[kept++] = buf[k];
                }
            }
            buf[kept] = '\0';
            return;
        }
    }
    TEST_FAIL_MESSAGE("golden file not found (run tests from the firmware/ directory)");
}

}  // namespace bw_ui_test
