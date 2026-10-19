#pragma once

// bw/alerts.h — docs/02_INTERFACES.md §4.6 (alert status; F10 owns the engine).
// An alert is a transient OVERLAY, not an operating mode (00 §6).

#include <cstdint>

#include <bw/state.h>
#include <bw/types.h>

namespace bw {

enum class AlertLevel : uint8_t { Normal, Warning, Active, Persistent };

struct AlertStatus {
    TimeMs t_ms;
    AlertLevel level;
    bool acknowledged;
    ReasonMask reasons;
    uint32_t since_ms;  // how long the current alert level has been active
};

}  // namespace bw
