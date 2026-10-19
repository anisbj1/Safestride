#pragma once

// firmware/src/app/wiring/wire_baseline.h — F13's wiring pair (convention:
// src/app/wiring/README.md; the file name is wire_baseline.* per F13 spec §2
// Owns). Construction, injection and the pump body declaration only — no
// filtering or decisions (01 §1 rule 3): every judgement lives in bw_baseline.

#include <bw/baseline/baseline_tracker.h>
#include <bw/containers/latest_value.h>
#include <bw/providers.h>

#include <atomic>
#include <cstdint>

namespace app {
struct AppContext;
}
namespace bw {
class IKeyValueStore;
}

namespace f13 {

// D4 publish seam. The tracker is single-writer (the F13 pump) and not
// thread-safe; F06's pump reads the 20 B BaselineProfile from another task, so
// the adapter holds bw::LatestValue — set()/get() copy the struct as one unit
// (F00 D10, no torn reads). profile() is false until the first publish, which
// is exactly F06's tested "baseline absent" case.
class BaselineProviderAdapter final : public bw::IBaselineProvider {
public:
    void publish(const bw::BaselineProfile& p) {
        box_.set(p);  // value first: a reader that sees published_ == true
        published_.store(true, std::memory_order_release);
    }

    // wire() semantics: every wire() re-constructs, so a previous run's publish
    // must not survive as a stale answer (host suites share one process).
    void reset() { published_.store(false, std::memory_order_release); }

    bool profile(bw::BaselineProfile& out) const override {
        if (!published_.load(std::memory_order_acquire)) {
            return false;
        }
        return box_.get(out);
    }

private:
    bw::LatestValue<bw::BaselineProfile> box_{};
    std::atomic<bool> published_{false};
};

// Constructs the tracker + adapter (function-local statics: idempotent, no
// heap after startup) and attempts the D3 load from `store`:
//  - store == nullptr, key NotFound, or a transient store error => in-memory
//    boot, honest (§10; no fault — nothing is broken, there is just nothing);
//  - Corrupt / Unsupported / Full (unreadable or version-incompatible blob)
//    => discard, restart learning, and report FaultCode::BaselineCorrupt
//    (CR-031, Warning) when ctx.faults is set (D6 — the CR landed at W6 G1).
// A successfully restored baseline is published immediately, before the first
// pump tick, so consumers see it even if the sensors stay silent (§10).
// Call once, in the README's intelligence slot AFTER the storage slot.
void wireWithStore(app::AppContext& ctx, bw::IKeyValueStore* store);

// Composition-root entry: wireWithStore(ctx, &f18::kvStore()) on the device
// (storage slot ran first — README call order); the honest no-store path on
// the host, where there is no composition root to resolve f18 from.
void wire(app::AppContext& ctx);

// Pump task body (D7): reads ctx.ppg_features / motion_features /
// activity_estimate / wear_estimate — skipping the tick entirely when any of
// the four is absent (never default-construct evidence) — rejects windows older
// than bw::baseline::kMaxSampleAgeMs, derives calibration/hold from the
// state/alert/episode mailboxes (D2/D5), steps the tracker, publishes the
// profile on change and persists per kPersistIntervalMs. The task is NOT
// created here: the integrator creates it at P4 from the constants below
// (spec §13 checklist ② — same deferral pattern as F06's pump before its P4).
// `arg` is the AppContext.
void pumpTask(void* arg);

// One non-blocking cycle, shared by the device task and host regression tests.
// Single writer; call wire() first. A sensor-window pair is consumed at most
// once, and all four evidence inputs must be fresh.
void pumpOnce(app::AppContext& ctx);

// Composition-root accessors, valid after wire() (01 §1 rule 3): the
// integrator injects provider() into wire_wake.cpp:105 replacing the nullptr;
// tracker() is the pump's handle and the host seam tests' (F17 sink() pattern).
bw::IBaselineProvider& provider();
bw::baseline::BaselineTracker& tracker();

// --- Pump constants (F13 spec §8; D7) ----------------------------------------

// [DERIVED] One tick per F02 1 s window (1 Hz grid): faster polling would feed
// the learner duplicate windows (§8, same rationale as F06's kWakePumpPeriodMs).
constexpr uint32_t kBaselinePumpPeriodMs = 1000;

// [EXPERIMENTAL] Stack bytes: BaselineSample (~20 B) + a profile + FreeRTOS
// frames; 2 KB matches the other feature pumps (§8). Re-measure with
// uxTaskGetStackHighWaterMark at Validate (§11).
constexpr uint32_t kBaselinePumpStackBytes = 2048;

// [DERIVED] Priority 2: below F06's pump (3) — the learner is background work
// (W2 wave convention: new tasks <= 3; §8).
constexpr uint32_t kBaselinePumpPriority = 2;

// [DERIVED] Core 0 (D7): the KV put() is synchronous and stalls flash, so the
// pump lives with storage/BLE off the real-time core (§8).
constexpr uint32_t kBaselinePumpCore = 0;

}  // namespace f13
