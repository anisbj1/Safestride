#pragma once

// firmware/src/app/wiring/wire_confirm.h — F14's wiring pair (convention:
// src/app/wiring/README.md; the file name wire_confirm.* is F14 spec §2 Owns).
//
// Construction, dependency injection and seam binding only — no logic (01 §1
// rule 3). F14 owns no task (plan §15): the manager is stepped from the app
// loop, so there are no stack/priority/period constants to declare.

#include <bw/commands.h>
#include <bw/confirm/confirmation_manager.h>
#include <bw/events.h>
#include <bw/providers.h>
#include <bw/types.h>

namespace app {
struct AppContext;
}

namespace f14 {

// Constructs the feature's manager (F14 §8 defaults) — idempotent, call once
// in the README's fixed wiring order (decision band, F07-F10 row). The static
// instance itself is built at startup (no heap); F17/F18 slots attach later
// (parallel wave) through attachEventStore / attachEventSink below.
void wire(app::AppContext& ctx);

// One pump of the state machine: reads the ack seam and drives defer / offer /
// timeout / store retries, refreshing the ConfirmPromptState provider F12
// reads. No dedicated task (plan §15) — the composition root calls this from
// its existing loop. With no clock yet it waits honestly (never a fabricated
// t = 0 timeline; F09 pump rule).
void step(app::AppContext& ctx);

// --- P4 seam bindings (plan §15): plain bw_core value types only ------------

// F17 subscriber fan-out target: an event that may need a confirmation prompt.
void onEvent(const bw::Event& e);

// F10 ack source, for callers that prefer an explicit edge over the
// AppContext::alert_status mailbox step(ctx) already reads (plan D3).
void onAlertStatus(const bw::AlertStatus& status);

// Dispatcher target: returns true iff ConfirmEvent/RejectEvent was consumed
// (arg0 = target Event::seq).
bool onCommand(const bw::Command& c);

// F18 / F17 slots (parallel wave): nullptr = labels / notifications are
// dropped and counted in labelStats(), never silently lost (spec §10).
void attachEventStore(bw::IEventStore* store);
void attachEventSink(bw::IEventSink* sink);

// Composition-root accessor: the F12 provider binding at P4 reads through
// this (bw::IConfirmPromptProvider, CR-016). Consumers never include F14
// headers — they receive state through bw_core channels (01 §1 rule 3).
bw::confirm::ConfirmationManager& manager();

}  // namespace f14
