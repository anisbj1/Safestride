#pragma once

// bw/reliability/fault_registry.h — F26's aggregation point (01 §7 "Faults are
// reported via IFaultReporter (F26 aggregates)"; spec §16.1 D3 choice (b)).
//
// Pure core: no platform calls, no locking. Reports arrive from several tasks,
// so the wiring layer hands features a bw::CriticalSection-guarded SUBCLASS
// (wire_reliability.cpp, D3); host tests drive this class single-threaded.
//
// Storage per code: {count, first_ms, last_ms, max_severity} plus the sticky
// fault_bits word of CR-030 — bit N <=> FaultCode value N, set on the first
// report this boot, never cleared (D4: no FaultCleared code exists, append-only
// registry; recovery stays visible via SensorStatus and events).

#include <cstddef>
#include <cstdint>

#include <bw/fault.h>
#include <bw/reliability/reliability_config.h>
#include <bw/types.h>

namespace bw::reliability {

struct FaultRecord {
    uint32_t count = 0;
    TimeMs first_ms = 0;
    TimeMs last_ms = 0;
    FaultSeverity max_severity = FaultSeverity::Info;
};

class FaultRegistry : public IFaultReporter {
public:
    // Out-of-range codes (>= kMaxFaultCodes) and FaultCode::None are defined
    // no-ops (F23 I2 precedent; None is the "no fault" sentinel, not a symptom
    // — reporting it would set bit 0 and corrupt the bit-N <=> code-N map).
    void report(FaultCode code, FaultSeverity severity, TimeMs t_ms) override;

    // CR-030 word: sticky, never cleared (D4).
    uint32_t faultBits() const { return fault_bits_; }

    bool has(FaultCode code) const;

    // false and `out` untouched when the code is None / out of range / never
    // reported — "no record" is an ordinary state, never a fabricated zero.
    bool record(FaultCode code, FaultRecord& out) const;

    // Distinct codes reported at least once this boot.
    uint32_t distinctCodes() const;

private:
    static bool valid(FaultCode code);

    FaultRecord records_[kMaxFaultCodes]{};
    uint32_t fault_bits_ = 0;
};

}  // namespace bw::reliability
