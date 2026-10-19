// bw_ml — no-heap test (F05 spec §9): global operator new/delete counters
// must stay at zero across 1000 predict()/contributions() calls. bw_testkit
// has no allocation hook (§9), so this suite owns the counters itself.

#include <cstddef>
#include <cstdlib>
#include <new>

#include <unity.h>

#include "models_under_test.h"

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

// [EXPERIMENTAL] Call count per §9's "0 allocations over 1000 predict() calls".
constexpr int kPredictCalls = 1000;

}  // namespace

void test_no_heap_no_allocation_over_1000_predicts(void) {
    // Construct every model (first-use static init) BEFORE the snapshot.
    bw::IModel* models[4] = {
        bw_ml_test::modelFor("linear_toy"),
        bw_ml_test::modelFor("linear_3class"),
        bw_ml_test::modelFor("tree_toy"),
        bw_ml_test::modelFor("tree_deep"),
    };
    for (bw::IModel* model : models) {
        TEST_ASSERT_NOT_NULL(model);
    }

    bw::FeatureVector fv{};
    fv.schema_id = bw_ml_test::kTestSchemaId;
    fv.schema_version = bw_ml_test::kTestSchemaVersion;
    fv.count = bw_ml_test::kTestFeatureCount;
    fv.v[0] = 1.0f;
    fv.v[1] = 72.0f;
    fv.t_ms = 100;
    bw::ModelOutput out{};
    bw::Contribution contrib[bw::kMaxFeatures];
    for (bw::IModel* model : models) {  // warm-up call per model (static guards)
        (void)model->predict(fv, out);
        (void)model->contributions(fv, contrib, bw::kMaxFeatures);
    }

    const std::size_t before = g_allocation_count;
    for (int i = 0; i < kPredictCalls; ++i) {
        for (bw::IModel* model : models) {
            (void)model->predict(fv, out);
        }
    }
    for (bw::IModel* model : models) {  // contributions must not allocate either
        (void)model->contributions(fv, contrib, bw::kMaxFeatures);
    }
    const std::size_t after = g_allocation_count;

    TEST_ASSERT_EQUAL_UINT(static_cast<unsigned>(before), static_cast<unsigned>(after));
}
