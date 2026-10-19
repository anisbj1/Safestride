#include <bw/containers/latest_value.h>

#include <unity.h>

#include <cstdint>

namespace {

struct Probe {
    bw::TimeMs t_ms;
    uint32_t id;
    float value;
};

}  // namespace

void test_latest_value_get_before_set_reports_no_data(void) {
    bw::LatestValue<Probe> box;
    Probe out{999, 999, 999.0f};
    TEST_ASSERT_FALSE(box.get(out));          // false — not a magic value (02 §1)
    TEST_ASSERT_EQUAL_UINT64(999, out.t_ms);  // out untouched when there is no data
    TEST_ASSERT_FALSE(box.hasValue());
    TEST_ASSERT_EQUAL_UINT32(0, box.sequence());
    TEST_ASSERT_EQUAL_UINT32(0, box.overwrites());
}

void test_latest_value_sequence_increments_per_set(void) {
    bw::LatestValue<Probe> box;
    Probe p{1, 1, 0.5f};
    box.set(p);
    TEST_ASSERT_EQUAL_UINT32(1, box.sequence());
    box.set(p);
    TEST_ASSERT_EQUAL_UINT32(2, box.sequence());
    box.set(p);
    TEST_ASSERT_EQUAL_UINT32(3, box.sequence());
    TEST_ASSERT_TRUE(box.hasValue());
}

void test_latest_value_newest_wins_and_value_copied_verbatim(void) {
    bw::LatestValue<Probe> box;
    box.set(Probe{1000, 1, 0.25f});
    box.set(Probe{2000, 2, 0.75f});
    box.set(Probe{3000, 3, 1.5f});  // intermediates dropped by design

    Probe out{};
    TEST_ASSERT_TRUE(box.get(out));
    TEST_ASSERT_EQUAL_UINT64(3000, out.t_ms);  // member-for-member copy of the newest
    TEST_ASSERT_EQUAL_UINT32(3, out.id);
    TEST_ASSERT_EQUAL_FLOAT(1.5f, out.value);
}

void test_latest_value_overwrites_counts_only_unread_losses(void) {
    bw::LatestValue<Probe> box;
    box.set(Probe{1, 1, 0.0f});  // nobody read the first value...
    box.set(Probe{2, 2, 0.0f});  // ...so this set really did lose one
    TEST_ASSERT_EQUAL_UINT32(1, box.overwrites());

    Probe out{};
    TEST_ASSERT_TRUE(box.get(out));  // consumer caught up

    box.set(Probe{3, 3, 0.0f});  // value was read first: nothing lost, not counted
    TEST_ASSERT_EQUAL_UINT32(1, box.overwrites());

    box.set(Probe{4, 4, 0.0f});  // 3 unread -> lost again
    TEST_ASSERT_EQUAL_UINT32(2, box.overwrites());

    TEST_ASSERT_TRUE(box.get(out));
    TEST_ASSERT_EQUAL_UINT32(4, out.id);
    // Sequence keeps counting every publish regardless of reads.
    TEST_ASSERT_EQUAL_UINT32(4, box.sequence());
}

void test_latest_value_repeated_gets_are_stable(void) {
    bw::LatestValue<Probe> box;
    box.set(Probe{42, 7, 3.25f});
    Probe a{};
    Probe b{};
    TEST_ASSERT_TRUE(box.get(a));
    TEST_ASSERT_TRUE(box.get(b));  // same value again — no consume-once semantics
    TEST_ASSERT_EQUAL_UINT64(a.t_ms, b.t_ms);
    TEST_ASSERT_EQUAL_UINT32(a.id, b.id);
    TEST_ASSERT_EQUAL_UINT32(0, box.overwrites());  // two gets lost nothing
}

void test_latest_value_is_trivially_copyable(void) {
    static_assert(std::is_trivially_copyable_v<bw::LatestValue<Probe>>,
                  "LatestValue must stay trivially copyable (02 §1)");
    TEST_PASS_MESSAGE("LatestValue<Probe> is trivially copyable");
}
