#include "../../src/app/wiring/task_health.h"

#include <fstream>
#include <iterator>
#include <string>

#include <unity.h>

namespace {
std::string source(const char* file) {
    std::ifstream stream(file);
    TEST_ASSERT_TRUE_MESSAGE(stream.is_open(), file);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
}  // namespace

void test_startup_barrier_is_closed_until_composition_publishes() {
    app::tasks::StartupBarrier barrier;
    TEST_ASSERT_FALSE(barrier.ready());
    barrier.release();
    TEST_ASSERT_TRUE(barrier.ready());
    barrier.release();  // one-way, idempotent publication
    TEST_ASSERT_TRUE(barrier.ready());
}

void test_every_device_task_waits_before_touching_the_graph() {
    // Structural regression guard, NOT a scheduler/race-detector test. The
    // device build compiles these exact bodies; removing any wait must fail.
    const struct {
        const char* file;
        const char* function;
    } tasks[] = {
        {"wire_activity.cpp", "pumpTask"},
        {"wire_alert.cpp", "alertTask"},
        {"wire_baseline.cpp", "pumpTask"},
        {"wire_ble.cpp", "commsTask"},
        {"wire_cataplexy.cpp", "pumpTask"},
        {"wire_diag.cpp", "diagTask"},
        {"wire_eventlog.cpp", "eventLogTask"},
        {"wire_f02.cpp", "processingTask"},
        {"wire_hal.cpp", "sensorTask"},
        {"wire_input.cpp", "inputTask"},
        {"wire_reliability.cpp", "supervisorTask"},
        {"wire_research.cpp", "researchTask"},
        {"wire_sleep.cpp", "sleepTask"},
        {"wire_state.cpp", "stateTask"},
        {"wire_storage.cpp", "storageTask"},
        {"wire_ui.cpp", "uiTask"},
        {"wire_wake.cpp", "pumpTask"},
        {"wire_wear.cpp", "pumpTask"},
    };
    for (const auto& task : tasks) {
        const std::string path = std::string("src/app/wiring/") + task.file;
        const auto text = source(path.c_str());
        if (text.find("xTaskCreatePinnedToCore(") != std::string::npos) {
            const bool supervisor = std::string(task.file) == "wire_reliability.cpp";
            const bool diag = std::string(task.file) == "wire_diag.cpp";
            const char* check = supervisor ? "reportTaskCreateFailure(ctx.clock"
                                : diag     ? "reportTaskCreateFailure(ctx, ctx.clock"
                                           : "app::tasks::checkCreated(";
            TEST_ASSERT_TRUE_MESSAGE(text.find(check) != std::string::npos, task.file);
        }
        if (std::string(task.file) == "wire_alert.cpp" ||
            std::string(task.file) == "wire_cataplexy.cpp") {
            TEST_ASSERT_TRUE_MESSAGE(text.find("app::tasks::reportEventDrop(") != std::string::npos,
                                     task.file);
        }
        const std::string start = std::string("void ") + task.function + "(void* arg) {";
        const auto pos = text.find(start);
        TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos, task.file);
        const auto first = text.find_first_not_of("\r\n\t ", pos + start.size());
        TEST_ASSERT_TRUE_MESSAGE(text.compare(first,
                                              std::string("app::tasks::waitForStartup();").size(),
                                              "app::tasks::waitForStartup();") == 0,
                                 task.file);
    }
    const auto app = source("src/app/app.cpp");
    TEST_ASSERT_TRUE(
        app.find("void servicePumpTask(void* arg) {\n    tasks::waitForStartup();") !=
            std::string::npos ||
        app.find("void servicePumpTask(void* arg) {\r\n    tasks::waitForStartup();") !=
            std::string::npos);
    const auto release = app.find("tasks::startup.release();");
    TEST_ASSERT_TRUE(release != std::string::npos);
    TEST_ASSERT_TRUE(app.rfind("::attach", release) < release);
    TEST_ASSERT_TRUE(app.find("::attach", release) == std::string::npos);
    TEST_ASSERT_TRUE(app.find("xTaskCreatePinnedToCore", release) == std::string::npos);
}
