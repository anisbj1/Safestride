// bw_ui — F12 §9 test_text_layout: D7 truncation is NUL-safe, caps hold, and
// '%' inside caller data is never interpreted as a format directive.

#include <cstring>

#include <unity.h>

#include <bw/ui/text_layout.h>
#include <bw/ui/ui_config.h>

using bw::ui::copyTruncated;
using bw::ui::formatLine;
using bw::ui::LineBuffer;

void test_text_layout_caps_match_spec(void) {
    // §8/D7: the caps are the contract between F12 and F01's glyph widths.
    TEST_ASSERT_EQUAL_UINT(21, static_cast<unsigned>(bw::ui::kMaxLineCharsSmall));
    TEST_ASSERT_EQUAL_UINT(16, static_cast<unsigned>(bw::ui::kMaxLineCharsMedium));
    TEST_ASSERT_EQUAL_UINT(12, static_cast<unsigned>(bw::ui::kMaxLineCharsLarge));
    // Offset-aware caps (audit F-2): (128-18)/8 = 13 for the banner text
    // after the warning icon; (128-10)/6 = 19 for menu entries after cursor.
    TEST_ASSERT_EQUAL_UINT(13, static_cast<unsigned>(bw::ui::kMaxLineCharsBanner));
    TEST_ASSERT_EQUAL_UINT(19, static_cast<unsigned>(bw::ui::kMaxLineCharsMenuText));
    // The offsets must actually tighten the caps, not just rename them.
    TEST_ASSERT_TRUE(bw::ui::kMaxLineCharsBanner < bw::ui::kMaxLineCharsMedium);
    TEST_ASSERT_TRUE(bw::ui::kMaxLineCharsMenuText < bw::ui::kMaxLineCharsSmall);
}

void test_text_layout_exact_cap_untouched(void) {
    LineBuffer line{};
    const char* const kExact = "123456789012345678901";  // 21 chars == cap
    copyTruncated(line, kExact);
    TEST_ASSERT_EQUAL_STRING(kExact, line.data());
}

void test_text_layout_over_cap_truncates(void) {
    LineBuffer line{};
    const char* const kLong = "12345678901234567890123456789";  // 29 chars
    copyTruncated(line, kLong);
    TEST_ASSERT_EQUAL_UINT(21, static_cast<unsigned>(std::strlen(line.data())));
    TEST_ASSERT_EQUAL_STRING("123456789012345678901", line.data());
    TEST_ASSERT_EQUAL_CHAR('\0', line[line.size() - 1]);
}

void test_text_layout_empty_string(void) {
    LineBuffer line{};
    line[0] = 'x';  // prove the formatter writes the NUL even for empty input
    copyTruncated(line, "");
    TEST_ASSERT_EQUAL_STRING("", line.data());
}

void test_text_layout_percent_stays_literal(void) {
    LineBuffer line{};
    copyTruncated(line, "100% done %s!");
    TEST_ASSERT_EQUAL_STRING("100% done %s!", line.data());

    formatLine(line, "batt %u%%", 87u);
    TEST_ASSERT_EQUAL_STRING("batt 87%", line.data());
}

void test_text_layout_format_truncates_nul_safe(void) {
    std::array<char, 8> small{};
    formatLine(small, "%s", "Hello world");  // 11 chars into a 8-byte buffer
    TEST_ASSERT_EQUAL_STRING("Hello w", small.data());
}

void test_text_layout_null_text_is_empty(void) {
    LineBuffer line{};
    line[0] = 'x';
    copyTruncated(line, nullptr);
    TEST_ASSERT_EQUAL_STRING("", line.data());
}
