// bw_reliability — no-heap test (spec §16.8 item 9): global operator
// new/delete counters must stay at zero across 1000 steady-state
// DegradationPolicy::step() / FaultRegistry::report() /
// WatchdogSupervisor::scan() calls — the hot path F26 adds (00 §7: no heap
// after startup in real-time paths). bw_testkit has no allocation hook, so
// this suite owns the counters itself (bw_diag/bw_ml precedent).

#include <cstddef>
#include <cstdlib>
#include <new>

#include <bw/fault.h>
#include <bw/providers.h>
#include <bw/reliability/degradation_policy.h>
#include <bw/reliability/fault_registry.h>
#include <bw/reliability/watchdog_supervisor.h>

#include <unity.h>

#include "reliability_test_support.h"

namespace {

// Counted allocations. Static-storage duration with constant
// initialization: zero before any dynamic initializer runs.
std::size_t g_allocation_count = 0;

// [EXPERIMENTAL] 1000 ticks ≈ 17 min of device runtime at 1 Hz (bw_diag
// §15.5 convention).
constexpr int kStepCalls = 1000;

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

void test_no_heap_no_allocation_over_1000_steps(void) {
    bw::reliability::DegradationPolicy policy;
    bw::reliability::FaultRegistry registry;
    bw::reliability::WatchdogSupervisor supervisor;
    supervisor.configure({bw::reliability::TaskId::Sensor, bw::reliability::TaskTier::A, 1000});
    supervisor.configure({bw::reliability::TaskId::Ui, bw::reliability::TaskTier::B, 100});

    bw::reliability::PolicyInput in = reliability_test::baseInput(0);
    (void)policy.step(in);  // warm-up before the snapshot (static guards)
    registry.report(bw::FaultCode::TaskStalled, bw::FaultSeverity::Error, 0);
    (void)supervisor.scan(0);

    const std::size_t before = g_allocation_count;
    for (int i = 1; i <= kStepCalls; ++i) {
        const bw::TimeMs t = static_cast<bw::TimeMs>(i) * 1000;
        in = reliability_test::baseInput(t);
        if (i % 3 == 0) {
            in.has_storage = true;
            in.storage = reliability_test::corruptStorage(t);
        }
        if (i % 5 == 0) {
            in.has_ble = true;
            in.ble.t_ms = t;
            in.ble.connected = (i % 10 == 0);
        }
        if (i % 7 == 0) {
            in.has_ppg_status = true;
            in.ppg_status = bw::SensorStatus::Failed;
        }
        if (i % 11 == 0) {
            in.tier_b_stalled_mask = 1u << static_cast<uint8_t>(bw::reliability::TaskId::Comms);
        }
        if (i % 13 == 0) {
            in.safe_mode = true;
        }
        (void)policy.step(in);
        registry.report(bw::FaultCode::TaskOverrun, bw::FaultSeverity::Warning, t);
        supervisor.beat(bw::reliability::TaskId::Sensor, t);
        (void)supervisor.scan(t);
    }
    const std::size_t after = g_allocation_count;

    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(before), static_cast<unsigned>(after));
}
