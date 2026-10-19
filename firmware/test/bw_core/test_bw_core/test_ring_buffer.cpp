#include <bw/containers/ring_buffer.h>

#include <unity.h>

#include <cstdint>

namespace {

// 48-byte payload mirroring the contract Event's size (F00 §9: "ordering preserved
// with T = Event-sized payload"; events.h lands in a later step).
struct EventSizedPayload {
    bw::TimeMs t_ms;
    uint32_t seq;
    uint8_t payload[36];
};
static_assert(sizeof(EventSizedPayload) == 48, "mirror of kEventBytes for layout tests");

void makeFull(bw::RingBuffer<uint32_t, 4>& rb) {
    TEST_ASSERT_EQUAL_INT(bw::Err::Ok, rb.push(10));
    TEST_ASSERT_EQUAL_INT(bw::Err::Ok, rb.push(20));
    TEST_ASSERT_EQUAL_INT(bw::Err::Ok, rb.push(30));
    TEST_ASSERT_EQUAL_INT(bw::Err::Ok, rb.push(40));
}

template <size_t N>
void assertValueAt(const bw::RingBuffer<uint32_t, N>& rb, size_t index, uint32_t expected) {
    const uint32_t* v = rb.at(index);
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_UINT32(expected, *v);
}

}  // namespace

void test_ring_buffer_empty_first_read(void) {
    bw::RingBuffer<uint32_t, 4> rb;
    TEST_ASSERT_TRUE(rb.empty());
    TEST_ASSERT_FALSE(rb.full());
    TEST_ASSERT_EQUAL_UINT32(0, rb.size());
    TEST_ASSERT_EQUAL_UINT32(4, rb.capacity());
    TEST_ASSERT_NULL(rb.at(0));  // no magic value: out-of-range reads return nullptr
}

void test_ring_buffer_push_to_capacity(void) {
    bw::RingBuffer<uint32_t, 4> rb;
    makeFull(rb);
    TEST_ASSERT_TRUE(rb.full());
    TEST_ASSERT_EQUAL_UINT32(4, rb.size());
    TEST_ASSERT_EQUAL_UINT32(0, rb.overwrites());
    assertValueAt(rb, 0, 10);
    assertValueAt(rb, 3, 40);
}

void test_ring_buffer_wrap_overwrites_oldest(void) {
    bw::RingBuffer<uint32_t, 4> rb;
    makeFull(rb);
    // 5th push overwrites the oldest (10) and signals data loss per CR-004.
    TEST_ASSERT_EQUAL_INT(bw::Err::Overflow, rb.push(50));
    TEST_ASSERT_EQUAL_UINT32(4, rb.size());
    TEST_ASSERT_EQUAL_UINT32(1, rb.overwrites());
    assertValueAt(rb, 0, 20);  // index 0 is still the oldest survivor
    assertValueAt(rb, 3, 50);

    TEST_ASSERT_EQUAL_INT(bw::Err::Overflow, rb.push(60));
    TEST_ASSERT_EQUAL_UINT32(2, rb.overwrites());
    assertValueAt(rb, 0, 30);
    assertValueAt(rb, 3, 60);
}

void test_ring_buffer_indexed_order_across_wrap(void) {
    bw::RingBuffer<uint32_t, 4> rb;
    makeFull(rb);
    rb.push(50);
    rb.push(60);
    rb.push(70);  // three wraps worth: survivors must stay FIFO-ordered
    TEST_ASSERT_EQUAL_UINT32(4, rb.size());
    assertValueAt(rb, 0, 40);
    assertValueAt(rb, 1, 50);
    assertValueAt(rb, 2, 60);
    assertValueAt(rb, 3, 70);
    TEST_ASSERT_NULL(rb.at(4));  // one past the end stays a nullptr, not a fault
}

void test_ring_buffer_single_slot(void) {
    bw::RingBuffer<uint32_t, 1> rb;
    TEST_ASSERT_EQUAL_INT(bw::Err::Ok, rb.push(1));
    TEST_ASSERT_TRUE(rb.full());
    TEST_ASSERT_EQUAL_INT(bw::Err::Overflow, rb.push(2));
    TEST_ASSERT_EQUAL_UINT32(1, rb.size());
    TEST_ASSERT_EQUAL_UINT32(1, rb.overwrites());  // exactly one overwrite counted
    assertValueAt(rb, 0, 2);
}

void test_ring_buffer_clear(void) {
    bw::RingBuffer<uint32_t, 4> rb;
    makeFull(rb);
    rb.push(50);  // one overwrite on the books
    rb.clear();
    TEST_ASSERT_TRUE(rb.empty());
    TEST_ASSERT_EQUAL_UINT32(0, rb.size());
    TEST_ASSERT_NULL(rb.at(0));
    TEST_ASSERT_EQUAL_UINT32(1, rb.overwrites());  // statistics survive clear()
    // and the buffer is fully usable again
    TEST_ASSERT_EQUAL_INT(bw::Err::Ok, rb.push(11));
    assertValueAt(rb, 0, 11);
}

void test_ring_buffer_event_sized_payload_ordering(void) {
    bw::RingBuffer<EventSizedPayload, 3> rb;
    static_assert(decltype(rb)::capacity() == 3, "capacity() is a compile-time value");
    for (uint32_t i = 0; i < 5; ++i) {  // 5 pushes into 3 slots -> 2 overwrites
        EventSizedPayload p{};
        p.t_ms = 1000 + i;
        p.seq = i + 1;
        const bw::Err err = rb.push(p);
        TEST_ASSERT_EQUAL_INT(i < 3 ? bw::Err::Ok : bw::Err::Overflow, err);
    }
    TEST_ASSERT_EQUAL_UINT32(3, rb.size());
    TEST_ASSERT_EQUAL_UINT32(2, rb.overwrites());
    const EventSizedPayload* oldest = rb.at(0);
    TEST_ASSERT_NOT_NULL(oldest);
    TEST_ASSERT_EQUAL_UINT64(1002, oldest->t_ms);  // pushes 1 and 2 were overwritten
    TEST_ASSERT_EQUAL_UINT32(3, oldest->seq);
    const EventSizedPayload* newest = rb.at(2);
    TEST_ASSERT_NOT_NULL(newest);
    TEST_ASSERT_EQUAL_UINT32(5, newest->seq);
}

void test_ring_buffer_is_trivially_copyable(void) {
    // A window copied into a mailbox or record must not manage resources (02 §1).
    static_assert(std::is_trivially_copyable_v<bw::RingBuffer<uint32_t, 4>>,
                  "RingBuffer must stay trivially copyable (02 §1)");
    TEST_PASS_MESSAGE("RingBuffer<uint32_t,4> is trivially copyable");
}
