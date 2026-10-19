#include "wire_confirm.h"

#include "../app.h"

namespace f14 {

namespace {

// Static-storage instance (no heap: construction happens before any task runs,
// embedded conventions). Defaults live in bw::confirm's named constants
// (F14 §8 / plan §8).
bw::confirm::ConfirmationManager s_manager{};

}  // namespace

void wire(app::AppContext& ctx) {
    // Construction only — the manager is already built with the F14 §8
    // defaults at static init. Nothing else to inject from AppContext here:
    // the ack seam and the clock are read per call by step(ctx), and the
    // F17/F18 slots arrive in a later binding call (plan §15).
    (void)ctx;
}

void step(app::AppContext& ctx) {
    // F10 ack seam (plan D3): the app-owned alert_status mailbox IS the
    // binding — LatestValue::get is edge-safe for us because onAlertStatus
    // latches the ack edge and ignores repeats (D1).
    bw::AlertStatus status{};
    if (ctx.alert_status.get(status)) {
        s_manager.onAlertStatus(status);
    }

    if (ctx.clock == nullptr) {
        return;  // honest wait: no fabricated timeline (F09 pump rule)
    }
    s_manager.step(ctx.clock->nowMs());
}

void onEvent(const bw::Event& e) {
    s_manager.onEvent(e);
}

void onAlertStatus(const bw::AlertStatus& status) {
    s_manager.onAlertStatus(status);
}

bool onCommand(const bw::Command& c) {
    return s_manager.onCommand(c);
}

void attachEventStore(bw::IEventStore* store) {
    s_manager.setEventStore(store);
}

void attachEventSink(bw::IEventSink* sink) {
    s_manager.setEventSink(sink);
}

bw::confirm::ConfirmationManager& manager() {
    return s_manager;
}

}  // namespace f14
