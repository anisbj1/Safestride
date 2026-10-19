// Test runner for the bw_ml / F05 suite.
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. REGISTER EVERY test
// function here: a function that is never RUN_TEST-ed silently never runs.
// (Same pattern as test/bw_activity/test_bw_activity/main.cpp: one strong
// setUp/tearDown definition per suite, weak defaults do not link.)

#include <unity.h>

// ---- handshake (D3 / FEATURE_SCHEMA.md §3) --------------------------------
void test_handshake_wrong_schema_id_rejected(void);
void test_handshake_tree_wrong_schema_version_rejected(void);
void test_handshake_tree_wrong_schema_id_rejected(void);
void test_handshake_wrong_schema_version_rejected(void);
void test_handshake_count_mismatch_rejected(void);
void test_handshake_nan_feature_rejected(void);
void test_handshake_inf_feature_rejected(void);
void test_handshake_zero_classes_corrupt(void);
void test_handshake_null_class_names_corrupt(void);
void test_handshake_zero_trees_corrupt(void);
void test_handshake_tree_cycle_corrupt(void);
void test_handshake_tree_out_of_range_index_corrupt(void);
void test_handshake_tree_out_of_range_feature_index_corrupt(void);
void test_handshake_linear_weights_overflow_corrupt(void);
void test_handshake_predict_output_untouched_on_error(void);
void test_handshake_contributions_output_untouched_on_error(void);

// ---- linear model (D7) ----------------------------------------------------
void test_linear_fixture_parity(void);
void test_linear_probability_sums_to_one(void);
void test_linear_large_weights_finite_and_normalized(void);
void test_linear_monotonic_positive_weight(void);
void test_linear_determinism_bit_identical(void);
void test_linear_timestamp_irrelevant(void);
void test_linear_confidence_is_max_p(void);

// ---- tree ensemble (D6) ---------------------------------------------------
void test_tree_fixture_parity(void);
void test_tree_soft_vote_and_boundary_rule(void);
void test_tree_probability_sums_to_one(void);
void test_tree_depth_within_limit(void);
void test_tree_counts_within_budget(void);

// ---- contributions (D7) ---------------------------------------------------
void test_contributions_fixture_values(void);
void test_contributions_feature_index_order(void);
void test_contributions_cap_at_max(void);
void test_contributions_zero_max_rejected(void);
void test_contributions_nullptr_rejected(void);
void test_contributions_tree_unsupported(void);
void test_contributions_bias_excluded(void);

// ---- determinism / no heap (§9) -------------------------------------------
void test_no_heap_no_allocation_over_1000_predicts(void);

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_handshake_wrong_schema_id_rejected);
    RUN_TEST(test_handshake_tree_wrong_schema_version_rejected);
    RUN_TEST(test_handshake_tree_wrong_schema_id_rejected);
    RUN_TEST(test_handshake_wrong_schema_version_rejected);
    RUN_TEST(test_handshake_count_mismatch_rejected);
    RUN_TEST(test_handshake_nan_feature_rejected);
    RUN_TEST(test_handshake_inf_feature_rejected);
    RUN_TEST(test_handshake_zero_classes_corrupt);
    RUN_TEST(test_handshake_null_class_names_corrupt);
    RUN_TEST(test_handshake_zero_trees_corrupt);
    RUN_TEST(test_handshake_tree_cycle_corrupt);
    RUN_TEST(test_handshake_tree_out_of_range_index_corrupt);
    RUN_TEST(test_handshake_tree_out_of_range_feature_index_corrupt);
    RUN_TEST(test_handshake_linear_weights_overflow_corrupt);
    RUN_TEST(test_handshake_predict_output_untouched_on_error);
    RUN_TEST(test_handshake_contributions_output_untouched_on_error);

    RUN_TEST(test_linear_fixture_parity);
    RUN_TEST(test_linear_probability_sums_to_one);
    RUN_TEST(test_linear_large_weights_finite_and_normalized);
    RUN_TEST(test_linear_monotonic_positive_weight);
    RUN_TEST(test_linear_determinism_bit_identical);
    RUN_TEST(test_linear_timestamp_irrelevant);
    RUN_TEST(test_linear_confidence_is_max_p);

    RUN_TEST(test_tree_fixture_parity);
    RUN_TEST(test_tree_soft_vote_and_boundary_rule);
    RUN_TEST(test_tree_probability_sums_to_one);
    RUN_TEST(test_tree_depth_within_limit);
    RUN_TEST(test_tree_counts_within_budget);

    RUN_TEST(test_contributions_fixture_values);
    RUN_TEST(test_contributions_feature_index_order);
    RUN_TEST(test_contributions_cap_at_max);
    RUN_TEST(test_contributions_zero_max_rejected);
    RUN_TEST(test_contributions_nullptr_rejected);
    RUN_TEST(test_contributions_tree_unsupported);
    RUN_TEST(test_contributions_bias_excluded);

    RUN_TEST(test_no_heap_no_allocation_over_1000_predicts);

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
