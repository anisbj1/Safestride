#include "wire_storage.h"
#include "task_health.h"

#include "../app.h"

#include <bw/storage/littlefs_adapter.h>  // the only platform-flavoured piece

#if defined(ARDUINO)
#include <Arduino.h>  // FreeRTOS task API for StorageTask (device build only)
#endif

namespace f18 {

namespace {

// Static-storage graph (no heap: constructed before any task runs). The
// adapter is the only platform-flavoured piece; the stores are pure logic
// (bw/storage) so the host suite drives the same code with RamFsFake
// (01 §1 rule 3).
bw::storage::LittleFsAdapter s_adapter{};
bw::storage::StorageCounters s_counters{};
bw::storage::LittleFsEventStore s_store{s_adapter, s_counters};
bw::storage::KvStore s_kv{s_adapter, s_counters};
bw::storage::WindowStore s_windows{s_adapter, s_counters};

#if defined(ARDUINO)
// F-07: producer-side critical section. append (F17), setLabel (input) and
// markSyncedThrough (F20) run in different tasks, so the SPSC op queue needs
// one producer at a time — this guard serializes them, keeping the queue
// effectively single-producer from its own point of view. Sections are
// RAM-only and never held across flash IO (D3). The host suite has no tasks
// and attaches its own counting guard instead.
portMUX_TYPE s_queue_mux = portMUX_INITIALIZER_UNLOCKED;

struct CriticalSectionGuard final : bw::storage::IQueueGuard {
    void enter() override { portENTER_CRITICAL(&s_queue_mux); }
    void leave() override { portEXIT_CRITICAL(&s_queue_mux); }
};

CriticalSectionGuard s_queue_guard{};
#endif

}  // namespace

void storageTask(void* arg) {
    app::tasks::waitForStartup();
    app::AppContext& ctx = *static_cast<app::AppContext*>(arg);

    for (;;) {
        // Clock may not be wired yet: pump(0) still drains the queue and
        // flushes a dirty marker immediately (honest "no timeline" rule).
        const bw::TimeMs now = (ctx.clock != nullptr) ? ctx.clock->nowMs() : 0;
        const size_t applied = s_store.pump(now);
        // Event-driven: no periodic release deadline to miss. Stack and
        // progress are measured; periodic wake lateness is not applicable.
        app::tasks::completeCycle(bw::diag::TaskSlot::Storage, 0);
        if (applied == 0) {
            // Event-driven with a bounded idle poll (runtime.md §3.2; no
            // delay() — vTaskDelay is the FreeRTOS sleep, never a busy wait).
            vTaskDelay(pdMS_TO_TICKS(kStorageIdlePollMs));
        }
    }
}

void wire(app::AppContext& ctx) {
#if defined(ARDUINO)
    // Attach fault reporters BEFORE mount: mount's fallback format (F-03)
    // reports StorageDegraded + counts format_recoveries, and a boot that
    // restarts the medium empty must not pass silently (§10 "restarts empty"
    // is honest only if someone is told).
    s_store.attachFaultReporter(ctx.faults);
    s_kv.attachFaultReporter(ctx.faults);
    s_windows.attachFaultReporter(ctx.faults);
    // F-07: producer guard attached before any producer task can exist.
    s_store.attachQueueGuard(&s_queue_guard);
    // Mount + recovery scan BEFORE the task starts: seq watermark, torn-tail
    // truncation and the sync cursor are all settled by the time the first
    // append can be queued. The fs mounts read-only first; only a failed
    // read-only mount falls back to format (F-03, reported above — §10: an
    // unreadable fs restarts empty with a fault, it never bricks boot).
    (void)s_store.mount();
    (void)s_windows.mount();
    app::tasks::checkCreated(
        xTaskCreatePinnedToCore(storageTask, "F18Store", kStorageTaskStackBytes, &ctx,
                                kStorageTaskPriority, nullptr, kStorageTaskCore),
        bw::diag::TaskSlot::Storage, ctx.faults);
#else
    (void)ctx;
#endif
}

bw::storage::LittleFsEventStore& eventStore() {
    return s_store;
}

bw::storage::KvStore& kvStore() {
    return s_kv;
}

bw::storage::WindowStore& windowStore() {
    return s_windows;
}

bw::IStorageHealthProvider& healthProvider() {
    return s_store;
}

}  // namespace f18
