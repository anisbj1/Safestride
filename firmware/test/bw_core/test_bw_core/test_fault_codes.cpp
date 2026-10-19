#include <bw/fault.h>

#include <unity.h>

#include <cstdint>
#include <cstdio>
#include <type_traits>

// Append-only guard for the F00-defined enums (02 §7 asks reviewers to check these
// against F01/F23/F26's specs — see F00's spec §16 for the derivation of every
// value). Renumbering or removing any row must fail this test.

namespace {

struct FaultRow {
    unsigned actual;
    unsigned expected;
    const char* name;
};

}  // namespace

void test_fault_code_values_pinned(void) {
    const FaultRow rows[] = {
        {static_cast<unsigned>(bw::FaultCode::None), 0, "None"},
        // bus / HAL (F01, F11)
        {static_cast<unsigned>(bw::FaultCode::I2cTimeout), 1, "I2cTimeout"},
        {static_cast<unsigned>(bw::FaultCode::I2cBusStuck), 2, "I2cBusStuck"},
        {static_cast<unsigned>(bw::FaultCode::ImuAbsent), 3, "ImuAbsent"},
        {static_cast<unsigned>(bw::FaultCode::ImuReadFailed), 4, "ImuReadFailed"},
        {static_cast<unsigned>(bw::FaultCode::PpgAbsent), 5, "PpgAbsent"},
        {static_cast<unsigned>(bw::FaultCode::PpgReadFailed), 6, "PpgReadFailed"},
        {static_cast<unsigned>(bw::FaultCode::PpgFifoOverrun), 7, "PpgFifoOverrun"},
        {static_cast<unsigned>(bw::FaultCode::DisplayFailed), 8, "DisplayFailed"},
        {static_cast<unsigned>(bw::FaultCode::BatterySenseUnavailable), 9,
         "BatterySenseUnavailable"},
        {static_cast<unsigned>(bw::FaultCode::ButtonStuck), 10, "ButtonStuck"},
        // data quality (F23)
        {static_cast<unsigned>(bw::FaultCode::ImuStuckSensor), 11, "ImuStuckSensor"},
        {static_cast<unsigned>(bw::FaultCode::PpgFlatline), 12, "PpgFlatline"},
        {static_cast<unsigned>(bw::FaultCode::PpgSaturated), 13, "PpgSaturated"},
        {static_cast<unsigned>(bw::FaultCode::PpgPoorContact), 14, "PpgPoorContact"},
        {static_cast<unsigned>(bw::FaultCode::TaskOverrun), 15, "TaskOverrun"},
        {static_cast<unsigned>(bw::FaultCode::LowHeap), 16, "LowHeap"},
        {static_cast<unsigned>(bw::FaultCode::LowBattery), 17, "LowBattery"},
        {static_cast<unsigned>(bw::FaultCode::CriticalBattery), 18, "CriticalBattery"},
        // reliability (F26)
        {static_cast<unsigned>(bw::FaultCode::TaskStalled), 19, "TaskStalled"},
        {static_cast<unsigned>(bw::FaultCode::WatchdogReset), 20, "WatchdogReset"},
        {static_cast<unsigned>(bw::FaultCode::Brownout), 21, "Brownout"},
        {static_cast<unsigned>(bw::FaultCode::BootLoop), 22, "BootLoop"},
        {static_cast<unsigned>(bw::FaultCode::StorageDegraded), 23, "StorageDegraded"},
        {static_cast<unsigned>(bw::FaultCode::BleDown), 24, "BleDown"},
        // personalization (F13, CR-031)
        {static_cast<unsigned>(bw::FaultCode::BaselineCorrupt), 25, "BaselineCorrupt"},
        // system overload (F23 M5, CR-032)
        {static_cast<unsigned>(bw::FaultCode::LowStack), 26, "LowStack"},
        // composition wiring (F26 backlogs 16/18, CR-033)
        {static_cast<unsigned>(bw::FaultCode::TaskCreateFailed), 27, "TaskCreateFailed"},
        {static_cast<unsigned>(bw::FaultCode::EventPublishDrop), 28, "EventPublishDrop"},
    };

    const size_t count = sizeof(rows) / sizeof(rows[0]);
    TEST_ASSERT_EQUAL_UINT32(29, count);  // 02 §7/F00 §16: derived set (CR-031..033)
    for (const FaultRow& row : rows) {
        char msg[80];
        snprintf(msg, sizeof(msg), "FaultCode::%s must keep value %u (append-only)", row.name,
                 row.expected);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(row.expected, row.actual, msg);
    }
}

void test_fault_severity_values_pinned(void) {
    // Four levels, one per distinct response of F26's policy (observe -> degrade
    // quality -> reduced pipeline -> safe mode/watchdog). F00 assigns no severity per
    // code; that mapping is F26's (F00 §16).
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<unsigned>(bw::FaultSeverity::Info));
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<unsigned>(bw::FaultSeverity::Warning));
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<unsigned>(bw::FaultSeverity::Error));
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<unsigned>(bw::FaultSeverity::Critical));
}

void test_fault_codes_abi(void) {
    static_assert(std::is_same_v<std::underlying_type_t<bw::FaultCode>, uint8_t>,
                  "FaultCode must stay uint8_t (02 §7)");
    static_assert(std::is_same_v<std::underlying_type_t<bw::FaultSeverity>, uint8_t>,
                  "FaultSeverity must stay uint8_t (02 §7)");
    static_assert(sizeof(bw::FaultCode) == 1, "FaultCode must stay one byte");
    static_assert(sizeof(bw::FaultSeverity) == 1, "FaultSeverity must stay one byte");
    TEST_PASS_MESSAGE("FaultCode/FaultSeverity ABI is uint8_t");
}
