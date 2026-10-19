#pragma once

// firmware/src/app/wiring/wire_ml.h — F05's wiring pair (convention:
// src/app/wiring/README.md; the file name is wire_ml.* per F05 spec §2 Owns).
//
// Construction and dependency injection only, no behavior (01 §1 rule 3).

namespace app {
struct AppContext;
}

namespace f05 {

// Constructs the model from F05's designated production `model_<name>_v<N>.h`
// header and injects the generated schema constants (mean/std/id/version) into
// F06's feature assembler — FEATURE_SCHEMA.md §2.2/§4, F05 D4.
//
// Status: DEFERRED documented no-op (same pattern as f03::wire — see
// wire_activity.h). Nothing exists to construct or inject yet: DEC-006 ships
// a heuristic baseline first, and no production `model_<name>_v<N>.h` exists
// today (first exported model at M5). The consumer side has landed — F06's
// feature assembler builds `FeatureVector`s — but it reads a hand-maintained
// mirror of the schema constants (`bw_wake/alertness_schema.h`), not F05's
// generated header, so there is nothing to inject either. Wiring a fixture/toy
// model into the app would hand F06 meaningless probabilities, so this stays
// an honest no-op until the first production model header exists; the
// assembler is the waiting side of that handoff (F05 §7).
//
// When it lands, this pair gains (spec §7): the model instance built from the
// generated header plus a `bw::IModel& model()` composition-root accessor, and
// the schema-constant injection happens here at wire() time. The integrator
// then adds the single call in app.cpp's fixed order (README §Call order) —
// features never edit app.cpp themselves.
//
// Idempotence contract (same as the other wire functions): call once, in the
// intelligence slot of the fixed call order (after F03-F04, before F07-F10).
void wire(app::AppContext& ctx);

}  // namespace f05
