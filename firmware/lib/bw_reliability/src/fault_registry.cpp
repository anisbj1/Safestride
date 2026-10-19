#include <bw/reliability/fault_registry.h>

namespace bw::reliability {

bool FaultRegistry::valid(const FaultCode code) {
    return code != FaultCode::None && static_cast<size_t>(code) < kMaxFaultCodes;
}

void FaultRegistry::report(const FaultCode code, const FaultSeverity severity, const TimeMs t_ms) {
    if (!valid(code)) {
        return;  // defined no-op (F23 I2 precedent), never UB
    }
    const size_t index = static_cast<size_t>(code);
    FaultRecord& rec = records_[index];
    if (rec.count == 0) {
        rec.first_ms = t_ms;  // first report this boot
    }
    ++rec.count;
    rec.last_ms = t_ms;
    if (severity > rec.max_severity) {
        rec.max_severity = severity;  // severity ordering = enum order (fault.h)
    }
    fault_bits_ |= (1u << index);  // sticky, never cleared (D4)
}

bool FaultRegistry::has(const FaultCode code) const {
    return valid(code) && records_[static_cast<size_t>(code)].count > 0;
}

bool FaultRegistry::record(const FaultCode code, FaultRecord& out) const {
    if (!has(code)) {
        return false;
    }
    out = records_[static_cast<size_t>(code)];
    return true;
}

uint32_t FaultRegistry::distinctCodes() const {
    uint32_t n = 0;
    for (size_t i = 0; i < kMaxFaultCodes; ++i) {
        n += records_[i].count > 0 ? 1u : 0u;
    }
    return n;
}

}  // namespace bw::reliability
