// Test runner for the test_wave3_dataflow suite (W3 P4, application-level:
// cross-feature F17 -> F18 dataflow, M3 exit evidence). Conventions match the
// feature suites: every .cpp here links into one program, PlatformIO does NOT
// generate a Unity main, and one strong setUp/tearDown pair lives here (weak
// defaults do not link on this MinGW/COFF toolchain — documented in
// bw_core/test_main.cpp).

#include <unity.h>

void test_dataflow_logger_to_store_in_order(void);
void test_dataflow_events_survive_reboot(void);
void test_dataflow_fanout_matches_store(void);

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_dataflow_logger_to_store_in_order);
    RUN_TEST(test_dataflow_events_survive_reboot);
    RUN_TEST(test_dataflow_fanout_matches_store);

    return UNITY_END();
}

void setUp(void) {}
void tearDown(void) {}
