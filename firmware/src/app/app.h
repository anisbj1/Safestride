#pragma once

#include <bw/alerts.h>
#include <bw/clock.h>
#include <bw/commands.h>
#include <bw/containers/latest_value.h>
#include <bw/containers/spsc_queue.h>
#include <bw/context.h>
#include <bw/fault.h>
#include <bw/features.h>
#include <bw/hardware.h>
#include <bw/model.h>
#include <bw/providers.h>
#include <bw/state.h>

namespace app {

// AppContext — the composition root's aggregate (01 §1 rule 3, 01 §2 wiring
// convention; F00 D9). A plain aggregate: no logic, no methods, no virtuals — so
// it is not a contract type and needs no schema version.
//
// Minimal but not empty at M0 (D9): only what M0 already mandates. Everything
// else is added by the integrator as features land; feature implementations only ever
// receive a reference to this struct through their wire() function, never by
// editing this file.
struct AppContext {
    // 01 §4 makes monotonic time available to every feature; 02 §2 defines IClock.
    // nullptr until the DEVICE implementation is assigned — open cross-feature
    // question in F00 §13 (a naive millis() is 32-bit and wraps at ~49.7 days, so
    // this is not a trivial wrapper): F01 or the integrator must supply it before
    // M1. Features must handle "no clock" until then.
    bw::IClock* clock;

    // One source of truth for the version banner (DoD §14) and the BLE Device Info
    // characteristic (02 §6.2): F19 reads it through wiring, never by including an
    // L5 header (01 §1).
    const char* firmware_version;

    // Fault sink handed to features at wire() time (02 §5: IFaultReporter is
    // write-only from a feature's side; F26's registry is the eventual owner).
    // nullptr until the integrator assigns one — F01's drivers take it through
    // their wire() and drop reports while it is null (F01 D7 fallback), so no
    // feature may assume a sink exists.
    bw::IFaultReporter* faults;

    // F01 -> F02 raw-sample input queues (F02 D10, developer-confirmed
    // 2026-10-02; fields added by the integrator 2026-10-04 because app.h is
    // integrator-owned, F00 D9). Single-sample messages, ordered.
    // Capacities are F02's §8 minimums: 64 = one 1 s consumer period at 50 Hz
    // + 28 % jitter [DERIVED]; 128 = same rule at 100 Hz [DERIVED] (≈5 KB
    // total, F02 §11). Producer = F01 SensorTask (D15), consumer = F02
    // ProcessingTask; push overflow is refused per CR-004 and recorded by
    // SpscQueue::drops() — the sensor task never blocks on a full queue.
    bw::SpscQueue<bw::ImuSample, 64> imu_samples;
    bw::SpscQueue<bw::PpgSample, 128> ppg_samples;

    // F02 -> downstream feature mailboxes (F02 spec §3 outputs: MotionFeatures
    // and PpgFeatures, 1 Hz LatestValue; F03 spec §3: consumes MotionFeatures,
    // produces ActivityEstimate). Added by the integrator 2026-10-08 as the
    // task-table step of the F01+F02+F03 pipeline integration (F00 D9: app.h
    // grows as features land — same non-contract treatment as the D10 queues
    // above; the contract TYPES live in bw_core and are unchanged, so no CR).
    // Hand-off map: motion = F02 ProcessingTask → F03 pump (sole reader); ppg =
    // F02 → nobody yet (F05/F09 consumers arrive with those features);
    // activity = F03 pump → bench [PIPE] diagnostics today, F07/F10 later.
    // LatestValue set()/get() hold the platform critical section, so writer and
    // reader may live on different tasks; overwrites() is F23's data-loss
    // counter (CR-004 sense).
    bw::LatestValue<bw::MotionFeatures> motion_features;
    bw::LatestValue<bw::PpgFeatures> ppg_features;
    bw::LatestValue<bw::ActivityEstimate> activity_estimate;

    // W1 (2026-10-11, integrator) — mailboxes added as W1 features landed;
    // same non-contract treatment as above (the contract TYPES live in bw_core
    // and are unchanged, so no CR; F00 D9). Hand-off map: wear = f04 pump
    // (deferred) -> f07; alertness = f06 pump (deferred) -> f07 (CR-010's
    // `reasons` field travels inside AlertnessProbabilities); state_snapshot =
    // f07 StateTask -> f10/f12/f28 consumers; alert_status = f10 (deferred
    // pump) -> f11/f12; commands = f11 InputTask -> f10 (one-tick conversion
    // is the consumer's job, non-blocking backlog 4). Until the f04/f06/f10
    // pumps exist, the mailboxes stay empty — a tested state (hasValue()
    // false), and f07 honestly reports Unknown/InsufficientEvidence.
    bw::LatestValue<bw::WearEstimate> wear_estimate;
    bw::LatestValue<bw::AlertnessProbabilities> alertness_probabilities;
    bw::LatestValue<bw::StateSnapshot> state_snapshot;
    bw::LatestValue<bw::AlertStatus> alert_status;
    bw::SpscQueue<bw::Command, 16> commands;

    // Demo wakefulness-check response (F11 -> F06/F07). A short press during
    // the silent/Warning check publishes its monotonic timestamp once; F06
    // clears the armed motion trigger and F07 records Awake. This is an
    // app-local coordination mailbox using existing bw_core types, not a
    // persisted/transmitted contract or a second consumer of ctx.commands.
    bw::LatestValue<bw::TimeMs> wake_check_passed_ms;

    // W2 P4 (2026-10-12, integrator) — the two W2 features whose outputs
    // have consumers but no ctx channel yet; same non-contract treatment as
    // above (the contract TYPES live in bw_core and are unchanged, so no CR;
    // F00 D9). episode_status = f08 StateTask -> F11/F12 consumers (CR-007,
    // written only on status_changed — F08 §7); health = f23 DiagTask ->
    // f12 (wire_ui.cpp's SFINAE seam binds it the moment this field exists;
    // F23's D5 file-static accessors stay the primary seam for other
    // consumers).
    bw::LatestValue<bw::EpisodeStatus> episode_status;
    bw::LatestValue<bw::HealthSnapshot> health;
};

// Composition root entry point: prints the version banner, wires features and
// creates tasks. Owned by the current integrator after F00.          [F00 skeleton]
void run();

}  // namespace app
