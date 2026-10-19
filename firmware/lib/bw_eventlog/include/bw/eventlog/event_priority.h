#pragma once

// bw/eventlog/event_priority.h — F17 spec §15.3: the per-EventType priority and
// de-dup (min-repeat) tables, plus the EventPriority enum the overflow policy
// (§6 D2) evicts by. This table IS the answer to the spec §13 open question
// ("which event types are critical and never dropped") and is the DoD §14
// artifact: all 12 current EventTypes are mapped explicitly below and pinned by
// static_assert here and table-driven in test_priority_table.cpp.

#include <cstddef>
#include <cstdint>

#include <bw/events.h>
#include <bw/types.h>

namespace bw::eventlog {

// Lower value = more important (§15.3). Critical residents are never evicted.
enum class EventPriority : uint8_t { Critical = 0, High = 1, Normal = 2, Low = 3 };

// Number of current bw::EventType values. bw::EventType is append-only
// (02 §6.6), so a NEW type gets value 12 and lands out of range below: both
// accessors then fall back to the documented conservative default (priority
// Normal = evictable but never evicting, interval 0 = never de-duped) until a
// plan revision extends these tables. [DERIVED] Why 12: EventType's last
// current member is Boot == 11 (bw/events.h:23-37).
inline constexpr size_t kEventTypeCount = 12;

// §15.3 priority column, indexed by EventType. Rows written in enum order.
// [DERIVED] Why each row: see the §15.3 "Why" column — Critical = system
// integrity / user-visible / flagship-detection records that must survive a
// burst; High = primary history; Normal = low-volume or burst-prone-but-
// replaceable context; Low is reserved for future chatty types (none today).
inline constexpr EventPriority kEventTypePriority[kEventTypeCount] = {
    /* SleepEpisode        */ EventPriority::High,
    /* DrowsinessEvent     */ EventPriority::High,
    /* SuspectedCataplexy  */ EventPriority::Critical,
    /* UserMarked          */ EventPriority::High,
    /* AlertRaised         */ EventPriority::Critical,
    /* SensorFault         */ EventPriority::Critical,
    /* DeviceNotWorn       */ EventPriority::Normal,
    /* BatteryWarning      */ EventPriority::Critical,
    /* NapPlanned          */ EventPriority::Normal,
    /* NapCompleted        */ EventPriority::Normal,
    /* TimeSync            */ EventPriority::High,
    /* Boot                */ EventPriority::Critical,
};

// §15.4 named de-dup intervals (the three non-zero rows of the table below).
// Every other EventType is a transition event already hysteresis'd upstream
// (F07/F08/F10), so its interval is 0 = never limited and repeats cannot flood.
inline constexpr TimeMs kMinRepeatIntervalSensorFaultMs = 1000;      // [EXPERIMENTAL]
inline constexpr TimeMs kMinRepeatIntervalDeviceNotWornMs = 5000;    // [EXPERIMENTAL]
inline constexpr TimeMs kMinRepeatIntervalBatteryWarningMs = 60000;  // [EXPERIMENTAL]

// §15.3 "Min repeat (ms)" column: per-type de-dup interval for the
// EventRateLimiter (§15.2 D7); 0 = never limited. Why the three non-zero rows:
// SensorFault - I²C faults repeat at task rate, 1 s keeps the signal while
// bounding queue fill; DeviceNotWorn - don/doff flapping bursts, wear
// transitions need ≥5 s to be meaningful history; BatteryWarning - battery %
// drifts slowly, 1/min bounds the flood while keeping the trend. Tune all
// three with F28/F29 data.
inline constexpr TimeMs kEventTypeMinRepeatMs[kEventTypeCount] = {
    /* SleepEpisode        */ 0,
    /* DrowsinessEvent     */ 0,
    /* SuspectedCataplexy  */ 0,
    /* UserMarked          */ 0,
    /* AlertRaised         */ 0,
    /* SensorFault         */ kMinRepeatIntervalSensorFaultMs,
    /* DeviceNotWorn       */ kMinRepeatIntervalDeviceNotWornMs,
    /* BatteryWarning      */ kMinRepeatIntervalBatteryWarningMs,
    /* NapPlanned          */ 0,
    /* NapCompleted        */ 0,
    /* TimeSync            */ 0,
    /* Boot                */ 0,
};

// Out-of-range (a future appended EventType) → Normal, the documented
// forward-compatible default (never evicts a Critical resident, never
// starved of queue space by Criticals alone).
constexpr EventPriority priorityFor(bw::EventType type) {
    const size_t index = static_cast<size_t>(type);
    return index < kEventTypeCount ? kEventTypePriority[index] : EventPriority::Normal;
}

// Out-of-range → 0 (no de-dup until the table is extended deliberately).
constexpr bw::TimeMs minRepeatIntervalMs(bw::EventType type) {
    const size_t index = static_cast<size_t>(type);
    return index < kEventTypeCount ? kEventTypeMinRepeatMs[index] : 0;
}

// Compile-time pins: the DoD §14 critical set and the de-dup rows. A drifted
// table must fail the BUILD, not just one Unity row (F08 audit L-3 pattern).
static_assert(priorityFor(bw::EventType::Boot) == EventPriority::Critical,
              "F17 §15.3: Boot must stay Critical (timeline anchor)");
static_assert(priorityFor(bw::EventType::SensorFault) == EventPriority::Critical,
              "F17 §15.3: SensorFault must stay Critical (hardware loss)");
static_assert(priorityFor(bw::EventType::AlertRaised) == EventPriority::Critical,
              "F17 §15.3: AlertRaised must stay Critical (alert audit)");
static_assert(priorityFor(bw::EventType::BatteryWarning) == EventPriority::Critical,
              "F17 §15.3: BatteryWarning must stay Critical (power history)");
static_assert(priorityFor(bw::EventType::SuspectedCataplexy) == EventPriority::Critical,
              "F17 §15.3: SuspectedCataplexy must stay Critical (flagship detection)");
static_assert(kEventTypeMinRepeatMs[static_cast<size_t>(bw::EventType::Boot)] == 0 &&
                  kEventTypeMinRepeatMs[static_cast<size_t>(bw::EventType::AlertRaised)] == 0 &&
                  kEventTypeMinRepeatMs[static_cast<size_t>(bw::EventType::SuspectedCataplexy)] ==
                      0,
              "F17 §15.3: transition events are never de-duped");
static_assert(static_cast<size_t>(bw::EventType::Boot) == kEventTypeCount - 1,
              "F17 tables are written in EventType order: a renumbering or a table edit that "
              "desynchronises them must fail the build (appended types fall back to Normal "
              "until §15.3 is extended — documented above)");

}  // namespace bw::eventlog
