#pragma once

// bw/quality.h — docs/02_INTERFACES.md §4.1 (data quality).

#include <bw/types.h>

namespace bw {

// Insufficient is a first-class output: "no reliable data" beats a guess (00 §1).
enum class DataQuality : uint8_t { Good, Degraded, Insufficient };

// Per-source quality: a dead PPG must not condemn the IMU stream (00 §6, F23).
struct SourceQuality {
    DataQuality imu;
    DataQuality ppg;
};

}  // namespace bw
