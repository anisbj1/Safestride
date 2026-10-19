// wire_explain — F22 composition-root seam (F22 §7).
//
// Consumers (F12 OLED, F14/F17 event text) include THIS header only and call
// through it, so bw_explain stays replaceable behind one include; the seam is
// stateless — no tasks, no buffers, no bindings — so app.cpp has nothing to
// instantiate, the integrator only includes it (F22 wave 3: no app.* edits).
#pragma once

#include <bw/explain/explain_short.h>
#include <bw/explain/reason_catalog.h>

namespace app {

// Stateless pass-through to bw::explain. Static members on purpose: there is
// no state to own, so the composition root needs no instance; a later consumer
// that wants dependency injection can wrap this without changing call sites.
// Rendering constants (kExplainShortBufSize, kMaxShortReasons, ...) stay in
// bw::explain and are reachable through this header alone.
struct WireExplain {
    // snprintf semantics, caller-owned buffer: see bw::explain::explainShort.
    static size_t explainShort(bw::ReasonMask mask, char* buf, size_t cap);
    // nullptr when the code has no catalog entry (never a silent placeholder).
    static const char* reasonText(bw::ReasonCode code);
    // bw::explain::kReasonCatalogVersion — the companion (W5) compares this
    // with its own catalog before rendering full explanations.
    static uint16_t catalogVersion();
};

}  // namespace app
