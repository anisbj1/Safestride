#include "wire_research.h"
#include "task_health.h"

#include "../app.h"

#include <bw/research/research_streamer.h>

#if defined(ARDUINO)
#include <Arduino.h>  // FreeRTOS task API + Serial (device build only)
#endif

namespace f28 {

namespace {

// Static-storage instances, constructed before any task runs - no heap and no
// init-order dependency (embedded conventions). Everything configurable
// (baud, depths, cadence) is a compile-time constant in
// bw/research/research_config.h (F28 spec §8).
bw::research::ResearchStreamer s_streamer{};
bw::research::RawTee s_tee{};

}  // namespace

#if defined(ARDUINO)

void researchTask(void* arg) {
    app::tasks::waitForStartup();
    app::AppContext& ctx = *static_cast<app::AppContext*>(arg);

    // Dependency injection only (01 §1 rule 3). Both provider pointers stay
    // null: AppContext exposes no IStateProvider and no IEventStore yet (§13
    // integration gaps) - the Research-mode gate is therefore flag-only and
    // E/M rows are absent on device until the integrator wires them, never
    // faked (00 §1).
    bw::research::StreamSources sources;
    sources.motion = &ctx.motion_features;
    sources.ppg = &ctx.ppg_features;
    sources.activity = &ctx.activity_estimate;
    sources.state = nullptr;
    sources.events = nullptr;
    sources.tee = &s_tee;
    sources.firmware_version = ctx.firmware_version;
    sources.boot_id = bw::research::kUnknownBootId;  // no F17 source yet (§13)
    s_streamer.setSources(sources);

    // The capture baud is only taken over once rows actually exist (§5): until
    // the first activation the app's own serial setup is left byte-for-byte
    // untouched.
    bool serial_started = false;

    // Absolute-deadline pacing (runtime.md §3.2: never delay()).
    TickType_t last_wake = xTaskGetTickCount();
    char chunk[bw::research::kDrainChunkBytes];

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(kResearchTaskPeriodMs));
        const app::tasks::ScopedCycle cycle{bw::diag::TaskSlot::Research, last_wake};

        const bw::TimeMs now = (ctx.clock != nullptr) ? ctx.clock->nowMs() : 0;
        s_streamer.runTick(now);  // format this tick's rows into the line ring

        if (!serial_started && s_streamer.headerEmitted()) {
            Serial.begin(bw::research::kSerialBaud);  // D2: 921600, see §11
            serial_started = true;
        }
        if (!serial_started) {
            continue;
        }

        // Drain first and only as far as the UART buffer already promised, so
        // this task never blocks on a full TX queue (F28 §10 drop/pause, and
        // the no-blocking rule of runtime.md §3.1). A partial write is fine:
        // the ring is FIFO, the rest follows on the next tick.
        // Throughput: one kDrainChunkBytes chunk per tick is the hardware
        // maximum (128 B ESP32 TX FIFO, no TX ring — research_config.h), so
        // sustained capacity = kDrainBytesPerSecond, static_assert-ed >= the
        // D2 worst-case stream load (F1 fixture root cause).
        const int writable = Serial.availableForWrite();
        if (writable <= 0) {
            continue;
        }
        size_t chunk_cap = sizeof(chunk);
        if (static_cast<size_t>(writable) < chunk_cap) {
            chunk_cap = static_cast<size_t>(writable);
        }
        const size_t n = s_streamer.drain(chunk, chunk_cap);
        if (n > 0) {
            Serial.write(reinterpret_cast<const uint8_t*>(chunk), n);
        }
    }
}

#endif  // ARDUINO

void wire(app::AppContext& ctx) {
    // Construction and task table only - the statics above already exist.
    //
    // Gate posture (§5/D4, spec §15 rollback): with the repo default
    // kResearchCaptureDefaultEnabled = false the function returns immediately -
    // no task is created, so the device behaves exactly as before F28
    // existed. Flipping the flag and rebuilding is the documented capture
    // procedure (§15 A3). When F24's Research mode lands, task creation
    // becomes unconditional and active() picks up OperatingMode::Research.
    if (!bw::research::kResearchCaptureDefaultEnabled) {
        (void)ctx;
        return;
    }
#if defined(ARDUINO)
    app::tasks::checkCreated(
        xTaskCreatePinnedToCore(researchTask, "ResearchTask", kResearchTaskStackBytes, &ctx,
                                kResearchTaskPriority, nullptr, kResearchTaskCore),
        bw::diag::TaskSlot::Research, ctx.faults);
#else
    (void)ctx;
#endif
}

bw::research::RawTee* rawTee() {
    // Off-path cost: one active() check (flag plus, later, the mode source).
    // nullptr means "capture off" - the A1 caller skips the copy entirely.
    return s_streamer.active() ? &s_tee : nullptr;
}

}  // namespace f28
