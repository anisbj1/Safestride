#include "wire_explain.h"

namespace app {

size_t WireExplain::explainShort(bw::ReasonMask mask, char* buf, size_t cap) {
    return bw::explain::explainShort(mask, buf, cap);
}

const char* WireExplain::reasonText(bw::ReasonCode code) {
    return bw::explain::reasonText(code);
}

uint16_t WireExplain::catalogVersion() {
    return bw::explain::kReasonCatalogVersion;
}

}  // namespace app
