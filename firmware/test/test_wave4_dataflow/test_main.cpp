// Test runner for the test_wave4_dataflow suite (W4 P4, application-level
// cross-feature dataflow — M4 exit evidence, docs/05 step 5). Conventions
// match test_wave3_dataflow: every .cpp here links into one program,
// PlatformIO does NOT generate a Unity main, and one strong setUp/tearDown
// pair lives here (weak defaults do not link on this MinGW/COFF toolchain —
// bw_core/test_main.cpp).

#include <unity.h>

void test_history_real_store_reboot_persistence(void);
void test_sync_real_store_partial_and_resume(void);
void test_history_and_sync_one_store(void);

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_history_real_store_reboot_persistence);
    RUN_TEST(test_sync_real_store_partial_and_resume);
    RUN_TEST(test_history_and_sync_one_store);

    return UNITY_END();
}

void setUp(void) {}
void tearDown(void) {}
