// Test runner for the bw_explain suite (F22). PlatformIO builds every .cpp in
// this directory into one program and does NOT generate a Unity main, so this
// file owns it. REGISTER EVERY test function here: a function that is never
// RUN_TEST-ed silently never runs. One strong setUp/tearDown per suite (same
// reason as bw_core/test_main.cpp: weak definitions do not link on MinGW/COFF).

#include <unity.h>

// test_reason_catalog.cpp
void test_catalog_completeness_pinned_to_enum(void);
void test_catalog_completeness_detects_appended_code(void);
void test_catalog_entries_reference_enumerators_symbolically(void);
void test_catalog_code_values_pinned(void);
void test_catalog_wording_has_no_diagnosis_language(void);
void test_catalog_version_is_pinned(void);

// test_explain_short.cpp
void test_explain_short_empty_mask_renders_empty(void);
void test_explain_short_every_known_code_round_trips(void);
void test_explain_short_three_bits_render_ascending_with_separator(void);
void test_explain_short_drops_bits_beyond_max(void);
void test_explain_short_unknown_bit_renders_reason_number(void);
void test_explain_short_unknown_bits_keep_positions_in_mixed_mask(void);
void test_explain_short_truncation_is_safe_at_every_capacity(void);
void test_explain_short_worst_case_fits_declared_buffer(void);
void test_explain_short_null_buffer_reports_length_without_writing(void);

// test_no_heap.cpp
void test_explain_path_allocates_no_heap(void);

// test_wire_explain_compile.cpp
void test_wire_explain_render_is_pass_through(void);
void test_wire_explain_reason_text_is_pass_through(void);
void test_wire_explain_catalog_version_is_reachable(void);

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_catalog_completeness_pinned_to_enum);
    RUN_TEST(test_catalog_completeness_detects_appended_code);
    RUN_TEST(test_catalog_entries_reference_enumerators_symbolically);
    RUN_TEST(test_catalog_code_values_pinned);
    RUN_TEST(test_catalog_wording_has_no_diagnosis_language);
    RUN_TEST(test_catalog_version_is_pinned);

    RUN_TEST(test_explain_short_empty_mask_renders_empty);
    RUN_TEST(test_explain_short_every_known_code_round_trips);
    RUN_TEST(test_explain_short_three_bits_render_ascending_with_separator);
    RUN_TEST(test_explain_short_drops_bits_beyond_max);
    RUN_TEST(test_explain_short_unknown_bit_renders_reason_number);
    RUN_TEST(test_explain_short_unknown_bits_keep_positions_in_mixed_mask);
    RUN_TEST(test_explain_short_truncation_is_safe_at_every_capacity);
    RUN_TEST(test_explain_short_worst_case_fits_declared_buffer);
    RUN_TEST(test_explain_short_null_buffer_reports_length_without_writing);

    RUN_TEST(test_explain_path_allocates_no_heap);

    RUN_TEST(test_wire_explain_render_is_pass_through);
    RUN_TEST(test_wire_explain_reason_text_is_pass_through);
    RUN_TEST(test_wire_explain_catalog_version_is_reachable);

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
