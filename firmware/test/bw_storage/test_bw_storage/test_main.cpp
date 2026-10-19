// Test runner for the bw_storage suite (F18 spec §15.5).
//
// PlatformIO builds every .cpp in this directory into one program and does NOT
// generate a Unity main, so this file owns it. REGISTER EVERY test function
// here: a function that is never RUN_TEST-ed silently never runs.
//
// setUp()/tearDown() are defined here instead of relying on PlatformIO's
// generated unity_config.c: it provides them as __attribute__((weak))
// defaults, and weak *definitions* do not link on this MinGW/COFF toolchain
// (observed and documented in bw_core/test_main.cpp). One strong definition
// per suite avoids the issue.

#include <unity.h>

// --- test_record_codec.cpp (D4 framing) --------------------------------------
void test_crc32_known_answer(void);
void test_crc32_split_update_equals_single_pass(void);
void test_encode_decode_event_roundtrip(void);
void test_decode_short_buffer_is_corrupt(void);
void test_decode_bad_magic_is_corrupt(void);
void test_decode_flipped_payload_byte_is_corrupt(void);
void test_encode_rejects_unknown_type_and_oversize(void);
void test_header_pre_filter_accepts_only_valid_headers(void);
void test_label_patch_and_marker_roundtrip(void);

// --- test_kv_store.cpp (D7) --------------------------------------------------
void test_kv_put_get_roundtrip(void);
void test_kv_missing_key_returns_not_found(void);
void test_kv_roundtrip_across_reboot(void);
void test_kv_corrupt_blob_discards_and_counts(void);
void test_kv_torn_put_leaves_old_value(void);
void test_kv_invalid_keys_rejected(void);
void test_kv_oversize_value_rejected(void);
void test_kv_small_buffer_returns_full_untouched(void);
void test_kv_stale_tmp_removed_on_next_put(void);
void test_kv_empty_value_roundtrip(void);
void test_kv_get_crc_validates_after_overwrite(void);

// --- test_window_store.cpp (spec §8 raw windows) ----------------------------
void test_window_put_get_roundtrip(void);
void test_window_missing_seq_returns_not_found(void);
void test_window_index_rebuilt_across_reboot(void);
void test_window_count_limit_evicts_oldest(void);
void test_window_reput_same_seq_is_idempotent(void);
void test_window_torn_write_discarded_at_mount(void);
void test_window_corrupt_crc_returns_corrupt(void);
void test_window_small_buffer_returns_full(void);
void test_window_invalid_args_rejected(void);
void test_window_fs_full_evicts_then_faults(void);
void test_window_mount_read_failure_keeps_blob(void);  // audit F-04b

// --- test_event_store.cpp (T1..T15 + audit F-01/F-02/F-03/F-04/F-07/F-10) ----
void test_store_first_append_assigns_seq_one(void);
void test_store_append_accepts_preassigned_seq(void);
void test_store_append_rejects_sequence_violation(void);
void test_store_read_empty_returns_zero(void);
void test_store_read_paging_inclusive_cursor(void);
void test_store_append_is_persisted_verbatim(void);
void test_store_seq_continuity_across_reboot(void);
void test_store_corrupt_record_skips_with_counter(void);
void test_store_torn_tail_truncated_at_mount(void);
void test_store_corrupt_segment_skipped_not_fatal(void);
void test_store_queue_full_refused_nothing_consumed(void);
void test_store_label_applied_in_read_and_persists(void);
void test_store_label_unknown_seq_and_bad_label_rejected(void);
void test_store_torn_label_patch_reverts_after_reboot(void);
void test_store_mark_synced_idempotent_and_persists(void);
void test_store_mark_synced_rejects_backwards_and_beyond(void);
void test_store_sync_cursor_conservative_after_power_cut(void);
void test_store_full_refuses_unsynced_then_evicts_synced(void);
void test_store_rotation_at_exact_segment_boundary(void);
void test_store_op_failures_reported_and_store_survives(void);
void test_store_health_false_before_pump_true_after(void);
void test_store_health_counters_accumulate(void);
void test_store_pump_idle_returns_zero_and_no_flash_ops(void);
void test_store_reserved_seq_survives_failed_pump_apply(void);                   // audit F-01
void test_segment_manager_state_within_budget(void);                             // audit F-02/F-09
void test_store_mount_unformatted_reports_and_counts(void);                      // audit F-03
void test_store_mount_read_failure_keeps_segment(void);                          // audit F-04
void test_store_queue_guard_wraps_entry_points(void);                            // audit F-07
void test_store_label_patch_wins_after_cache_eviction(void);                     // audit F-10
void test_store_inband_label_survives_read_without_patch(void);                  // audit F-10
void test_store_patch_cache_republish_never_pairs_new_seq_with_old_label(void);  // NEW-M1
void test_store_patch_for_validates_entry_across_a_concurrent_republish(void);   // NEW-M1

// --- test_doc_claims.cpp (audit F-05 comment truth + F-09 spec numbers) -------
void test_doc_wire_storage_write_claim_is_true(void);
void test_doc_spec_numbers_match_code(void);
void test_doc_thread_model_lists_shared_state(void);          // audit NEW-L1
void test_doc_counters_header_names_format_recoveries(void);  // audit NEW-L2
void test_doc_partition_label_matches_csv(void);              // bench M3 device fix
void test_vfs_path_prefixes_bare_relative_names(void);        // bench round-2 path fix
void test_vfs_path_is_idempotent_for_prefixed_input(void);    // bench round-2 path fix
void test_vfs_path_cut_name_agrees_across_builders(void);     // bench round-2 path fix
void test_vfs_path_bounds_and_bad_args_rejected(void);        // bench round-2 path fix

// --- test_torn_write.cpp (T3 byte-offset DoD sweeps) -------------------------
void test_torn_write_runtime_repair_at_every_byte_offset(void);
void test_torn_write_power_cut_at_every_byte_offset(void);
void test_torn_write_rotation_header_at_every_byte_offset(void);
void test_torn_write_new_segment_first_record_at_every_byte_offset(void);
void test_torn_write_straddles_segment_end_at_every_byte_offset(void);
void test_torn_write_sync_marker_at_every_byte_offset(void);

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_crc32_known_answer);
    RUN_TEST(test_crc32_split_update_equals_single_pass);
    RUN_TEST(test_encode_decode_event_roundtrip);
    RUN_TEST(test_decode_short_buffer_is_corrupt);
    RUN_TEST(test_decode_bad_magic_is_corrupt);
    RUN_TEST(test_decode_flipped_payload_byte_is_corrupt);
    RUN_TEST(test_encode_rejects_unknown_type_and_oversize);
    RUN_TEST(test_header_pre_filter_accepts_only_valid_headers);
    RUN_TEST(test_label_patch_and_marker_roundtrip);

    RUN_TEST(test_kv_put_get_roundtrip);
    RUN_TEST(test_kv_missing_key_returns_not_found);
    RUN_TEST(test_kv_roundtrip_across_reboot);
    RUN_TEST(test_kv_corrupt_blob_discards_and_counts);
    RUN_TEST(test_kv_torn_put_leaves_old_value);
    RUN_TEST(test_kv_invalid_keys_rejected);
    RUN_TEST(test_kv_oversize_value_rejected);
    RUN_TEST(test_kv_small_buffer_returns_full_untouched);
    RUN_TEST(test_kv_stale_tmp_removed_on_next_put);
    RUN_TEST(test_kv_empty_value_roundtrip);
    RUN_TEST(test_kv_get_crc_validates_after_overwrite);

    RUN_TEST(test_window_put_get_roundtrip);
    RUN_TEST(test_window_missing_seq_returns_not_found);
    RUN_TEST(test_window_index_rebuilt_across_reboot);
    RUN_TEST(test_window_count_limit_evicts_oldest);
    RUN_TEST(test_window_reput_same_seq_is_idempotent);
    RUN_TEST(test_window_torn_write_discarded_at_mount);
    RUN_TEST(test_window_corrupt_crc_returns_corrupt);
    RUN_TEST(test_window_small_buffer_returns_full);
    RUN_TEST(test_window_invalid_args_rejected);
    RUN_TEST(test_window_fs_full_evicts_then_faults);
    RUN_TEST(test_window_mount_read_failure_keeps_blob);

    RUN_TEST(test_store_first_append_assigns_seq_one);
    RUN_TEST(test_store_append_accepts_preassigned_seq);
    RUN_TEST(test_store_append_rejects_sequence_violation);
    RUN_TEST(test_store_read_empty_returns_zero);
    RUN_TEST(test_store_read_paging_inclusive_cursor);
    RUN_TEST(test_store_append_is_persisted_verbatim);
    RUN_TEST(test_store_seq_continuity_across_reboot);
    RUN_TEST(test_store_corrupt_record_skips_with_counter);
    RUN_TEST(test_store_torn_tail_truncated_at_mount);
    RUN_TEST(test_store_corrupt_segment_skipped_not_fatal);
    RUN_TEST(test_store_queue_full_refused_nothing_consumed);
    RUN_TEST(test_store_label_applied_in_read_and_persists);
    RUN_TEST(test_store_label_unknown_seq_and_bad_label_rejected);
    RUN_TEST(test_store_torn_label_patch_reverts_after_reboot);
    RUN_TEST(test_store_mark_synced_idempotent_and_persists);
    RUN_TEST(test_store_mark_synced_rejects_backwards_and_beyond);
    RUN_TEST(test_store_sync_cursor_conservative_after_power_cut);
    RUN_TEST(test_store_full_refuses_unsynced_then_evicts_synced);
    RUN_TEST(test_store_rotation_at_exact_segment_boundary);
    RUN_TEST(test_store_op_failures_reported_and_store_survives);
    RUN_TEST(test_store_health_false_before_pump_true_after);
    RUN_TEST(test_store_health_counters_accumulate);
    RUN_TEST(test_store_pump_idle_returns_zero_and_no_flash_ops);
    RUN_TEST(test_store_reserved_seq_survives_failed_pump_apply);
    RUN_TEST(test_segment_manager_state_within_budget);
    RUN_TEST(test_store_mount_unformatted_reports_and_counts);
    RUN_TEST(test_store_mount_read_failure_keeps_segment);
    RUN_TEST(test_store_queue_guard_wraps_entry_points);
    RUN_TEST(test_store_label_patch_wins_after_cache_eviction);
    RUN_TEST(test_store_inband_label_survives_read_without_patch);
    RUN_TEST(test_store_patch_cache_republish_never_pairs_new_seq_with_old_label);
    RUN_TEST(test_store_patch_for_validates_entry_across_a_concurrent_republish);

    RUN_TEST(test_doc_wire_storage_write_claim_is_true);
    RUN_TEST(test_doc_spec_numbers_match_code);
    RUN_TEST(test_doc_thread_model_lists_shared_state);
    RUN_TEST(test_doc_counters_header_names_format_recoveries);
    RUN_TEST(test_doc_partition_label_matches_csv);
    RUN_TEST(test_vfs_path_prefixes_bare_relative_names);
    RUN_TEST(test_vfs_path_is_idempotent_for_prefixed_input);
    RUN_TEST(test_vfs_path_cut_name_agrees_across_builders);
    RUN_TEST(test_vfs_path_bounds_and_bad_args_rejected);

    RUN_TEST(test_torn_write_runtime_repair_at_every_byte_offset);
    RUN_TEST(test_torn_write_power_cut_at_every_byte_offset);
    RUN_TEST(test_torn_write_rotation_header_at_every_byte_offset);
    RUN_TEST(test_torn_write_new_segment_first_record_at_every_byte_offset);
    RUN_TEST(test_torn_write_straddles_segment_end_at_every_byte_offset);
    RUN_TEST(test_torn_write_sync_marker_at_every_byte_offset);

    return UNITY_END();
}

void setUp(void) {}
void tearDown(void) {}
