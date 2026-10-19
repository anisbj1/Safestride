// Test runner for the bw_sync / F20 suite.
//
// PlatformIO builds every .cpp in this directory into one program and does
// NOT generate a Unity main, so this file owns it. REGISTER EVERY test
// function here: a function that is never RUN_TEST-ed silently never runs.
// (Same pattern as test/bw_confirm/test_bw_confirm/main.cpp.)
//
// 52 tests, mapped to F20 §9: T01–T14 flow/ack/window (test_sync_session),
// T15–T22 packetizer/CRC + golden vectors V2–V4 (test_crc16, test_packetizer),
// F01–F12 + companion reboot + P01–P04 fault matrix (test_sync_fault_injection),
// W01–W02 wiring (test_wire_sync).

#include <unity.h>

// ---- crc16 (T15 + edges) -------------------------------------------------
void test_crc16_ccitt_false_check_value(void);
void test_crc16_empty_input_is_init(void);
void test_crc16_incremental_equals_block(void);
void test_crc16_detects_single_bit_flip(void);

// ---- packetizer (T16–T22, contract pins, V2–V4) --------------------------
void test_packetizer_chunk_round_trip(void);
void test_packetizer_corrupted_crc_rejected(void);
void test_packetizer_corrupted_payload_rejected(void);
void test_packetizer_mtu_floor_is_exactly_one_event(void);
void test_packetizer_mtu_255_is_five_events(void);
void test_packetizer_payload_always_whole_records(void);
void test_packetizer_unknown_version_rejected(void);
void test_packetizer_malformed_control_frames_rejected(void);
void test_packetizer_malformed_chunk_rejected(void);
void test_packetizer_encode_argument_guards(void);
void test_packetizer_contract_size_pins(void);
void test_packetizer_v2_cursor_golden(void);
void test_packetizer_v3_status_golden(void);
void test_packetizer_v4_chunk_golden(void);

// ---- sync session (T01–T14) ----------------------------------------------
void test_session_happy_path_syncs_and_marks(void);
void test_session_empty_store_status_only(void);
void test_session_request_from_mid_history(void);
void test_session_max_cap_honored_exactly(void);
void test_session_max_zero_streams_everything(void);
void test_session_status_query_gets_status(void);
void test_session_status_flags_at_edges(void);
void test_session_window_never_exceeds_max_unacked(void);
void test_session_ack_advances_marker_exactly_once(void);
void test_session_duplicate_ack_is_ignored(void);
void test_session_backward_ack_ignored(void);
void test_session_ack_of_missing_seq_ignored(void);
void test_session_ack_beyond_head_ignored(void);
void test_session_new_request_mid_stream_restarts(void);
void test_session_without_request_never_streams(void);

// ---- fault matrix (F01–F12, P01–P04) -------------------------------------
void test_fault_dropped_chunk_resends_without_loss(void);
void test_fault_duplicated_chunk_is_idempotent(void);
void test_fault_reordered_chunks_assemble_by_seq(void);
void test_fault_disconnect_mid_sync_resumes_with_no_gap_no_dup(void);
void test_fault_link_flap_twice_converges(void);
void test_fault_device_reboot_mid_sync_no_loss_no_dup(void);
void test_fault_companion_reboot_mid_transfer_resumes_from_persisted_hw(void);
void test_fault_persistent_send_failure_aborts_bounded(void);
void test_fault_lost_ack_timeout_resend_converges(void);
void test_fault_busy_send_retried_next_pump(void);
void test_fault_mtu_below_floor_sends_nothing(void);
void test_fault_store_read_failure_abandons_then_retries(void);
void test_fault_inbound_garbage_dropped_session_healthy(void);
void test_property_idempotent_replay(void);
void test_property_marker_monotonic_across_matrix(void);
void test_property_zero_loss_zero_duplicates_across_matrix(void);
void test_property_marker_never_ahead_of_companion_hw(void);

// ---- wiring (W01–W02) ----------------------------------------------------
void test_wire_pump_once_drives_real_path(void);
void test_wire_rewire_binds_second_context(void);

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_crc16_ccitt_false_check_value);
    RUN_TEST(test_crc16_empty_input_is_init);
    RUN_TEST(test_crc16_incremental_equals_block);
    RUN_TEST(test_crc16_detects_single_bit_flip);

    RUN_TEST(test_packetizer_chunk_round_trip);
    RUN_TEST(test_packetizer_corrupted_crc_rejected);
    RUN_TEST(test_packetizer_corrupted_payload_rejected);
    RUN_TEST(test_packetizer_mtu_floor_is_exactly_one_event);
    RUN_TEST(test_packetizer_mtu_255_is_five_events);
    RUN_TEST(test_packetizer_payload_always_whole_records);
    RUN_TEST(test_packetizer_unknown_version_rejected);
    RUN_TEST(test_packetizer_malformed_control_frames_rejected);
    RUN_TEST(test_packetizer_malformed_chunk_rejected);
    RUN_TEST(test_packetizer_encode_argument_guards);
    RUN_TEST(test_packetizer_contract_size_pins);
    RUN_TEST(test_packetizer_v2_cursor_golden);
    RUN_TEST(test_packetizer_v3_status_golden);
    RUN_TEST(test_packetizer_v4_chunk_golden);

    RUN_TEST(test_session_happy_path_syncs_and_marks);
    RUN_TEST(test_session_empty_store_status_only);
    RUN_TEST(test_session_request_from_mid_history);
    RUN_TEST(test_session_max_cap_honored_exactly);
    RUN_TEST(test_session_max_zero_streams_everything);
    RUN_TEST(test_session_status_query_gets_status);
    RUN_TEST(test_session_status_flags_at_edges);
    RUN_TEST(test_session_window_never_exceeds_max_unacked);
    RUN_TEST(test_session_ack_advances_marker_exactly_once);
    RUN_TEST(test_session_duplicate_ack_is_ignored);
    RUN_TEST(test_session_backward_ack_ignored);
    RUN_TEST(test_session_ack_of_missing_seq_ignored);
    RUN_TEST(test_session_ack_beyond_head_ignored);
    RUN_TEST(test_session_new_request_mid_stream_restarts);
    RUN_TEST(test_session_without_request_never_streams);

    RUN_TEST(test_fault_dropped_chunk_resends_without_loss);
    RUN_TEST(test_fault_duplicated_chunk_is_idempotent);
    RUN_TEST(test_fault_reordered_chunks_assemble_by_seq);
    RUN_TEST(test_fault_disconnect_mid_sync_resumes_with_no_gap_no_dup);
    RUN_TEST(test_fault_link_flap_twice_converges);
    RUN_TEST(test_fault_device_reboot_mid_sync_no_loss_no_dup);
    RUN_TEST(test_fault_companion_reboot_mid_transfer_resumes_from_persisted_hw);
    RUN_TEST(test_fault_persistent_send_failure_aborts_bounded);
    RUN_TEST(test_fault_lost_ack_timeout_resend_converges);
    RUN_TEST(test_fault_busy_send_retried_next_pump);
    RUN_TEST(test_fault_mtu_below_floor_sends_nothing);
    RUN_TEST(test_fault_store_read_failure_abandons_then_retries);
    RUN_TEST(test_fault_inbound_garbage_dropped_session_healthy);
    RUN_TEST(test_property_idempotent_replay);
    RUN_TEST(test_property_marker_monotonic_across_matrix);
    RUN_TEST(test_property_zero_loss_zero_duplicates_across_matrix);
    RUN_TEST(test_property_marker_never_ahead_of_companion_hw);

    RUN_TEST(test_wire_pump_once_drives_real_path);
    RUN_TEST(test_wire_rewire_binds_second_context);

    return UNITY_END();
}

void setUp(void) {}
void tearDown(void) {}
