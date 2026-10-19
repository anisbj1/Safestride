#pragma once

// Support for the bw_sleep / F08 suite (§9): the two fakes bw_testkit does
// NOT provide — a recording IEventSink and a write-counting IKeyValueStore
// decorator — plus a scripted StateSnapshot builder. Everything deterministic:

#include <bw/events.h>
#include <bw/providers.h>
#include <bw/sleep/sleep_config.h>
#include <bw/state.h>
#include <bw/types.h>

namespace f08_test {

// Recording event sink (§9: bw_testkit has no IEventSink double, so the suite
// carries its own, owned path). Counts publish ATTEMPTS even after the buffer
// is full, and can reject with Err::Full to pin §10's "bookkeeping completes,
// the drop is not retried" behaviour.
class RecordingEventSink final : public bw::IEventSink {
public:
    static constexpr size_t kCapacity = 16;

    bw::Result<void> publish(const bw::Event& e) override {
        // Count every ATTEMPT, including rejected ones — the wiring's drop
        // counter needs the attempts, not just the successes.
        const size_t slot = count;
        ++count;
        if (reject) {
            return {bw::Err::Full};
        }
        if (slot < kCapacity) {
            events[slot] = e;
        }
        return {bw::Err::Ok};
    }

    const bw::Event& at(size_t i) const { return events[i]; }

    bw::Event events[kCapacity] = {};
    size_t count = 0;
    bool reject = false;
};

// Counting KV-store decorator (§9): bw_testkit's InMemoryKeyValueStore does
// not count writes, so the "one open put + one close put per Active episode"
// assertions wrap it here; fail_puts injects §10's storage-failure-at-
// promotion case.
template <class Inner> class CountingKvStore final : public bw::IKeyValueStore {
public:
    bw::Result<size_t> get(const char* key, void* buf, size_t cap) override {
        return inner.get(key, buf, cap);
    }

    bw::Result<void> put(const char* key, const void* data, size_t len) override {
        ++put_count;
        if (fail_puts) {
            return {bw::Err::HwFault};
        }
        return inner.put(key, data, len);
    }

    Inner inner;
    size_t put_count = 0;
    bool fail_puts = false;
};

// Scripted StateSnapshot with explicit t_ms — no real sleeping (§9). Defaults
// are a healthy worn sleep-like observation; tests vary only what they study.
inline bw::StateSnapshot
makeSnapshot(bw::TimeMs t_ms, bw::AlertnessState alertness = bw::AlertnessState::PossibleSleep,
             bw::WearState wear = bw::WearState::Worn,
             bw::OperatingMode mode = bw::OperatingMode::Day,
             bw::DataQuality imu = bw::DataQuality::Good,
             bw::DataQuality ppg = bw::DataQuality::Good, float confidence = 0.8f) {
    bw::StateSnapshot s{};
    s.t_ms = t_ms;
    s.alertness = alertness;
    s.wear = wear;
    s.activity = bw::ActivityContext::LyingResting;
    s.quality = {imu, ppg};
    s.mode = mode;
    s.power = bw::PowerProfile::Normal;
    s.in_state_ms = 0;
    s.confidence = confidence;
    s.reasons = 0;
    return s;
}

// Awake (non-sleep-like) helper: F07 reports Awake while worn.
inline bw::StateSnapshot awake(bw::TimeMs t_ms, bw::OperatingMode mode = bw::OperatingMode::Day) {
    return makeSnapshot(t_ms, bw::AlertnessState::Awake, bw::WearState::Worn, mode);
}

// NotWorn interruption: F07's NotWorn gate keeps alertness off PossibleSleep
// while unworn (§6 D1/§13) — ordinary gap time for F08, never a pause state.
inline bw::StateSnapshot removed(bw::TimeMs t_ms) {
    return makeSnapshot(t_ms, bw::AlertnessState::NotWorn, bw::WearState::NotWorn);
}

// Bit for an expected reason. bw_core defines ReasonCode but no bit helper and
// features may not share code (01 §1 rule 1) — every suite carries its own.
inline bw::ReasonMask reasonBit(bw::ReasonCode code) {
    return static_cast<bw::ReasonMask>(1u) << static_cast<uint8_t>(code);
}

}  // namespace f08_test
