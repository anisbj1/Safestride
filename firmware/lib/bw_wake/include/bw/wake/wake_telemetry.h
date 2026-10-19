#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

#include <bw/result.h>
#include <bw/wake/wake_types.h>

namespace bw::wake {

inline const char* inputRejectionName(WakeInputRejection rejection) {
    switch (rejection) {
    case WakeInputRejection::None:
        return "none";
    case WakeInputRejection::WearUnsafe:
        return "wear_unsafe";
    case WakeInputRejection::WearFuture:
        return "wear_future";
    case WakeInputRejection::WearStale:
        return "wear_stale";
    case WakeInputRejection::SampleNotIncreasing:
        return "sample_not_increasing";
    case WakeInputRejection::MotionFuture:
        return "motion_future";
    case WakeInputRejection::MotionStale:
        return "motion_stale";
    case WakeInputRejection::ImuInsufficient:
        return "imu_insufficient";
    case WakeInputRejection::MotionInsufficient:
        return "motion_insufficient";
    case WakeInputRejection::EnmoInvalid:
        return "enmo_invalid";
    case WakeInputRejection::CadenceGap:
        return "cadence_gap";
    }
    return "unknown";
}

// [DERIVED] One g contains 10^6 micro-g; integer logging preserves the gentle
// 0.015 g threshold without invoking newlib's stack-heavy float formatter.
constexpr uint32_t kTelemetryMicroGPerG = 1000000;
// [DERIVED] Adding half a micro-g rounds to the nearest logging unit only;
// qualification always uses the original float, never this display value.
constexpr double kTelemetryRoundingMicroG = 0.5;

inline Result<uint32_t> telemetryMicroG(float value_g) {
    const double scaled =
        static_cast<double>(value_g) * kTelemetryMicroGPerG + kTelemetryRoundingMicroG;
    if (!std::isfinite(value_g) || value_g < 0.0f ||
        scaled > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
        return {Err::InvalidArg, 0};
    }
    return {Err::Ok, static_cast<uint32_t>(scaled)};
}

}  // namespace bw::wake
