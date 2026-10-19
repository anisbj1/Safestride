#pragma once

// reliability_test_support.h — fakes and shared helpers for the F26 host
// suite (spec §16.8). Header-only on purpose: fakes are per-TU, while
// resetWiring() is DEFINED in test_wire_reliability_compile.cpp — the only
// translation unit allowed to include wire_reliability.cpp.

#include <cstddef>
#include <cstdint>

#include <bw/events.h>
#include <bw/providers.h>
#include <bw/reliability/degradation_policy.h>
#include <bw/types.h>

namespace reliability_test {

// Runs f26::wire() on a fresh AppContext — the full static reset — then
// detaches every attach* seam (wire() deliberately lets them survive, so a
// test must never inherit another test's stack fake). Returns ctx.faults,
// the F26 registry, so tests inject codes exactly as features do through
// the AppContext seam (D14).
bw::IFaultReporter* resetWiring();

// --- fakes -------------------------------------------------------------------

// Records every published event; `fail` simulates a refusing sink — the
// wiring counts those as dropped events (backlog 18 CR request).
class FakeEventSink final : public bw::IEventSink {
public:
    bw::Result<void> publish(const bw::Event& event) override {
        if (fail) {
            return {bw::Err::Full};
        }
        if (count < kMaxEvents) {
            events[count] = event;
        }
        ++count;
        return {bw::Err::Ok};
    }

    static constexpr size_t kMaxEvents = 8;
    bw::Event events[kMaxEvents]{};
    size_t count = 0;
    bool fail = false;
};

// CR-015 snapshot provider behind an explicit presence flag (absent provider
// must stay "unknown", never "down" — D5).
class FakeStorageHealth final : public bw::IStorageHealthProvider {
public:
    bool latest(bw::StorageHealth& out) const override {
        if (!present) {
            return false;
        }
        out = health;
        return true;
    }

    bw::StorageHealth health{};
    bool present = false;
};

// CR-018 snapshot provider, same presence rule (D5).
class FakeBleLink final : public bw::IBleLinkProvider {
public:
    bool latest(bw::BleLinkState& out) const override {
        if (!present) {
            return false;
        }
        out = link;
        return true;
    }

    bw::BleLinkState link{};
    bool present = false;
};

// --- value helpers -----------------------------------------------------------

// Healthy store snapshot: nothing corrupt/skipped, half the space free —
// never a row-3 trigger.
inline bw::StorageHealth healthyStorage(bw::TimeMs t_ms) {
    bw::StorageHealth h{};
    h.t_ms = t_ms;
    h.total_bytes = 1000;
    h.free_bytes = 500;
    return h;
}

// Row 3 trigger via corrupt_records > 0.
inline bw::StorageHealth corruptStorage(bw::TimeMs t_ms) {
    bw::StorageHealth h = healthyStorage(t_ms);
    h.corrupt_records = 1;
    return h;
}

// Row 3 trigger via free < kStorageDegradedFreePct (50/1000 = 5 % < 10 %).
inline bw::StorageHealth lowFreeStorage(bw::TimeMs t_ms) {
    bw::StorageHealth h = healthyStorage(t_ms);
    h.free_bytes = 50;
    return h;
}

// A fresh policy input with nothing attached: every input unknown, no flags.
inline bw::reliability::PolicyInput baseInput(bw::TimeMs t_ms) {
    bw::reliability::PolicyInput in{};
    in.t_ms = t_ms;
    return in;
}

}  // namespace reliability_test
