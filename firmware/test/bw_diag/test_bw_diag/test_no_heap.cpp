// bw_diag — no-heap test (F23 §15.5 item 6): global operator new/delete
// counters must stay at zero across 1000 Diagnostics::step() calls with
// varied inputs (bw_ml/test_no_heap.cpp pattern; bw_testkit has no
// allocation hook, so this suite owns the counters itself).
// M5 (§16.4 item 6) adds the registry RMW + raw drain loops below.

#include <cstddef>
#include <cstdlib>
#include <new>

#include <bw/diag/diag_config.h>
#include <bw/diag/diagnostics.h>
#include <bw/diag/task_registry.h>

#include <unity.h>

#include "../../../src/app/app.h"
#include "../../../src/app/wiring/wire_diag.h"
#include "diag_test_support.h"

namespace {

// Counted allocations. Static-storage duration with constant
// initialization: zero before any dynamic initializer runs.
std::size_t g_allocation_count = 0;

}  // namespace

void* operator new(std::size_t size) {
    if (size == 0) {
        size = 1;  // malloc(0) may return null, which must not mean "failure"
    }
    ++g_allocation_count;
    if (void* memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {

// [EXPERIMENTAL] Call count per §15.5 item 6's "0 allocations across
// step() loops" (1000 ticks = ~17 min of device runtime at 1 Hz).
constexpr int kStepCalls = 1000;

}  // namespace

void test_no_heap_no_allocation_over_1000_steps(void) {
    bw::diag::Diagnostics d;
    bw::PpgSample ppg_window[50];
    bw::ImuSample imu_window[32];
    bw_diag_test::healthyPpgWindow(ppg_window, 50);
    bw_diag_test::healthyImuWindow(imu_window, 32);

    bw::diag::DiagInput in = bw_diag_test::healthyInput(0);
    (void)d.step(in);  // warm-up before the snapshot (static guards)

    const std::size_t before = g_allocation_count;
    for (int i = 1; i <= kStepCalls; ++i) {
        const bw::TimeMs t = static_cast<bw::TimeMs>(i) * 1000;
        in = bw_diag_test::healthyInput(t);
        // Exercise every path: raw windows, invalid battery, bad stats.
        if (i % 3 == 0) {
            in.ppg_raw = ppg_window;
            in.ppg_raw_n = 50;
            in.imu_raw = imu_window;
            in.imu_raw_n = 32;
        }
        if (i % 5 == 0) {
            in.battery = bw_diag_test::unknownBattery();
        }
        if (i % 7 == 0) {
            in.stats = bw::diag::SystemStats{};  // invalid
        }
        (void)d.step(in);
    }
    const std::size_t after = g_allocation_count;

    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(before), static_cast<unsigned>(after));
}

// M5 (§16.4 item 6): registry read-modify-write + raw tee drain loops.
// Drives the REAL wiring seams (f23::noteTaskCycle / onImuSample /
// onPpgSample / gatherInput) and the composer for 1000 ticks: covers the
// added statics (registry mailboxes, SPSC queue, static windows) that the
// M2-only loop above never touched.
void test_no_heap_registry_rmw_and_raw_drain_1000(void) {
    app::AppContext ctx{};
    bw::diag::Diagnostics d;

    bw::PpgSample ppg{};
    bw_diag_test::healthyPpgWindow(&ppg, 1);
    const bw::ImuSample imu = bw_diag_test::healthyImuSample(1000, 0.0f);

    (void)f23::gatherInput(ctx, 0);  // warm-up (static guards) before the snapshot

    const std::size_t before = g_allocation_count;
    for (int i = 1; i <= kStepCalls; ++i) {
        const bw::TimeMs t = static_cast<bw::TimeMs>(i) * 1000;
        // Every slot every tick (RMW + tear-free copy), varied lateness/watermark.
        for (uint8_t s = 0; s < static_cast<uint8_t>(bw::diag::TaskSlot::Count); ++s) {
            f23::noteTaskCycle(static_cast<bw::diag::TaskSlot>(s), t, static_cast<uint32_t>(i % 60),
                               static_cast<uint16_t>(900 - (i % 400)));
        }
        // 8 raw pushes per tick per sensor (producer side; Full refusals just
        // bump the drop counter, which gatherInput's drop-hole rule reads).
        for (int k = 0; k < 8; ++k) {
            f23::onImuSample(imu);
            f23::onPpgSample(ppg);
        }
        const bw::diag::DiagInput in = f23::gatherInput(ctx, t);
        (void)d.step(in);
    }
    const std::size_t after = g_allocation_count;

    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(before), static_cast<unsigned>(after));
}
