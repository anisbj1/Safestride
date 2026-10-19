#include "wire_ble.h"
#include "task_health.h"

#if defined(ARDUINO)

#include <Arduino.h>  // FreeRTOS task API for CommsTask (device build only)

#include <bw/ble/ble_server.h>
#include <bw/ble/connection_monitor.h>
#include <bw/ble/event_notifier.h>

#endif

// Host + device (spec D12): the transport gate is pure, so the binding pass
// below is compiled AND executed by the host wiring test - the guard for
// the M4 bench bug where bindMonitor() was never called anywhere.
#include <bw/ble/sync_transport.h>

#include "../app.h"

namespace f19 {

namespace {

// P4 seams (wire_ble.h): the AppContext slots AppContext does not carry yet.
// nullptr providers are honest - F19 counts refusals, never fabricates data.
bw::IStateProvider* s_state = nullptr;
bw::IFeatureProvider* s_features = nullptr;
bw::IBatteryMonitor* s_battery = nullptr;
bw::ISettings* s_settings = nullptr;
bw::ICommandSink* s_commands = nullptr;
bw::IEventStore* s_events = nullptr;
const char* s_hw_rev = "unknown";
const char* s_schema_versions = "unknown";

bool s_wired = false;

// Static-storage graph (constructed at startup, no heap - 00 7.2). The
// transport is pure (spec D12) and therefore exists on the host too, where
// it is never exposed - the getters stay honest nullptrs (tested).
bw::ble::SyncTransport s_transport;

// The link source the transport observes (M4 bench bug 1). Device: the
// ConnectionMonitor, set in wire(); host: no radio exists, so it stays
// nullptr - the wiring test swaps a fake into this same slot. Together
// with bindMonitor(s_link) in applyBindings() this replaces the old
// device-only bind whose absence made linkUp() false on every device.
bw::ble::ISyncLink* s_link = nullptr;

#if defined(ARDUINO)

bw::ble::ConnectionMonitor s_monitor;
bw::ble::EventNotifier s_notifier;
bw::ble::BleServer s_server;
bw::ble::BleServer::Deps s_deps;

#endif  // ARDUINO

// Idempotent re-bind: wire() and the attach* seams may run in either order
// (README: wiring is construction + injection only), so every seam funnels
// through here. Host + device on purpose (spec D12): the transport->link
// binding below is the seam F20's linkUp() depends on, so it must live in a
// pass the HOST wiring test executes (H10 revert guard - if this line ever
// disappears, test_wire_transport_binds_link_slot fails on the PC).
void applyBindings() {
    // M4 bench finding #1: the engine-side transport must observe the link
    // monitor or linkUp() is false forever and F20 can never send.
    s_transport.bindMonitor(s_link);
#if defined(ARDUINO)
    s_deps.state = s_state;
    s_deps.features = s_features;
    s_deps.battery = s_battery;
    s_deps.settings = s_settings;
    s_deps.commands = s_commands;
    s_deps.events = s_events;
    s_deps.identity.hw_rev = s_hw_rev;
    s_deps.identity.schema_versions = s_schema_versions;
    s_server.bind(s_deps, &s_monitor, &s_transport, &s_notifier, bw::ble::Config{});
#endif
}

}  // namespace

void attachProviders(bw::IStateProvider* state, bw::IFeatureProvider* features,
                     bw::IBatteryMonitor* battery, bw::ISettings* settings,
                     bw::ICommandSink* commands, bw::IEventStore* events) {
    s_state = state;
    s_features = features;
    s_battery = battery;
    s_settings = settings;
    s_commands = commands;
    s_events = events;
#if defined(ARDUINO)
    s_notifier.bind(s_events);  // nullptr store = honest "nothing to notify"
#endif
    applyBindings();
}

void attachIdentity(const char* hw_rev, const char* schema_versions) {
    s_hw_rev = hw_rev != nullptr ? hw_rev : "unknown";
    s_schema_versions = schema_versions != nullptr ? schema_versions : "unknown";
    applyBindings();
}

void wire(app::AppContext& ctx) {
    if (s_wired) {
        return;  // idempotent: a second wire() must not spawn a second task
    }
    s_wired = true;
#if defined(ARDUINO)
    // Bind the AppContext-owned inputs; the P4 provider seams may still
    // arrive later (applyBindings re-runs then).
    s_link = &s_monitor;  // M4 bench bug 1: the transport observes the monitor
    s_deps.faults = ctx.faults;
    s_deps.clock = ctx.clock;
    s_deps.identity.firmware_rev =
        ctx.firmware_version != nullptr ? ctx.firmware_version : "unknown";
    s_monitor.setup(&s_transport.inbound(), ctx.clock);
    s_notifier.bind(s_events);
    applyBindings();

    // The task is the input pump (H7): it starts even with nothing attached
    // yet and idles on its timed queue wait until the first provider binds.
    app::tasks::checkCreated(xTaskCreatePinnedToCore(commsTask, "CommsTask", kCommsTaskStackBytes,
                                                     &ctx, kCommsTaskPriority, nullptr,
                                                     kCommsTaskCore),
                             bw::diag::TaskSlot::Comms, ctx.faults);
#else
    applyBindings();  // same binding pass as the device (no radio: no link
                      // source, honest null - tested)
    (void)ctx;        // no BLE stack on the host: construction-free, honest nullptrs
#endif
}

#if defined(ARDUINO)

void commsTask(void* arg) {
    app::tasks::waitForStartup();
    app::AppContext& ctx = *static_cast<app::AppContext*>(arg);
    const TickType_t period_ticks = pdMS_TO_TICKS(bw::ble::kCommsTickMs);
    TickType_t next_wake = xTaskGetTickCount();

    for (;;) {
        const uint32_t late_ms = app::tasks::wakeLatenessMs(next_wake);
        const bw::TimeMs now = ctx.clock != nullptr ? ctx.clock->nowMs() : 0;

        // Drain everything the host task queued (bounded, non-blocking),
        // then run the cadence work of this tick.
        bw::ble::BleAction action{};
        while (s_monitor.popAction(&action, 0)) {
            s_server.handleAction(action, now);
        }
        s_server.tick(now);
        app::tasks::completeCycle(bw::diag::TaskSlot::Comms, late_ms);

        // Absolute-deadline pacing (runtime 3.2: no delay(), vTaskDelayUntil
        // semantics) with an action-queue wait as the sleep: a phone write
        // wakes the pump immediately, and a late wake only shifts itself.
        next_wake += period_ticks;
        TickType_t remaining = next_wake - xTaskGetTickCount();  // wraps if late
        if (remaining > period_ticks) {
            next_wake = xTaskGetTickCount() + 1;  // late: resync, never spiral
            remaining = 1;
        }
        if (s_monitor.popAction(&action, static_cast<uint32_t>(remaining) * portTICK_PERIOD_MS)) {
            // Woken early by a write: handle now with a fresh timestamp.
            const bw::TimeMs t = ctx.clock != nullptr ? ctx.clock->nowMs() : 0;
            s_server.handleAction(action, t);
        }
    }
}

#endif  // ARDUINO

bw::ISyncTransport* syncTransport() {
#if defined(ARDUINO)
    return s_wired ? &s_transport : nullptr;
#else
    return nullptr;  // no BLE on the host - F20 tests against a fake (CR-019)
#endif
}

bw::IBleLinkProvider* linkProvider() {
#if defined(ARDUINO)
    return s_wired ? &s_monitor.provider() : nullptr;
#else
    return nullptr;
#endif
}

bw::ble::BleServer* server() {
#if defined(ARDUINO)
    return s_wired ? &s_server : nullptr;
#else
    return nullptr;
#endif
}

}  // namespace f19
