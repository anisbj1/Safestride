// firmware/src/app/wiring/wire_history.cpp — F16's construction only.

#include "wire_history.h"

#include "../app.h"  // AppContext: ctx.clock is read here, at construction

namespace f16 {

namespace {

// Set by wireHistoryInit(); read by the accessors. Kept separate from the
// object so the accessors are usable before any function-local static would
// otherwise be touched from another translation unit (same pattern as F06's
// wire_wake).
bw::history::HistoryAggregator* s_aggregator = nullptr;

// Last initialization status. Busy is expected while bounded restore is
// progressing; a transient KV failure is observable and retried next tick.
bw::Result<void> s_boot_status = {bw::Err::NotReady};

}  // namespace

void wireHistoryInit(app::AppContext& ctx, bw::IEventStore* events, bw::IKeyValueStore* kv) {
    // Function-local static: constructed on the first call only, so a second
    // wireHistoryInit() keeps the first instance (idempotence contract in
    // wire_history.h) and no dynamic allocation happens after startup
    // (00 §7.2). boot() only schedules the restore state machine; the
    // ServicePump advances bounded work after the startup barrier opens.
    static bw::history::HistoryAggregator instance{bw::history::HistoryConfig{}, events, kv,
                                                   ctx.clock};
    s_aggregator = &instance;
    s_boot_status = instance.boot();  // stored, not dropped (historyBootStatus)
}

bw::Result<void> historyTick() {
    if (s_aggregator == nullptr) {
        return {bw::Err::NotReady};  // wireHistoryInit() has not run yet
    }
    const bool was_ready = s_aggregator->ready();
    const bw::Result<void> result = s_aggregator->step();
    if (!was_ready) {
        s_boot_status = result;
    }
    return result;
}

bw::Result<void> historyBootStatus() {
    return s_boot_status;  // default NotReady until wireHistoryInit() runs
}

bool historyReady() {
    return s_aggregator != nullptr && s_aggregator->ready();
}

bw::history::HistoryAggregator& historyAggregator() {
    return *s_aggregator;
}

bool historyLatestDailySummary(bw::history::DailySummary& out) {
    return s_aggregator != nullptr && s_aggregator->latestDailySummary(out);
}

}  // namespace f16
