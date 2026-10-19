// F16 §9 T9: no heap in steady-state step() (spec §6 D10, 00 §7.2) — the
// global operator new/delete counters must stay at zero across boot() and
// 1000 ticks. Same pattern as bw_ml/test_no_heap.

#include <cstddef>
#include <cstdlib>
#include <new>

#include <unity.h>

#include "history_test_support.h"

namespace {

// Counted allocations. Static-storage duration with constant initialization:
// zero before any dynamic initializer runs, so allocations during static init
// are counted too and the snapshot below is always valid.
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

using namespace f16_test;
namespace bh = bw::history;

// [EXPERIMENTAL] Ticks per §9's "0 allocations over 1000 step() calls".
constexpr int kStepCalls = 1000;

}  // namespace

void test_no_heap_no_allocation_over_1000_ticks(void) {
    bw::testkit::FakeClock clock;
    bw::testkit::InMemoryEventStore<128> store;
    bw::testkit::InMemoryKeyValueStore<> kv;

    // A backlog across several days: the first ticks drain, finalize and
    // persist (KV writes included), the rest idle-read. All of it must run
    // without a single allocation after the snapshot.
    for (uint32_t day = 40; day <= 44; ++day) {
        (void)appendEvent(store, napCompleted(1, wallUnix(day, 10, 0), 1000 + day));
        (void)appendEvent(store, napCompleted(1, wallUnix(day, 22, 0), 2000 + day));
    }
    (void)appendEvent(store, napCompleted(1, wallUnix(45, 10, 0), 9999));

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &store, &kv, &clock};
    // Warm-up: construction, the boot probe and one tick (function-local
    // statics, first-call guards) run BEFORE the snapshot.
    (void)f16_test::bootToReady(agg);
    (void)agg.step();

    const std::size_t before = g_allocation_count;
    for (int i = 0; i < kStepCalls; ++i) {
        (void)agg.step();
    }
    const std::size_t after = g_allocation_count;

    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(before), static_cast<unsigned>(after));
}
