#include "wire_ml.h"

// The F05 runtime and the schema constants this wiring will construct/inject
// are included even while wire() is a no-op: lib LDF only builds what is
// included, so these two lines keep bw_ml + the generated schema header
// compiling under esp32dev (DoD "compiles for esp32dev and native") and give
// the generated header's static_asserts an xtensa check. When the first
// production model header lands, the model instance is built right here
// (F05 spec §7 "Implement stage" + status log for the deferral rationale).
#include <bw/ml/feature_schema_v2.h>
#include <bw/ml/model_linear.h>
#include <bw/ml/model_tree.h>

namespace f05 {

void wire(app::AppContext&) {
    // Intentionally empty until F05's first production model header lands;
    // wiring must stay logic-free (01 §1 rule 3) and honest (F05 §10: never
    // fabricate an estimate from a model that does not exist).
}

}  // namespace f05
