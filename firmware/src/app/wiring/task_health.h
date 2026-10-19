#pragma once

// Application-level task adoption seam. Each actual task is the ONLY writer
// of its F23 slot and F26 heartbeat. Never combine separately scheduled pumps.
#include "wire_diag.h"
#include "wire_reliability.h"

#include <atomic>

namespace app::tasks {

// One-way startup publication: all construction/attachment writes happen before
// release; every task acquires before touching the graph, on either core.
class StartupBarrier {
public:
    bool ready() const { return ready_.load(std::memory_order_acquire); }
    void release() { ready_.store(true, std::memory_order_release); }

private:
    std::atomic<bool> ready_{false};
};
inline StartupBarrier startup;

inline void reportTaskCreation(bool succeeded, bw::diag::TaskSlot slot, bw::IFaultReporter* faults,
                               bw::TimeMs now_ms) {
    if (succeeded || faults == nullptr) {
        return;
    }
    using bw::diag::TaskSlot;
    const bool critical =
        slot == TaskSlot::Sensor || slot == TaskSlot::Processing ||
        slot == TaskSlot::Intelligence || slot == TaskSlot::Wear || slot == TaskSlot::Wake ||
        slot == TaskSlot::State || slot == TaskSlot::Sleep || slot == TaskSlot::Cataplexy ||
        slot == TaskSlot::Alert || slot == TaskSlot::Input || slot == TaskSlot::Supervisor;
    faults->report(bw::FaultCode::TaskCreateFailed,
                   critical ? bw::FaultSeverity::Critical : bw::FaultSeverity::Error, now_ms);
}

inline void reportEventDrop(bw::IFaultReporter* faults, bw::TimeMs now_ms) {
    if (faults != nullptr) {
        faults->report(bw::FaultCode::EventPublishDrop, bw::FaultSeverity::Warning, now_ms);
    }
}

inline void recordCycle(bw::diag::TaskSlot slot, bw::TimeMs now_ms, uint32_t lateness_ms,
                        uint16_t stack_free_bytes) {
    using bw::diag::TaskSlot;
    using bw::reliability::TaskId;
    TaskId id = TaskId::Count;
    switch (slot) {
    case TaskSlot::Sensor:
        id = TaskId::Sensor;
        break;
    case TaskSlot::Processing:
        id = TaskId::Processing;
        break;
    case TaskSlot::Intelligence:
        id = TaskId::Intelligence;
        break;
    case TaskSlot::State:
        id = TaskId::State;
        break;
    case TaskSlot::Alert:
        id = TaskId::Alert;
        break;
    case TaskSlot::Input:
        id = TaskId::Input;
        break;
    case TaskSlot::Ui:
        id = TaskId::Ui;
        break;
    case TaskSlot::Storage:
        id = TaskId::Storage;
        break;
    case TaskSlot::Comms:
        id = TaskId::Comms;
        break;
    case TaskSlot::ServicePump:
        id = TaskId::ServicePump;
        break;
    case TaskSlot::DiagSelf:
        id = TaskId::Diag;
        break;
    case TaskSlot::Wear:
        id = TaskId::Wear;
        break;
    case TaskSlot::Wake:
        id = TaskId::Wake;
        break;
    case TaskSlot::Sleep:
        id = TaskId::Sleep;
        break;
    case TaskSlot::Cataplexy:
        id = TaskId::Cataplexy;
        break;
    case TaskSlot::EventLog:
        id = TaskId::EventLog;
        break;
    case TaskSlot::Baseline:
        id = TaskId::Baseline;
        break;
    case TaskSlot::Research:
        id = TaskId::Research;
        break;
    case TaskSlot::Supervisor:
        break;  // F23 observes it; it owns the WDT feed
    case TaskSlot::Count:
        return;
    default:
        return;
    }
    f23::noteTaskCycle(slot, now_ms, lateness_ms, stack_free_bytes);
    if (id != TaskId::Count) {
        f26::beat(id, now_ms);
    }
}

}  // namespace app::tasks

#if defined(ARDUINO)
#include <Arduino.h>
#include <esp_timer.h>

namespace app::tasks {

// [EXPERIMENTAL] Startup-only bounded sleep; avoids spinning/starving setup on
// core 1. No task reports evidence or feeds the watchdog while the graph builds.
constexpr uint32_t kStartupPollMs = 10;
inline void waitForStartup() {
    TickType_t wake = xTaskGetTickCount();
    while (!startup.ready()) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(kStartupPollMs));
    }
}

// [DERIVED] SI conversion, same epoch/source as F01 DeviceClock. millis()
// would truncate to 32 bits and invent stalls after its wrap on a long boot.
constexpr uint32_t kMicrosecondsPerMillisecond = 1000;
inline bw::TimeMs monotonicMs() {
    return static_cast<bw::TimeMs>(esp_timer_get_time() / kMicrosecondsPerMillisecond);
}

inline void checkCreated(BaseType_t result, bw::diag::TaskSlot slot, bw::IFaultReporter* faults) {
    reportTaskCreation(result == pdPASS, slot, faults, monotonicMs());
}

// Capture wake lateness at START, then publish only after this iteration's
// work completes. Tick subtraction is wrap-safe on the FreeRTOS tick width.
inline uint32_t wakeLatenessMs(TickType_t scheduled) {
    const int32_t late_ticks = static_cast<int32_t>(xTaskGetTickCount() - scheduled);
    return late_ticks > 0 ? static_cast<uint32_t>(late_ticks) * portTICK_PERIOD_MS : 0;
}

inline void completeCycle(bw::diag::TaskSlot slot, uint32_t lateness_ms) {
    recordCycle(slot, monotonicMs(), lateness_ms,
                f23::stackWatermarkBytes(uxTaskGetStackHighWaterMark(nullptr)));
}

// For a pump with early-continue paths, after its paced wait. No successful
// idle/empty-buffer branch may disappear from the health/heartbeat stream.
class ScopedCycle final {
public:
    ScopedCycle(bw::diag::TaskSlot slot, TickType_t scheduled)
        : slot_(slot), late_ms_(wakeLatenessMs(scheduled)) {}
    ~ScopedCycle() { completeCycle(slot_, late_ms_); }
    ScopedCycle(const ScopedCycle&) = delete;
    ScopedCycle& operator=(const ScopedCycle&) = delete;

private:
    bw::diag::TaskSlot slot_;
    uint32_t late_ms_;
};

}  // namespace app::tasks
#endif
