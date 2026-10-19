// DoD §14 + spec §15.3: the event type/priority table itself. This is the
// answer to the §11/§13 open question ("which event types are critical and
// never dropped") — one explicit row per current EventType, plus the registry
// pins and the documented forward-compatible fallback for future types.

#include <unity.h>

#include <bw/eventlog/event_logger.h>
#include <bw/eventlog/event_priority.h>

using bw::eventlog::EventPriority;
using bw::eventlog::kEventTypeCount;
using bw::eventlog::kEventTypeMinRepeatMs;
using bw::eventlog::kEventTypePriority;
using bw::eventlog::minRepeatIntervalMs;
using bw::eventlog::priorityFor;

namespace {

struct Row {
    bw::EventType type;
    EventPriority priority;
    bw::TimeMs interval_ms;
};

// The §15.3 table, transcribed row by row (enum order).
const Row kSpecTable[] = {
    {bw::EventType::SleepEpisode, EventPriority::High, 0},
    {bw::EventType::DrowsinessEvent, EventPriority::High, 0},
    {bw::EventType::SuspectedCataplexy, EventPriority::Critical, 0},
    {bw::EventType::UserMarked, EventPriority::High, 0},
    {bw::EventType::AlertRaised, EventPriority::Critical, 0},
    {bw::EventType::SensorFault, EventPriority::Critical, 1000},
    {bw::EventType::DeviceNotWorn, EventPriority::Normal, 5000},
    {bw::EventType::BatteryWarning, EventPriority::Critical, 60000},
    {bw::EventType::NapPlanned, EventPriority::Normal, 0},
    {bw::EventType::NapCompleted, EventPriority::Normal, 0},
    {bw::EventType::TimeSync, EventPriority::High, 0},
    {bw::EventType::Boot, EventPriority::Critical, 0},
};

const bw::EventType kCriticalSet[] = {
    bw::EventType::Boot,           bw::EventType::SensorFault,        bw::EventType::AlertRaised,
    bw::EventType::BatteryWarning, bw::EventType::SuspectedCataplexy,
};

bool isInCriticalSet(bw::EventType type) {
    for (const bw::EventType critical : kCriticalSet) {
        if (critical == type) {
            return true;
        }
    }
    return false;
}

}  // namespace

void test_priority_all_twelve_rows_match_spec(void) {
    const size_t rows = sizeof(kSpecTable) / sizeof(kSpecTable[0]);
    TEST_ASSERT_EQUAL_UINT32(kEventTypeCount, rows);
    for (size_t i = 0; i < rows; ++i) {
        const Row& row = kSpecTable[i];
        TEST_ASSERT_EQUAL_MESSAGE(static_cast<int>(row.priority),
                                  static_cast<int>(priorityFor(row.type)),
                                  "priority drifted from spec §15.3");
        TEST_ASSERT_EQUAL_UINT32(row.interval_ms, minRepeatIntervalMs(row.type));
        TEST_ASSERT_EQUAL_UINT32(row.interval_ms,
                                 kEventTypeMinRepeatMs[static_cast<size_t>(row.type)]);
        TEST_ASSERT_EQUAL(static_cast<int>(row.priority),
                          static_cast<int>(kEventTypePriority[static_cast<size_t>(row.type)]));
    }
}

void test_priority_critical_set_is_exactly_the_five(void) {
    size_t critical_count = 0;
    for (const Row& row : kSpecTable) {
        const bool is_critical = priorityFor(row.type) == EventPriority::Critical;
        if (is_critical) {
            ++critical_count;
        }
        // Membership is exact: Critical ⟺ one of the five §15.3 rows.
        TEST_ASSERT_EQUAL_MESSAGE(isInCriticalSet(row.type) ? 1 : 0, is_critical ? 1 : 0,
                                  "critical set drifted from spec §15.3");
    }
    TEST_ASSERT_EQUAL_UINT32(5, critical_count);
}

void test_priority_unknown_future_type_defaults_normal(void) {
    // Append-only enums grow: an EventType F17 has never seen must land on the
    // documented forward-compatible default (evictable but never evicting,
    // never de-duped) instead of reading past the tables.
    const bw::EventType future_type = static_cast<bw::EventType>(200);
    TEST_ASSERT_EQUAL(static_cast<int>(EventPriority::Normal),
                      static_cast<int>(priorityFor(future_type)));
    TEST_ASSERT_EQUAL_UINT32(0, minRepeatIntervalMs(future_type));
}

void test_priority_registry_and_constant_pins(void) {
    // Registry values (integrator-allocated, W1 G0) and §15.4 constants: a
    // drift here is a High finding, so pin it in every suite that depends on it.
    TEST_ASSERT_EQUAL_UINT32(17, bw::eventlog::kEventlogDetectorId);
    TEST_ASSERT_EQUAL_UINT32(16, bw::eventlog::kEventQueueDepth);
    TEST_ASSERT_EQUAL_UINT32(500, bw::eventlog::kStoreRetryBackoffMs);
    TEST_ASSERT_EQUAL_UINT32(5, bw::eventlog::kWallResyncToleranceS);
    TEST_ASSERT_EQUAL_UINT32(8, bw::eventlog::kMaxEventSubscribers);
    TEST_ASSERT_EQUAL_UINT32(12, kEventTypeCount);
    // §15.4 de-dup intervals, by name.
    TEST_ASSERT_EQUAL_UINT32(1000, bw::eventlog::kMinRepeatIntervalSensorFaultMs);
    TEST_ASSERT_EQUAL_UINT32(5000, bw::eventlog::kMinRepeatIntervalDeviceNotWornMs);
    TEST_ASSERT_EQUAL_UINT32(60000, bw::eventlog::kMinRepeatIntervalBatteryWarningMs);
}
