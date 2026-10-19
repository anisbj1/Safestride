#include <bw/containers/spsc_queue.h>

#include <unity.h>

#include <cstdint>

void test_spsc_fifo_order_across_wrap(void) {
    bw::SpscQueue<uint32_t, 4> q;
    TEST_ASSERT_TRUE(q.empty());
    TEST_ASSERT_EQUAL_UINT32(4, q.capacity());

    TEST_ASSERT_EQUAL_INT(bw::Err::Ok, q.push(1));
    TEST_ASSERT_EQUAL_INT(bw::Err::Ok, q.push(2));
    TEST_ASSERT_EQUAL_INT(bw::Err::Ok, q.push(3));
    TEST_ASSERT_EQUAL_UINT32(3, q.size());

    uint32_t v = 0;
    TEST_ASSERT_TRUE(q.pop(v));
    TEST_ASSERT_EQUAL_UINT32(1, v);  // FIFO: first in, first out

    // Fill the freed slots: tail now wraps past the physical end of buf_.
    TEST_ASSERT_EQUAL_INT(bw::Err::Ok, q.push(4));
    TEST_ASSERT_EQUAL_INT(bw::Err::Ok, q.push(5));
    TEST_ASSERT_TRUE(q.full());

    // Drain with wrap on the consumer side too; order must hold throughout.
    const uint32_t expected[] = {2, 3, 4, 5};
    for (uint32_t e : expected) {
        TEST_ASSERT_TRUE(q.pop(v));
        TEST_ASSERT_EQUAL_UINT32(e, v);
    }
    TEST_ASSERT_TRUE(q.empty());

    // A second lap after the wrap still behaves.
    TEST_ASSERT_EQUAL_INT(bw::Err::Ok, q.push(6));
    TEST_ASSERT_EQUAL_INT(bw::Err::Ok, q.push(7));
    TEST_ASSERT_TRUE(q.pop(v));
    TEST_ASSERT_EQUAL_UINT32(6, v);
    TEST_ASSERT_TRUE(q.pop(v));
    TEST_ASSERT_EQUAL_UINT32(7, v);
    TEST_ASSERT_TRUE(q.empty());
    TEST_ASSERT_EQUAL_UINT32(0, q.drops());  // nothing was ever refused
}

void test_spsc_push_full_refused_and_drop_counted(void) {
    bw::SpscQueue<uint32_t, 2> q;
    TEST_ASSERT_EQUAL_INT(bw::Err::Ok, q.push(10));
    TEST_ASSERT_EQUAL_INT(bw::Err::Ok, q.push(20));

    // CR-004 Full: refused, nothing written — stored items untouched, drop counted.
    TEST_ASSERT_EQUAL_INT(bw::Err::Full, q.push(30));
    TEST_ASSERT_EQUAL_UINT32(1, q.drops());
    TEST_ASSERT_EQUAL_UINT32(2, q.size());
    TEST_ASSERT_TRUE(q.full());

    uint32_t v = 0;
    TEST_ASSERT_TRUE(q.pop(v));
    TEST_ASSERT_EQUAL_UINT32(10, v);
    TEST_ASSERT_TRUE(q.pop(v));
    TEST_ASSERT_EQUAL_UINT32(20, v);  // the refused push corrupted nothing

    // After draining, the same value pushes fine: Full was transient, not sticky.
    TEST_ASSERT_EQUAL_INT(bw::Err::Ok, q.push(30));
    TEST_ASSERT_TRUE(q.pop(v));
    TEST_ASSERT_EQUAL_UINT32(30, v);
    TEST_ASSERT_EQUAL_UINT32(1, q.drops());
}

void test_spsc_pop_empty_returns_false(void) {
    bw::SpscQueue<uint32_t, 2> q;
    uint32_t v = 12345;
    TEST_ASSERT_FALSE(q.pop(v));
    TEST_ASSERT_EQUAL_UINT32(12345, v);  // out untouched on empty

    TEST_ASSERT_EQUAL_INT(bw::Err::Ok, q.push(7));
    TEST_ASSERT_TRUE(q.pop(v));
    TEST_ASSERT_EQUAL_UINT32(7, v);
    TEST_ASSERT_FALSE(q.pop(v));  // second pop: empty again
    TEST_ASSERT_EQUAL_UINT32(7, v);
    TEST_ASSERT_EQUAL_UINT32(0, q.drops());  // empty pops are not drops
}

void test_spsc_interleaved_script_loses_nothing_silently(void) {
    // 01 §3.1: "order matters, nothing may be lost silently." Script an interleaved
    // producer/consumer run (deterministic, no threads) that hits both the empty and
    // the full boundary, and check the conservation law:
    //   accepted pushes == popped values + still queued, and every refusal is counted.
    bw::SpscQueue<uint32_t, 3> q;
    uint32_t next_to_push = 0;
    uint32_t next_to_pop = 0;
    uint32_t accepted = 0;
    uint32_t refused = 0;

    const auto produce = [&](uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            const bw::Err err = q.push(next_to_push);
            if (err == bw::Err::Ok) {
                ++next_to_push;
                ++accepted;
            } else {
                TEST_ASSERT_EQUAL_INT(bw::Err::Full, err);
                ++refused;
            }
        }
    };
    const auto consume = [&](uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t v = 0;
            if (!q.pop(v)) {
                break;  // consumer runs dry — legitimate, not a loss
            }
            TEST_ASSERT_EQUAL_UINT32(next_to_pop, v);  // exact order, no gaps
            ++next_to_pop;
        }
    };

    produce(5);  // 3 accepted, 2 refused (full)
    consume(2);
    produce(4);  // space for 2, then full again
    consume(5);
    produce(3);
    consume(10);  // drain everything

    TEST_ASSERT_TRUE(q.empty());
    TEST_ASSERT_EQUAL_UINT32(accepted, next_to_pop + q.size());
    TEST_ASSERT_EQUAL_UINT32(refused, q.drops());
    TEST_ASSERT_TRUE(q.drops() > 0);  // script must actually have exercised overflow
    TEST_ASSERT_EQUAL_UINT32(0, q.size());
}

void test_spsc_capacity_one_edge(void) {
    bw::SpscQueue<uint32_t, 1> q;
    TEST_ASSERT_EQUAL_INT(bw::Err::Ok, q.push(1));
    TEST_ASSERT_TRUE(q.full());
    TEST_ASSERT_EQUAL_INT(bw::Err::Full, q.push(2));
    uint32_t v = 0;
    TEST_ASSERT_TRUE(q.pop(v));
    TEST_ASSERT_EQUAL_UINT32(1, v);
    TEST_ASSERT_FALSE(q.pop(v));
    TEST_ASSERT_EQUAL_UINT32(1, q.drops());
}

void test_spsc_is_trivially_copyable(void) {
    static_assert(std::is_trivially_copyable_v<bw::SpscQueue<uint32_t, 4>>,
                  "SpscQueue must stay trivially copyable (02 §1)");
    TEST_PASS_MESSAGE("SpscQueue<uint32_t,4> is trivially copyable");
}
