// bw_explain — F22 §9 rendering tests: formatting, ordering (max
// kMaxShortReasons by ascending bit index), unknown codes -> 'reason #N',
// empty mask, and snprintf-style truncation safety at EVERY capacity (§6 D3).

#include <cstring>
#include <string>

#include <unity.h>

#include <bw/explain/explain_short.h>
#include <bw/explain/reason_catalog.h>

namespace {

// Wide enough for any untruncated render (worst case < kExplainShortBufSize).
char g_full[128];

std::string render(bw::ReasonMask mask) {
    const size_t n = bw::explain::explainShort(mask, g_full, sizeof g_full);
    TEST_ASSERT_TRUE(n < sizeof g_full);
    return std::string(g_full, n);
}

}  // namespace

void test_explain_short_empty_mask_renders_empty(void) {
    char buf[16];
    std::memset(buf, 'x', sizeof buf);
    const size_t n = bw::explain::explainShort(0, buf, sizeof buf);
    TEST_ASSERT_EQUAL_UINT(0, n);
    TEST_ASSERT_EQUAL_STRING("", buf);
    TEST_ASSERT_EQUAL_UINT('x', buf[1]);  // only buf[0] became the NUL
}

void test_explain_short_every_known_code_round_trips(void) {
    char buf[64];
    for (uint8_t i = 0; i < bw::explain::kReasonCatalogCount; ++i) {
        const bw::ReasonMask mask = static_cast<bw::ReasonMask>(1) << i;
        const size_t n = bw::explain::explainShort(mask, buf, sizeof buf);
        const char* text = bw::explain::reasonText(static_cast<bw::ReasonCode>(i));
        TEST_ASSERT_NOT_NULL(text);
        TEST_ASSERT_EQUAL_STRING(text, buf);
        TEST_ASSERT_EQUAL_UINT(std::strlen(text), n);
    }
}

void test_explain_short_three_bits_render_ascending_with_separator(void) {
    char buf[64];
    // Bits handed in scrambled order: output follows BIT order (§6 D4), joined
    // with ", ". Golden pins the join format and the current wording in one
    // readable line — rewording the catalog updates this line on purpose.
    const bw::ReasonMask mask = (1u << 5) | (1u << 0) | (1u << 2);
    const size_t n = bw::explain::explainShort(mask, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("low motion, low hr, typical time", buf);
    TEST_ASSERT_EQUAL_UINT(32, n);
}

void test_explain_short_drops_bits_beyond_max(void) {
    char buf[64];
    char three[64];
    // Four set bits: only the first three by ascending index survive; the
    // caller reports the drop via popcount(mask) (§6 D4).
    const bw::ReasonMask four = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 30);
    const bw::ReasonMask only_three = (1u << 0) | (1u << 1) | (1u << 2);
    const size_t n_four = bw::explain::explainShort(four, buf, sizeof buf);
    const size_t n_three = bw::explain::explainShort(only_three, three, sizeof three);
    TEST_ASSERT_EQUAL_STRING(three, buf);
    TEST_ASSERT_EQUAL_UINT(n_three, n_four);
}

void test_explain_short_unknown_bit_renders_reason_number(void) {
    // Bits 18..31 have no enumerator today (18 codes, bits 0..17) — synthetic
    // unknowns that stay valid until the enum actually grows (§10).
    char buf[64];
    bw::explain::explainShort(static_cast<bw::ReasonMask>(1) << 20, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("reason #20", buf);
    bw::explain::explainShort(static_cast<bw::ReasonMask>(1) << 31, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("reason #31", buf);
}

void test_explain_short_unknown_bits_keep_positions_in_mixed_mask(void) {
    char buf[64];
    // Known bit first, unknowns inline after it at their own positions —
    // never collapsed, never silently skipped (§10, §6 D4).
    const bw::ReasonMask mask = (static_cast<bw::ReasonMask>(1) << 0) |
                                (static_cast<bw::ReasonMask>(1) << 20) |
                                (static_cast<bw::ReasonMask>(1) << 31);
    bw::explain::explainShort(mask, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("low motion, reason #20, reason #31", buf);
}

void test_explain_short_truncation_is_safe_at_every_capacity(void) {
    const bw::ReasonMask mask = (1u << 0) | (1u << 2) | (1u << 20);
    const std::string full = render(mask);
    TEST_ASSERT_TRUE(!full.empty());

    char ref[128];
    std::memset(ref, 'x', sizeof ref);
    char buf[128];
    for (size_t cap = 0; cap <= full.size() + 4; ++cap) {
        std::memset(buf, 'x', sizeof buf);
        const size_t n = bw::explain::explainShort(mask, buf, cap);
        // snprintf semantics: the return value is ALWAYS the untruncated length.
        TEST_ASSERT_EQUAL_UINT(full.size(), n);
        if (cap == 0) {
            TEST_ASSERT_EQUAL_MEMORY(ref, buf, sizeof buf);  // no write at all
            continue;
        }
        const size_t stored = (full.size() < cap) ? full.size() : cap - 1;
        TEST_ASSERT_EQUAL_UINT('\0', buf[stored]);  // NUL-terminated
        TEST_ASSERT_EQUAL_STRING_LEN(full.c_str(), buf, stored);
        TEST_ASSERT_EQUAL_MEMORY(ref + cap, buf + cap, sizeof buf - cap);  // no overflow
    }
}

void test_explain_short_worst_case_fits_declared_buffer(void) {
    char buf[bw::explain::kExplainShortBufSize];
    std::memset(buf, 'x', sizeof buf);
    const size_t n = bw::explain::explainShort(0xFFFFFFFFu, buf, sizeof buf);
    TEST_ASSERT_TRUE(n < bw::explain::kExplainShortBufSize);  // room for the NUL
    TEST_ASSERT_EQUAL_UINT('\0', buf[n]);
    TEST_ASSERT_EQUAL_UINT(n, std::strlen(buf));
    // Exactly kMaxShortReasons - 1 separators => exactly kMaxShortReasons items.
    unsigned separators = 0;
    for (const char* p = std::strstr(buf, ", "); p != nullptr; p = std::strstr(p + 1, ", ")) {
        ++separators;
    }
    TEST_ASSERT_EQUAL_UINT(bw::explain::kMaxShortReasons - 1, separators);
}

void test_explain_short_null_buffer_reports_length_without_writing(void) {
    const bw::ReasonMask mask = (1u << 0) | (1u << 2) | (1u << 20);
    const size_t full = render(mask).size();
    TEST_ASSERT_EQUAL_UINT(full, bw::explain::explainShort(mask, nullptr, 8));
}
