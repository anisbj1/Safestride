#pragma once

// firmware/src/app/wiring/wire_storage.h — F18's wiring pair (convention:
// src/app/wiring/README.md; owned per F18 spec §2 Owns).
//
// Construction, dependency injection and task creation only, no behavior
// (01 §1 rule 3). The concrete LittleFS adapter is device-only; the stores
// are pure logic and compile for both esp32dev and native.
//
// Consumers NEVER include these headers for data: F17/F20/F16 reach the log
// through bw::IEventStore, settings blobs through bw::IKeyValueStore, and
// F23 reads storage health through bw::IStorageHealthProvider (CR-015) —
// handed out here at composition-root time (01 §1 rule 3).

#include <bw/storage/event_store.h>
#include <bw/storage/kv_store.h>
#include <bw/storage/window_store.h>

namespace app {
struct AppContext;
}

namespace f18 {

// Constructs the F18 graph (LittleFsAdapter + the three stores + shared
// counters) and creates StorageTask (runtime.md §3.2: core 0, event-driven,
// isolated from real-time tasks — flash writes stall other flash users).
// Idempotence contract: call once, in the "storage" slot of the README's
// fixed call order. The task starts immediately and idles at
// kStorageIdlePollMs until the first op is queued; mount() has already run
// inside wire() so appends are safe from the start.
void wire(app::AppContext& ctx);

// StorageTask body: the FreeRTOS shell around bw::storage::LittleFsEventStore
// — drains the op queue (D3) and flushes the due sync checkpoint (D5). This
// task owns the EVENT LOG's flash writes; KV (put) and window (putWindow)
// have no queue and are synchronous in the caller's task, so their callers
// must be non-RT tasks (F18 §5). `arg` is the AppContext (for clock + fault
// sink).
void storageTask(void* arg);

// Composition-root accessors: tests, diagnostics and future wiring (F17's
// sink binding, F20's sync reader, F23's health consumer) reach the stores
// through these — never by including F18 internals from another feature.
bw::storage::LittleFsEventStore& eventStore();
bw::storage::KvStore& kvStore();
bw::storage::WindowStore& windowStore();
bw::IStorageHealthProvider& healthProvider();

// --- StorageTask constants (F18 spec §15.4) ---------------------------------

// [DERIVED] Priority 2: below every real-time task (Sensor/Processing/Alert/
// Input on core 1, Supervisor on core 0) and below F19's CommsTask — storage
// is deliberately the lowest-latency-insensitive writer (runtime.md §3.2
// "low"; wave budget: new tasks <= 3).
constexpr uint32_t kStorageTaskPriority = 2;

// [EXPERIMENTAL] Stack bytes: the pump path holds one StorageOp (64 B) plus
// per-call encode/decode scratch on the STACK (76 B record buffer, moved off
// SegmentManager by audit F-02 so a concurrent readFrom shares no buffer) and
// FreeRTOS frames — roughly 400 B of live data, so 4 KB is ~10x headroom.
// Re-measure with uxTaskGetStackHighWaterMark at Validate.
constexpr uint32_t kStorageTaskStackBytes = 4096;

// [runtime.md §3.2] Core 0 — flash writes stall both cores' code fetch, so
// the writer lives with BLE/supervisor off the real-time core (confirmed by
// integrator 2026-10-12; runtime.md wins over the W2 core-1 convention).
constexpr uint32_t kStorageTaskCore = 0;

// [EXPERIMENTAL] Idle poll: the queue is drained immediately while ops flow;
// when idle the task sleeps this long, bounding sync-checkpoint flush
// latency (kSyncCheckpointIntervalMs = 5000 >> 1000) and idle CPU to ~0.
constexpr uint32_t kStorageIdlePollMs = 1000;

}  // namespace f18
