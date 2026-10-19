// Test runner for the bw_ble / F19 suite.
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. REGISTER EVERY test
// function here: a function that is never RUN_TEST-ed silently never runs.
// (Same pattern as test/bw_state/test_bw_state/main.cpp.)

#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

// ---- codec: live status (spec 9 row 1, D6) ---------------------------------
void test_live_status_round_trip(void);
void test_live_status_version_byte(void);
void test_live_status_every_enum_value(void);
void test_live_status_in_state_saturates(void);
void test_live_status_confidence_clamps(void);
void test_live_status_reasons_full_32_bits(void);
void test_live_status_encode_rejects_small_cap(void);

// ---- codec: heart rate (row 2) --------------------------------------------
void test_hr_round_trip_uint8(void);
void test_hr_round_trip_uint16(void);
void test_hr_refuses_invalid(void);
void test_hr_encode_rejects_small_cap(void);
void test_hr_decode_rejects_foreign_flags(void);

// ---- codec: command (row 3) -----------------------------------------------
void test_command_happy_path(void);
void test_command_wrong_length(void);
void test_command_unknown_type_rejected(void);

// ---- codec: config (row 4) ------------------------------------------------
void test_config_dump_order_and_totals(void);
void test_config_dump_truncates_at_entry_boundary(void);
void test_config_dump_rejects_null(void);
void test_config_write_happy_paths(void);
void test_config_write_unknown_name(void);
void test_config_write_type_mismatch(void);
void test_config_write_out_of_range_clamps(void);
void test_config_write_version_and_length(void);

// ---- codec: sync control (row 5, re-scoped by CR-019 G1 ruling) -----------
void test_sync_control_length_bounds(void);
void test_sync_control_bytes_stay_opaque(void);
void test_sync_control_passthrough_round_trip(void);

// ---- codec: event (row 6, CR-002 image) -----------------------------------
void test_event_passthrough_is_verbatim(void);
void test_event_encode_rejects_small_cap(void);

// ---- event notifier (row 7, D7) -------------------------------------------
void test_notifier_empty_store(void);
void test_notifier_first_subscribe_baselines_no_burst(void);
void test_notifier_newest_only_skips_intermediates(void);
void test_notifier_store_error_skips_silently(void);
void test_notifier_unsubscribe_stops_reads(void);
void test_notifier_null_store_is_honest(void);

// ---- link state tracker (row 8, D10/CR-018/CR-019) ------------------------
void test_tracker_no_snapshot_before_first_event(void);
void test_tracker_up_produces_unencrypted_state(void);
void test_tracker_encryption_and_subscription_gates_linkup(void);
void test_tracker_mtu_transitions(void);
void test_tracker_down_resets_per_link_state(void);
void test_tracker_second_link_starts_clean(void);
void test_link_state_provider_over_mailbox(void);

// ---- sync frame queue (CR-019 inbound path, D9) ---------------------------
void test_frame_queue_empty_receive(void);
void test_frame_queue_round_trip(void);
void test_frame_queue_fifo_order(void);
void test_frame_queue_full_is_counted_refusal(void);
void test_frame_queue_push_length_bounds(void);
void test_frame_queue_oversize_cap_drops_and_counts(void);
void test_frame_queue_generation_discards_on_disconnect(void);

// ---- contract (row 9, spec DoD/H9) ----------------------------------------
void test_contract_uuid_scheme(void);
void test_contract_uuid_scheme_bounds(void);
void test_contract_service_table(void);
void test_contract_characteristic_table(void);
void test_contract_pinned_constants(void);
void test_contract_live_status_fits_mandate(void);

// ---- codec: battery (AUD-7: unknown = 0xFF, never a fabricated level) ------
void test_battery_invalid_reading_is_unknown(void);
void test_battery_nan_and_infinity_are_unknown(void);
void test_battery_rounds_and_clamps(void);

// ---- publish gate (AUD-1 layer 2 / AUD-6: THE notify gate) ------------------
void test_publish_gate_unencrypted_link_emits_nothing(void);
void test_publish_gate_encrypted_link_delivers(void);
void test_publish_gate_drop_is_counted_not_queued(void);
void test_publish_gate_no_link_never_counts(void);
void test_publish_gate_hr_is_capped_at_1hz(void);
void test_publish_gate_cadence_per_channel_independent(void);

// ---- sync transport gate (spec 15 / CR-019, D12; M4 bench bugs) ------------
void test_transport_no_link_source_is_honest_no_link(void);
void test_transport_link_up_follows_monitor_up_and_down(void);
void test_transport_max_send_bytes_follows_monitor(void);
void test_transport_send_attempts_when_link_usable(void);
void test_transport_send_refused_notready_while_link_down(void);
void test_transport_send_argument_bounds(void);
void test_transport_busy_from_stack_propagates(void);
void test_transport_host_base_delivers_nothing_silently(void);
void test_transport_receive_delegates_inbound_queue(void);

// ---- wiring compile + seams (spec 18, DoD/H7) -----------------------------
void test_wire_getters_null_before_wire(void);
void test_wire_idempotent_and_null_seams(void);
void test_wire_task_constants_pinned(void);
void test_wire_transport_binds_link_slot(void);

// Order note: the wiring tests share one f19:: static state and must run
// in the registered order (the compile-test file header); the last one
// swaps a fake link into that shared state on purpose.
int main(int, char**) {
    UNITY_BEGIN();

    // codec: live status
    RUN_TEST(test_live_status_round_trip);
    RUN_TEST(test_live_status_version_byte);
    RUN_TEST(test_live_status_every_enum_value);
    RUN_TEST(test_live_status_in_state_saturates);
    RUN_TEST(test_live_status_confidence_clamps);
    RUN_TEST(test_live_status_reasons_full_32_bits);
    RUN_TEST(test_live_status_encode_rejects_small_cap);

    // codec: heart rate
    RUN_TEST(test_hr_round_trip_uint8);
    RUN_TEST(test_hr_round_trip_uint16);
    RUN_TEST(test_hr_refuses_invalid);
    RUN_TEST(test_hr_encode_rejects_small_cap);
    RUN_TEST(test_hr_decode_rejects_foreign_flags);

    // codec: command
    RUN_TEST(test_command_happy_path);
    RUN_TEST(test_command_wrong_length);
    RUN_TEST(test_command_unknown_type_rejected);

    // codec: config
    RUN_TEST(test_config_dump_order_and_totals);
    RUN_TEST(test_config_dump_truncates_at_entry_boundary);
    RUN_TEST(test_config_dump_rejects_null);
    RUN_TEST(test_config_write_happy_paths);
    RUN_TEST(test_config_write_unknown_name);
    RUN_TEST(test_config_write_type_mismatch);
    RUN_TEST(test_config_write_out_of_range_clamps);
    RUN_TEST(test_config_write_version_and_length);

    // codec: sync control
    RUN_TEST(test_sync_control_length_bounds);
    RUN_TEST(test_sync_control_bytes_stay_opaque);
    RUN_TEST(test_sync_control_passthrough_round_trip);

    // codec: event
    RUN_TEST(test_event_passthrough_is_verbatim);
    RUN_TEST(test_event_encode_rejects_small_cap);

    // event notifier
    RUN_TEST(test_notifier_empty_store);
    RUN_TEST(test_notifier_first_subscribe_baselines_no_burst);
    RUN_TEST(test_notifier_newest_only_skips_intermediates);
    RUN_TEST(test_notifier_store_error_skips_silently);
    RUN_TEST(test_notifier_unsubscribe_stops_reads);
    RUN_TEST(test_notifier_null_store_is_honest);

    // link state tracker
    RUN_TEST(test_tracker_no_snapshot_before_first_event);
    RUN_TEST(test_tracker_up_produces_unencrypted_state);
    RUN_TEST(test_tracker_encryption_and_subscription_gates_linkup);
    RUN_TEST(test_tracker_mtu_transitions);
    RUN_TEST(test_tracker_down_resets_per_link_state);
    RUN_TEST(test_tracker_second_link_starts_clean);
    RUN_TEST(test_link_state_provider_over_mailbox);

    // sync frame queue
    RUN_TEST(test_frame_queue_empty_receive);
    RUN_TEST(test_frame_queue_round_trip);
    RUN_TEST(test_frame_queue_fifo_order);
    RUN_TEST(test_frame_queue_full_is_counted_refusal);
    RUN_TEST(test_frame_queue_push_length_bounds);
    RUN_TEST(test_frame_queue_oversize_cap_drops_and_counts);
    RUN_TEST(test_frame_queue_generation_discards_on_disconnect);

    // contract
    RUN_TEST(test_contract_uuid_scheme);
    RUN_TEST(test_contract_uuid_scheme_bounds);
    RUN_TEST(test_contract_service_table);
    RUN_TEST(test_contract_characteristic_table);
    RUN_TEST(test_contract_pinned_constants);
    RUN_TEST(test_contract_live_status_fits_mandate);

    // codec: battery (AUD-7)
    RUN_TEST(test_battery_invalid_reading_is_unknown);
    RUN_TEST(test_battery_nan_and_infinity_are_unknown);
    RUN_TEST(test_battery_rounds_and_clamps);

    // publish gate (AUD-1 layer 2 / AUD-6)
    RUN_TEST(test_publish_gate_unencrypted_link_emits_nothing);
    RUN_TEST(test_publish_gate_encrypted_link_delivers);
    RUN_TEST(test_publish_gate_drop_is_counted_not_queued);
    RUN_TEST(test_publish_gate_no_link_never_counts);
    RUN_TEST(test_publish_gate_hr_is_capped_at_1hz);
    RUN_TEST(test_publish_gate_cadence_per_channel_independent);

    // sync transport gate (D12; M4 bench bugs 1/2 host contract)
    RUN_TEST(test_transport_no_link_source_is_honest_no_link);
    RUN_TEST(test_transport_link_up_follows_monitor_up_and_down);
    RUN_TEST(test_transport_max_send_bytes_follows_monitor);
    RUN_TEST(test_transport_send_attempts_when_link_usable);
    RUN_TEST(test_transport_send_refused_notready_while_link_down);
    RUN_TEST(test_transport_send_argument_bounds);
    RUN_TEST(test_transport_busy_from_stack_propagates);
    RUN_TEST(test_transport_host_base_delivers_nothing_silently);
    RUN_TEST(test_transport_receive_delegates_inbound_queue);

    // wiring (LAST: shared static state, order-dependent)
    RUN_TEST(test_wire_getters_null_before_wire);
    RUN_TEST(test_wire_idempotent_and_null_seams);
    RUN_TEST(test_wire_task_constants_pinned);
    RUN_TEST(test_wire_transport_binds_link_slot);

    return UNITY_END();
}
