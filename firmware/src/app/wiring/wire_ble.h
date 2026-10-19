#pragma once

// firmware/src/app/wiring/wire_ble.h - F19's wiring pair (convention:
// src/app/wiring/README.md; the file name wire_ble.* is F19 spec 2 Owns).
//
// Construction, dependency injection and task creation only - no logic
// (01 1 rule 3). The BLE graph is device-only (NimBLE) except the pure
// transport gate (spec D12), so the pair compiles on the host too, where
// wire() runs the same binding pass with no link source and every getter
// is an honest nullptr (both are tested states - see the wiring compile
// suite; README 2 "where meaningful").

#include <bw/hardware.h>
#include <bw/providers.h>
#include <bw/sync_transport.h>
#include <bw/types.h>

namespace bw::ble {
class BleServer;
}

namespace app {
struct AppContext;
}

namespace f19 {

// Constructs the F19 graph (BleServer + ConnectionMonitor + SyncTransport +
// EventNotifier over the CR-018 mailbox), binds what AppContext already
// carries (clock, faults, firmware_version) and creates CommsTask
// (runtime.md 3.2: core 0, event-driven - the BLE stack lives off the
// real-time core, DEC-008). Idempotence contract: call once, in the
// README's fixed "comms" slot; providers still missing at that point may
// arrive later through attachProviders / attachIdentity (P4 seams).
void wire(app::AppContext& ctx);

// CommsTask body (spec 18 / H7): THE input pump - drains the NimBLE action
// queue with a timeout equal to the time left to the next kCommsTickMs
// deadline (a phone write wakes the pump immediately, the cadence grid
// stays absolute), then runs one BleServer tick. Created by wire(); `arg`
// is the AppContext. Never blocks without a timeout (00 7.2).
void commsTask(void* arg);

// --- P4 seams (integrator calls from app.cpp) ------------------------------

// Provider pointers AppContext does not carry yet (spec 18 / spec 3):
// nullptr providers are honest - F19 counts refusals and fabricates nothing
// (00 1). May be called before or after wire(); binding is idempotent.
void attachProviders(bw::IStateProvider* state, bw::IFeatureProvider* features,
                     bw::IBatteryMonitor* battery, bw::ISettings* settings,
                     bw::ICommandSink* commands, bw::IEventStore* events);

// Device Information strings beyond the firmware version banner (app.h only
// carries the version): hw revision and the schema-versions string, both
// build facts the app layer owns (D4, spec 18). "unknown" until called.
void attachIdentity(const char* hw_rev, const char* schema_versions);

// --- Composition-root getters (spec 18; integrator wires consumers) --------
// Consumers never include F19 headers for DATA (01 1 rule 1): F20's engine
// receives the ISyncTransport seam (CR-019), F23/F12 receive the CR-018
// IBleLinkProvider, F26 takes the BleServer* for its restart policy (D11).
// All three are nullptr until wire() has run - and on the host, forever
// (no radio there).
bw::ISyncTransport* syncTransport();
bw::IBleLinkProvider* linkProvider();
bw::ble::BleServer* server();

// ---- CommsTask constants (runtime.md 3.2 task registry; spec 8/18) --------

// [EXPERIMENTAL] Stack bytes - pinned by the G1 ruling (integrator, W4):
// the task frame holds one BleAction (68 B) plus per-tick codec scratch on
// the STACK (<= 48 B for the Event image + small locals); the big buffers
// (Config dump 512 B, spec table) live in BleServer's static storage, not
// on the stack. Re-measure with uxTaskGetStackHighWaterMark at DoD (spec 11).
constexpr uint32_t kCommsTaskStackBytes = 4096;

// [EXPERIMENTAL] Priority 2 - pinned by the G1 ruling: runtime.md 3.2's
// "low-med" rung of the task table. Equal to StorageTask (both tolerate
// latency); below every real-time task on core 1 and below SupervisorTask.
constexpr uint32_t kCommsTaskPriority = 2;

// [DERIVED] Core 0 - runtime.md 3.2 / DEC-008: BLE lives with the storage
// and supervisor tasks, off core 1 where alerting runs, so a phone can
// never delay an alert (00 7.1 non-negotiable).
constexpr uint32_t kCommsTaskCore = 0;

}  // namespace f19
