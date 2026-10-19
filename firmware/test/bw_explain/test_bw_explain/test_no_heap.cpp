// bw_explain — F22 §11 (no heap) + §9 item 14: the explain path
// (explainShort + reasonText) must allocate 0 times in steady state. Same
// suite-local allocation-hook pattern as bw_ui/bw_ml test_no_heap.cpp
// (bw_testkit has no shared hook); only THIS file defines operator new, one
// strong definition per suite program.

#include <cstddef>
#include <cstdlib>
#include <new>

#include <unity.h>

#include <bw/explain/explain_short.h>
#include <bw/explain/reason_catalog.h>

namespace {

std::size_t g_allocation_count = 0;

// [DERIVED] Why 512: mask = i * 0x9E3779B9u (golden-ratio spread) walks every
// bit position of the 32-bit mask across the loop — including the unknown
// (>= 18) path — while staying deterministic (no wall-clock, no rand).
constexpr unsigned kExplainIterations = 512;

}  // namespace

void* operator new(std::size_t size) {
    if (size == 0) {
        size = 1;
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

void test_explain_path_allocates_no_heap(void) {
    char buf[bw::explain::kExplainShortBufSize];

    // Warm-up: any one-time initialization happens BEFORE the snapshot.
    (void)bw::explain::explainShort(0x12345678u, buf, sizeof buf);
    (void)bw::explain::explainShort(0u, buf, sizeof buf);
    (void)bw::explain::explainShort(0xFFFFFFFFu, buf, sizeof buf);
    (void)bw::explain::reasonText(bw::ReasonCode::PossibleFall);

    const std::size_t before = g_allocation_count;
    for (unsigned i = 1; i <= kExplainIterations; ++i) {
        const bw::ReasonMask mask = static_cast<bw::ReasonMask>(i * 0x9E3779B9u) ^ (i & 1u);
        (void)bw::explain::explainShort(mask, buf, sizeof buf);
        (void)bw::explain::reasonText(static_cast<bw::ReasonCode>(i % 32));
    }
    const std::size_t after = g_allocation_count;

    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(before), static_cast<unsigned>(after));
}
