#pragma once

// bw/diag/diagnostics.h — F23 §1: the 1 Hz composer. One step() folds the
// IMU assessor, the PPG assessor and the system-health monitor through the
// per-source hysteresis (D7) into SourceQuality + HealthSnapshot, applies
// the D9 fault-reporting policy and accumulates the sticky fault_bits (D8).
//
// Fault-reporting policy (D9, integrator-pinned G1):
//  - modules emit condition-TRANSITION edges; this class rate-limits them:
//    a re-edge of the same code closer than kFaultRepeatMs to its previous
//    report is suppressed, so a flapping condition reports ≤ 1/min and a
//    persistent one only once; fault_bits keeps it visible meanwhile;
//  - fault_bits bit N = static_cast<uint8_t>(FaultCode) == N diagnosed by
//    F23 this boot — sticky, never cleared (D8). M5 (D11, CR-030): the
//    published snapshot is own | registry when F26's provider has a
//    snapshot, own-only otherwise (honest fallback, never fabricated);
//  - degraded_mode = (imu != Good) || (ppg != Good) [ASSUMED], D8 —
//    ORed with the CR-029/CR-030 reliability snapshot when present (D11).
//
// Fail-safe (§10): empty/garbage input yields Insufficient/Insufficient,
// battery_valid=false, degraded_mode=true.
//
// Pure logic: no FreeRTOS/Arduino calls (01 §1 rule 5).

#include <bw/diag/diag_config.h>
#include <bw/diag/diag_types.h>
#include <bw/diag/imu_quality_assessor.h>
#include <bw/diag/ppg_quality_assessor.h>
#include <bw/diag/quality_hysteresis.h>
#include <bw/diag/system_health_monitor.h>
#include <bw/fault.h>
#include <bw/types.h>

namespace bw::diag {

class Diagnostics {
public:
    explicit Diagnostics(const DiagConfig& config = DiagConfig{})
        : config_(config), imu_hyst_(config), ppg_hyst_(config), imu_(config), ppg_(config),
          health_(config) {}

    // One 1 Hz tick: assess, apply hysteresis, fill outputs, collect the
    // rate-limited edges and sticky fault_bits.
    DiagOutput step(const DiagInput& in);

    // Sticky F23 diagnoses of this boot (D8): bit N set == code N ever
    // reported. Never cleared, readable for tests/debugging.
    uint32_t faultBits() const { return fault_bits_; }

    // D9 pipeline for one candidate edge: sticky bit + rate limit + append.
    // Public so host tests can pin the policy directly (and a future
    // F26-side feed can reuse it); touches only this class's state.
    // M5: LowStack(26) now flows through here too (§16.3) — its own D9
    // rate-limit slot (kF23RateLimitSlots), so a flapping stack episode
    // reports ≤ 1/min like every other F23 code.
    void consider(const FaultEdge& edge, TimeMs now_ms, DiagOutput& out);

    void reset();

private:
    DiagConfig config_;
    QualityHysteresis imu_hyst_;
    QualityHysteresis ppg_hyst_;
    ImuQualityAssessor imu_;
    PpgQualityAssessor ppg_;
    SystemHealthMonitor health_;

    // D9 rate-limit table, one slot per F23-owned reportable code: the
    // 11..18 run plus LowStack(26) (kF23RateLimitSlots). When its last
    // report went out and whether it ever did (0 ms is a valid boot time,
    // so "ever" needs its own flag).
    TimeMs last_report_ms_[kF23RateLimitSlots]{};
    bool ever_reported_[kF23RateLimitSlots]{};
    uint32_t fault_bits_ = 0;  // D8 sticky
};

}  // namespace bw::diag
