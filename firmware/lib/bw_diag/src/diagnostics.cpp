// bw/diag/diagnostics.cpp — F23 composer. See header.

#include <bw/diag/diagnostics.h>

namespace bw::diag {

void Diagnostics::consider(const FaultEdge& edge, TimeMs now_ms, DiagOutput& out) {
    if (edge.code == FaultCode::None) {
        return;
    }

    // D8 sticky bits: diagnose-once-per-boot visibility, independent of
    // the report rate limit. Range-check ABOVE the shift: fault_bits is a
    // uint32_t mask (kFaultBitsWidth), so an out-of-range code is a
    // defined no-op instead of a shift >= 32 (undefined behavior).
    const uint8_t code = static_cast<uint8_t>(edge.code);
    if (code >= kFaultBitsWidth) {
        return;
    }
    fault_bits_ |= (1u << code);

    // D9 rate-limit slot: F23-owned codes only — the 11..18 run plus
    // LowStack(26, CR-032). Foreign codes (1..10, 19..25, 27..31) record
    // their bit and stop here: no edge, no rate-limit entry (I2).
    int slot = -1;
    if (code >= kF23FaultCodeFirst && code <= kF23FaultCodeLast) {
        slot = static_cast<int>(code - kF23FaultCodeFirst);
    } else if (code == kLowStackFaultCode) {
        slot = static_cast<int>(kF23FaultCodeCount);
    }
    if (slot < 0) {
        return;
    }

    // D9 rate limit: first report always passes; a flapping re-edge
    // within kFaultRepeatMs of the previous one is suppressed.
    if (ever_reported_[slot] &&
        now_ms < last_report_ms_[slot] + static_cast<TimeMs>(config_.fault_repeat_ms)) {
        return;
    }
    if (out.edge_count >= kMaxFaultEdgesPerTick) {
        return;  // unreachable with 7 max conditions; guarded, never overflowed
    }
    out.edges[out.edge_count++] = edge;
    last_report_ms_[slot] = now_ms;
    ever_reported_[slot] = true;
}

DiagOutput Diagnostics::step(const DiagInput& in) {
    DiagOutput out{};

    // --- per-source instantaneous judgments (fail-safe defaults) ------------
    const MotionFeatures* motion = in.has_motion ? &in.motion : nullptr;
    const PpgFeatures* ppg_feats = in.has_ppg ? &in.ppg : nullptr;

    const ImuAssessOutput imu_out =
        imu_.step(in.now_ms, in.has_imu_status, in.imu_status, motion, in.imu_raw, in.imu_raw_n);
    const PpgAssessOutput ppg_out =
        ppg_.step(in.now_ms, in.has_ppg_status, in.ppg_status, ppg_feats, in.ppg_raw, in.ppg_raw_n);

    // --- hysteresis (D7) ------------------------------------------------------
    out.quality.imu = imu_hyst_.update(imu_out.instantaneous);
    out.quality.ppg = ppg_hyst_.update(ppg_out.instantaneous);

    // --- health fields (§15.4 honest-degradation table + M5 D11/D13) --------
    const HealthAssessOutput h = health_.step(in.battery, in.stats, in.tasks, in.task_count);
    out.health.t_ms = in.now_ms;
    out.health.quality = out.quality;
    out.health.battery_percent = h.battery_percent;
    out.health.battery_valid = h.battery_valid;
    out.health.min_free_heap = h.min_free_heap;
    out.health.worst_task_lateness_ms = h.worst_task_lateness_ms;
    out.health.ble_connected = h.ble_connected;  // D12: F19 snapshot as-is (wiring)

    // D11: degraded_mode = D8's quality definition, ORed with F26's
    // CR-029/CR-030 snapshot when one exists (degraded facility mask or
    // safe mode = capability reduced beyond the data-quality view). No
    // provider ⇒ quality definition stands alone (the M2 fallback, never
    // a fabricated degraded/safe state).
    const bool quality_degraded =
        SystemHealthMonitor::degradedMode(out.quality.imu, out.quality.ppg);
    const bool registry_degraded =
        in.has_reliability &&
        (in.reliability.degraded != bw::DegradedMode{0} || in.reliability.safe_mode);
    out.health.degraded_mode = quality_degraded || registry_degraded;

    // --- fault policy (D9): sticky bits + rate-limited edges ------------------
    consider(imu_out.edge, in.now_ms, out);
    consider(ppg_out.edge, in.now_ms, out);
    for (uint8_t i = 0; i < h.edge_count; ++i) {
        consider(h.edges[i], in.now_ms, out);
    }
    // D11 merge (CR-030): own sticky bits | the registry's snapshot — a
    // union, because both sides are diagnosed-this-boot evidence with the
    // same D8 bit mapping and the union is idempotent (F23's codes reach
    // the registry through ctx.faults, so overlap ORs harmlessly). No
    // provider (has_reliability=false) ⇒ own-only: honest fallback, never
    // a fabricated zero or a fabricated foreign bit. Foreign bits arrive
    // here ONLY through this line — they never pass consider(), so they
    // produce no edges and enter no rate limiter (I2 through the new path).
    out.health.fault_bits = fault_bits_ | (in.has_reliability ? in.reliability.fault_bits : 0u);
    return out;
}

void Diagnostics::reset() {
    imu_hyst_.reset();
    ppg_hyst_.reset();
    imu_.reset();
    ppg_.reset();
    health_.reset();
    for (uint8_t i = 0; i < kF23RateLimitSlots; ++i) {
        last_report_ms_[i] = 0;
        ever_reported_[i] = false;
    }
    fault_bits_ = 0;
}

}  // namespace bw::diag
