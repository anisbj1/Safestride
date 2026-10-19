#include <bw/reliability/boot_guard.h>

namespace bw::reliability {

void BootGuard::onBoot(const uint8_t streak_base, const ResetReason reason) {
    // this boot counts (D10); saturate so the u8 never wraps into "safe"
    const uint16_t next = static_cast<uint16_t>(streak_base) + 1u;
    streak_ = next > kMaxRebootsInWindow ? static_cast<uint8_t>(kMaxRebootsInWindow)
                                         : static_cast<uint8_t>(next);
    safe_mode_ = streak_ >= kMaxRebootsInWindow;
    reason_ = reason;
}

bool BootGuard::tickSurvived(const TimeMs now_ms, const TimeMs boot_ms) {
    if (streak_ == 0 || now_ms < boot_ms) {
        return false;  // nothing to clear, or a backwards clock jump (never clear on it)
    }
    if (now_ms - boot_ms < kBootLoopBootLimitMs) {
        return false;  // still inside the danger window
    }
    streak_ = 0;  // survived: streak cleared; safe_mode_ stays as decided (D10
                  // exit = next boot, no live change)
    return true;
}

FaultCode BootGuard::resetFaultCode(const ResetReason reason) {
    switch (reason) {
    case ResetReason::IntWatchdog:
    case ResetReason::TaskWatchdog:
        return FaultCode::WatchdogReset;
    case ResetReason::Brownout:
        return FaultCode::Brownout;
    default:
        return FaultCode::None;  // PowerOn/Software/DeepSleep/Unknown/Panic
    }
}

}  // namespace bw::reliability
