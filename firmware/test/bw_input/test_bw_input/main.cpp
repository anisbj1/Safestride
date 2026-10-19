// Test runner for the bw_input / F11 suite (F11 §9).
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. Each test file exposes one
// register_* entry point that RUN_TESTs everything it defines — a function
// that is never registered silently never runs (same rule as the bw_wear
// suite).

#include <unity.h>

void register_debouncer_tests(void);
void register_gesture_detector_tests(void);
void register_input_router_tests(void);
void register_context_tests(void);
void register_engine_tests(void);
void register_contract_tests(void);
void register_wake_check_tests(void);
void register_alert_consumption_tests();

int main() {
    UNITY_BEGIN();
    register_alert_consumption_tests();

    register_debouncer_tests();         // 8 cases
    register_gesture_detector_tests();  // 16 cases
    register_input_router_tests();      // 22 cases (20 D2-N cells + 2 table tests)
    register_context_tests();           // 8 cases
    register_engine_tests();            // 8 cases
    register_contract_tests();          // 4 cases
    register_wake_check_tests();        // Warning-only check publication

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
