// Test runner for the bw_core / bw_testkit contract suite (F00 §9).
//
// PlatformIO builds every .cpp in this directory into one program and does NOT
// generate a Unity main, so this file owns it. REGISTER EVERY test function here:
// a function that is never RUN_TEST-ed silently never runs.
//
// setUp()/tearDown() are defined here instead of relying on PlatformIO's generated
// unity_config.c: it provides them as __attribute__((weak)) defaults, and weak
// *definitions* do not link on this MinGW/COFF toolchain (observed: undefined
// reference to setUp/tearDown). One strong definition per suite avoids the issue and
// keeps future per-suite setup a one-line edit.

#include <unity.h>

void test_err_values_pinned_in_declaration_order(void);
void test_result_ok_true_only_for_err_ok(void);
void test_result_holds_value_on_success(void);
void test_result_value_default_constructed_when_err_only(void);
void test_result_stores_value_even_on_error_path(void);
void test_result_void_specialization(void);

void test_elapsed_no_32bit_truncation_across_2pow32(void);
void test_elapsed_zero_when_equal(void);
void test_elapsed_zero_when_now_before_since(void);
void test_elapsed_huge_forward_jump(void);
void test_elapsed_near_uint64_max_does_not_wrap(void);
void test_within_boundary_is_inclusive(void);

void test_ring_buffer_empty_first_read(void);
void test_ring_buffer_push_to_capacity(void);
void test_ring_buffer_wrap_overwrites_oldest(void);
void test_ring_buffer_indexed_order_across_wrap(void);
void test_ring_buffer_single_slot(void);
void test_ring_buffer_clear(void);
void test_ring_buffer_event_sized_payload_ordering(void);
void test_ring_buffer_is_trivially_copyable(void);

void test_spsc_fifo_order_across_wrap(void);
void test_spsc_push_full_refused_and_drop_counted(void);
void test_spsc_pop_empty_returns_false(void);
void test_spsc_interleaved_script_loses_nothing_silently(void);
void test_spsc_capacity_one_edge(void);
void test_spsc_is_trivially_copyable(void);

void test_latest_value_get_before_set_reports_no_data(void);
void test_latest_value_sequence_increments_per_set(void);
void test_latest_value_newest_wins_and_value_copied_verbatim(void);
void test_latest_value_overwrites_counts_only_unread_losses(void);
void test_latest_value_repeated_gets_are_stable(void);
void test_latest_value_is_trivially_copyable(void);

void test_contract_layout_sizes_and_alignment(void);
void test_contract_enum_values_append_only(void);
void test_contract_setting_key_and_sizes(void);
void test_contract_event_offsets(void);

void test_fault_code_values_pinned(void);
void test_fault_severity_values_pinned(void);
void test_fault_codes_abi(void);

void test_csv_split_yields_fields_without_copying(void);
void test_csv_fixture_replays_deterministically(void);
void test_csv_malformed_row_returns_corrupt(void);
void test_csv_overlong_row_respects_max_line_bytes(void);
void test_csv_missing_columns_returns_invalid_arg(void);
void test_csv_garbage_never_crashes(void);
void test_csv_ppg_fixture_replays_deterministically(void);

void test_task_health_invalid_before_first_cycle(void);
void test_task_health_first_cycle_marks_valid(void);
void test_task_health_worst_lateness_keeps_max_never_descends(void);
void test_task_health_lateness_saturates_not_wraps(void);
void test_task_health_stack_reported_as_measured(void);

void test_state_provider_no_data_until_first_publish(void);
void test_state_provider_returns_newest_snapshot(void);
void test_feature_provider_no_data_until_publish(void);
void test_feature_provider_channels_are_independent(void);
void test_settings_unregistered_key_returns_not_found(void);
void test_settings_get_returns_registered_default(void);
void test_settings_type_mismatch_returns_invalid_arg(void);
void test_settings_out_of_range_is_clamped_not_rejected(void);
void test_settings_in_range_stored_verbatim(void);
void test_settings_list_returns_range_table(void);

void test_settings_list_truncates_at_max_but_returns_total(void);

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_err_values_pinned_in_declaration_order);
    RUN_TEST(test_result_ok_true_only_for_err_ok);
    RUN_TEST(test_result_holds_value_on_success);
    RUN_TEST(test_result_value_default_constructed_when_err_only);
    RUN_TEST(test_result_stores_value_even_on_error_path);
    RUN_TEST(test_result_void_specialization);

    RUN_TEST(test_elapsed_no_32bit_truncation_across_2pow32);
    RUN_TEST(test_elapsed_zero_when_equal);
    RUN_TEST(test_elapsed_zero_when_now_before_since);
    RUN_TEST(test_elapsed_huge_forward_jump);
    RUN_TEST(test_elapsed_near_uint64_max_does_not_wrap);
    RUN_TEST(test_within_boundary_is_inclusive);

    RUN_TEST(test_ring_buffer_empty_first_read);
    RUN_TEST(test_ring_buffer_push_to_capacity);
    RUN_TEST(test_ring_buffer_wrap_overwrites_oldest);
    RUN_TEST(test_ring_buffer_indexed_order_across_wrap);
    RUN_TEST(test_ring_buffer_single_slot);
    RUN_TEST(test_ring_buffer_clear);
    RUN_TEST(test_ring_buffer_event_sized_payload_ordering);
    RUN_TEST(test_ring_buffer_is_trivially_copyable);

    RUN_TEST(test_spsc_fifo_order_across_wrap);
    RUN_TEST(test_spsc_push_full_refused_and_drop_counted);
    RUN_TEST(test_spsc_pop_empty_returns_false);
    RUN_TEST(test_spsc_interleaved_script_loses_nothing_silently);
    RUN_TEST(test_spsc_capacity_one_edge);
    RUN_TEST(test_spsc_is_trivially_copyable);

    RUN_TEST(test_latest_value_get_before_set_reports_no_data);
    RUN_TEST(test_latest_value_sequence_increments_per_set);
    RUN_TEST(test_latest_value_newest_wins_and_value_copied_verbatim);
    RUN_TEST(test_latest_value_overwrites_counts_only_unread_losses);
    RUN_TEST(test_latest_value_repeated_gets_are_stable);
    RUN_TEST(test_latest_value_is_trivially_copyable);

    RUN_TEST(test_contract_layout_sizes_and_alignment);
    RUN_TEST(test_contract_enum_values_append_only);
    RUN_TEST(test_contract_setting_key_and_sizes);
    RUN_TEST(test_contract_event_offsets);

    RUN_TEST(test_fault_code_values_pinned);
    RUN_TEST(test_fault_severity_values_pinned);
    RUN_TEST(test_fault_codes_abi);

    RUN_TEST(test_csv_split_yields_fields_without_copying);
    RUN_TEST(test_csv_fixture_replays_deterministically);
    RUN_TEST(test_csv_malformed_row_returns_corrupt);
    RUN_TEST(test_csv_overlong_row_respects_max_line_bytes);
    RUN_TEST(test_csv_missing_columns_returns_invalid_arg);
    RUN_TEST(test_csv_garbage_never_crashes);
    RUN_TEST(test_csv_ppg_fixture_replays_deterministically);

    RUN_TEST(test_task_health_invalid_before_first_cycle);
    RUN_TEST(test_task_health_first_cycle_marks_valid);
    RUN_TEST(test_task_health_worst_lateness_keeps_max_never_descends);
    RUN_TEST(test_task_health_lateness_saturates_not_wraps);
    RUN_TEST(test_task_health_stack_reported_as_measured);

    RUN_TEST(test_state_provider_no_data_until_first_publish);
    RUN_TEST(test_state_provider_returns_newest_snapshot);
    RUN_TEST(test_feature_provider_no_data_until_publish);
    RUN_TEST(test_feature_provider_channels_are_independent);
    RUN_TEST(test_settings_unregistered_key_returns_not_found);
    RUN_TEST(test_settings_get_returns_registered_default);
    RUN_TEST(test_settings_type_mismatch_returns_invalid_arg);
    RUN_TEST(test_settings_out_of_range_is_clamped_not_rejected);
    RUN_TEST(test_settings_in_range_stored_verbatim);
    RUN_TEST(test_settings_list_returns_range_table);
    RUN_TEST(test_settings_list_truncates_at_max_but_returns_total);

    return UNITY_END();
}

void setUp(void) {}

void tearDown(void) {}
