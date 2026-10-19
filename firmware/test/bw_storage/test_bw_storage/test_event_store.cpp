// F18 §15.5 event-store tests: T1/T2 (seq + read), T4 (CRC skip), T5
// (full/eviction), T6 (labels), T7 (sync cursor), T8 (segment skip),
// T11 (op failures), T12 (health provider), T13 (empty), T14 (queue Full),
// T15 (idle pump) plus audit FIX tests F-01/F-02/F-03/F-04/F-06/F-07/F-10.
// DoD byte-offset sweeps live in test_torn_write.cpp.

#include <unity.h>

#include <bw/testkit/recording_fault_reporter.h>

#include <bw/storage/event_store.h>

#include "ram_fs_fake.h"

using bw::storage::LittleFsEventStore;
using bw::storage::StorageCounters;

namespace {

bw::Event makeEvent(uint32_t seq_hint, uint32_t tag = 0) {
    bw::Event e{};
    e.t_start_ms = 100000 + tag;
    e.seq = seq_hint;  // 0 = "store assigns"
    e.boot_id = 1;
    e.duration_ms = 500 + tag;
    e.confidence = 0.5f;
    e.reasons = 0x11;
    e.payload[0] = static_cast<uint8_t>(tag & 0xFF);
    e.type = bw::EventType::DrowsinessEvent;
    e.label = bw::UserLabel::Unlabeled;
    e.alertness_at_start = bw::AlertnessState::Awake;
    e.detector_id = 1;
    e.schema_version = bw::Event::kSchemaVersion;
    return e;
}

struct StoreFixture {
    RamFsFake fs;
    StorageCounters counters{};
    bw::testkit::RecordingFaultReporter<64> faults;
    LittleFsEventStore store{fs, counters};

    explicit StoreFixture(uint32_t capacity = 512u * 1024u) : fs(capacity) {
        // Mount BEFORE attaching the reporter: the blank-medium fallback
        // format (F-03) reports into a null reporter here on purpose — this
        // fixture's first boot is provisioning, and every test below counts
        // only the faults its own steps produce (its exact-count and >= 1
        // assertions stay honest). test_store_mount_unformatted_... attaches
        // BEFORE its own mount to verify the F-03 contract itself.
        TEST_ASSERT_TRUE(store.mount().ok());
        store.attachFaultReporter(&faults);
    }
};

struct ReadResult {
    size_t count;
    uint32_t first_seq;
    uint32_t last_seq;
};

ReadResult readAll(LittleFsEventStore& store, uint32_t from = 1) {
    ReadResult r{0, 0, 0};
    bw::Event buf[128];
    uint32_t cursor = from;
    for (;;) {
        const auto got = store.readFrom(cursor, buf, 128);
        TEST_ASSERT_TRUE(got.ok());
        if (got.value == 0) {
            break;
        }
        if (r.count == 0) {
            r.first_seq = buf[0].seq;
        }
        r.count += got.value;
        r.last_seq = buf[got.value - 1].seq;
        cursor = buf[got.value - 1].seq + 1;
        if (got.value < 128) {
            break;
        }
    }
    return r;
}

// F-07: deterministic contract spy — the fix under test is "each producer
// entry point takes the guard exactly once"; a reverted fix leaves enters at
// 0. No threads needed (RamFsFake is not thread-safe; see status log).
struct CountingGuard final : bw::storage::IQueueGuard {
    int enters = 0;
    int leaves = 0;
    void enter() override { ++enters; }
    void leave() override { ++leaves; }
};

}  // namespace

// --- T1/T13: happy path + empty state ----------------------------------------

void test_store_first_append_assigns_seq_one(void) {
    StoreFixture f;
    const auto r = f.store.append(makeEvent(0, 1));
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_UINT32(1, r.value);
}

void test_store_append_accepts_preassigned_seq(void) {
    StoreFixture f;
    const auto r1 = f.store.append(makeEvent(1, 1));  // F17 pre-assign style
    TEST_ASSERT_TRUE(r1.ok());
    TEST_ASSERT_EQUAL_UINT32(1, r1.value);
    const auto r2 = f.store.append(makeEvent(0, 2));  // store-assign style
    TEST_ASSERT_TRUE(r2.ok());
    TEST_ASSERT_EQUAL_UINT32(2, r2.value);
}

void test_store_append_rejects_sequence_violation(void) {
    StoreFixture f;
    const auto bad = f.store.append(makeEvent(2, 1));  // skipped seq 1
    TEST_ASSERT_FALSE(bad.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::Err::InvalidArg),
                            static_cast<uint8_t>(bad.err));
    TEST_ASSERT_TRUE(f.store.append(makeEvent(1, 1)).ok());
    const auto duplicate = f.store.append(makeEvent(1, 2));  // seq 1 again
    TEST_ASSERT_FALSE(duplicate.ok());
}

void test_store_read_empty_returns_zero(void) {
    StoreFixture f;
    bw::Event buf[4];
    const auto got = f.store.readFrom(1, buf, 4);
    TEST_ASSERT_TRUE(got.ok());
    TEST_ASSERT_EQUAL_UINT(0, got.value);
    bw::StorageHealth health{};
    TEST_ASSERT_FALSE(f.store.latest(health));  // T13: no snapshot before pump
}

void test_store_read_paging_inclusive_cursor(void) {
    StoreFixture f;
    for (uint32_t i = 1; i <= 5; ++i) {
        TEST_ASSERT_TRUE(f.store.append(makeEvent(0, i)).ok());
    }
    f.store.pump(1000);
    bw::Event buf[3];
    const auto page1 = f.store.readFrom(1, buf, 3);
    TEST_ASSERT_TRUE(page1.ok());
    TEST_ASSERT_EQUAL_UINT(3, page1.value);
    TEST_ASSERT_EQUAL_UINT32(1, buf[0].seq);
    TEST_ASSERT_EQUAL_UINT32(3, buf[2].seq);
    const auto page2 = f.store.readFrom(4, buf, 3);
    TEST_ASSERT_TRUE(page2.ok());
    TEST_ASSERT_EQUAL_UINT(2, page2.value);
    const auto mid = f.store.readFrom(3, buf, 3);  // inclusive from_seq
    TEST_ASSERT_TRUE(mid.ok());
    TEST_ASSERT_EQUAL_UINT(3, mid.value);  // events 3,4,5 all fit the page
    TEST_ASSERT_EQUAL_UINT32(3, buf[0].seq);
    const auto past = f.store.readFrom(99, buf, 3);
    TEST_ASSERT_TRUE(past.ok());
    TEST_ASSERT_EQUAL_UINT(0, past.value);
    const auto zero_max = f.store.readFrom(1, buf, 0);
    TEST_ASSERT_FALSE(zero_max.ok());  // invalid arg, not a silent no-op
}

void test_store_append_is_persisted_verbatim(void) {
    StoreFixture f;
    bw::Event in = makeEvent(0, 9);
    in.boot_id = 77;
    in.duration_ms = 4321;
    in.confidence = 0.875f;
    in.reasons = 0x00C0FFEE;
    in.type = bw::EventType::SuspectedCataplexy;
    TEST_ASSERT_TRUE(f.store.append(in).ok());
    f.store.pump(1000);
    bw::Event out[2];
    const auto got = f.store.readFrom(1, out, 2);
    TEST_ASSERT_TRUE(got.ok());
    TEST_ASSERT_EQUAL_UINT(1, got.value);
    TEST_ASSERT_EQUAL_UINT32(77, out[0].boot_id);
    TEST_ASSERT_EQUAL_UINT32(4321, out[0].duration_ms);
    TEST_ASSERT_EQUAL_FLOAT(0.875f, out[0].confidence);
    TEST_ASSERT_EQUAL_HEX32(0x00C0FFEE, out[0].reasons);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::EventType::SuspectedCataplexy),
                            static_cast<uint8_t>(out[0].type));
    TEST_ASSERT_EQUAL_UINT8(1, out[0].schema_version);
}

// --- T2: seq continuity across simulated reboots -----------------------------

void test_store_seq_continuity_across_reboot(void) {
    StoreFixture f;
    TEST_ASSERT_EQUAL_UINT32(1, f.store.append(makeEvent(0, 1)).value);
    TEST_ASSERT_EQUAL_UINT32(2, f.store.append(makeEvent(0, 2)).value);
    f.store.pump(1000);
    {
        LittleFsEventStore again{f.fs, f.counters};  // reboot 1
        TEST_ASSERT_TRUE(again.mount().ok());
        const auto r = again.append(makeEvent(0, 3));
        TEST_ASSERT_TRUE(r.ok());
        TEST_ASSERT_EQUAL_UINT32(3, r.value);
        again.pump(2000);
        LittleFsEventStore third{f.fs, f.counters};  // reboot 2
        TEST_ASSERT_TRUE(third.mount().ok());
        const auto r2 = third.append(makeEvent(0, 4));
        TEST_ASSERT_TRUE(r2.ok());
        TEST_ASSERT_EQUAL_UINT32(4, r2.value);
    }
}

// --- T4: CRC failure mid-segment ---------------------------------------------

void test_store_corrupt_record_skips_with_counter(void) {
    StoreFixture f;
    for (uint32_t i = 1; i <= 5; ++i) {
        TEST_ASSERT_TRUE(f.store.append(makeEvent(0, i)).ok());
    }
    f.store.pump(1000);
    // Byte inside record 3 (records start after the 16 B segment header).
    const size_t offset3 =
        bw::storage::kSegmentHeaderBytes + 2 * bw::storage::kEventRecordBytes + 10;
    f.fs.corruptByte("evt_000.bin", offset3);

    LittleFsEventStore again{f.fs, f.counters};  // reboot
    TEST_ASSERT_TRUE(again.mount().ok());
    TEST_ASSERT_TRUE(f.counters.corrupt_records >= 1);
    const ReadResult all = readAll(again);
    TEST_ASSERT_EQUAL_UINT(4, all.count);  // 1,2,4,5 — seq 3 skipped (D4/D8)
    TEST_ASSERT_EQUAL_UINT32(1, all.first_seq);
    TEST_ASSERT_EQUAL_UINT32(5, all.last_seq);
    const auto next = again.append(makeEvent(0, 6));
    TEST_ASSERT_TRUE(next.ok());
    TEST_ASSERT_EQUAL_UINT32(6, next.value);  // watermark intact
}

void test_store_torn_tail_truncated_at_mount(void) {
    StoreFixture f;
    for (uint32_t i = 1; i <= 3; ++i) {
        TEST_ASSERT_TRUE(f.store.append(makeEvent(0, i)).ok());
    }
    f.store.pump(1000);
    // Power cut mid-append: 7 garbage bytes past the last good record.
    std::vector<uint8_t>* seg = f.fs.file("evt_000.bin");
    TEST_ASSERT_NOT_NULL(seg);
    for (int i = 0; i < 7; ++i) {
        seg->push_back(static_cast<uint8_t>(0x5A + i));
    }
    const size_t dirty_size = seg->size();

    LittleFsEventStore again{f.fs, f.counters};  // reboot
    TEST_ASSERT_TRUE(again.mount().ok());
    TEST_ASSERT_EQUAL_UINT32(1, f.counters.torn_tails_repaired);
    std::vector<uint8_t>* clean = f.fs.file("evt_000.bin");
    TEST_ASSERT_NOT_NULL(clean);
    TEST_ASSERT_EQUAL_UINT(dirty_size - 7, clean->size());  // truncated to last record
    const ReadResult all = readAll(again);
    TEST_ASSERT_EQUAL_UINT(3, all.count);
}

// --- T8: whole corrupt segment skipped ---------------------------------------

void test_store_corrupt_segment_skipped_not_fatal(void) {
    StoreFixture f;
    // 273 events: 272 fill segment 0, #273 rotates into segment 1.
    for (uint32_t i = 1; i <= 273; ++i) {
        TEST_ASSERT_TRUE(f.store.append(makeEvent(0, i)).ok());
        f.store.pump(1000);
    }
    TEST_ASSERT_EQUAL_UINT(2, f.store.segments().segmentCount());
    // Smash segment 0's header: the whole segment becomes unidentifiable.
    const std::vector<uint8_t> garbage(100, 0xEE);
    f.fs.setBytes("evt_000.bin", garbage);

    LittleFsEventStore again{f.fs, f.counters};  // reboot
    TEST_ASSERT_TRUE(again.mount().ok());
    TEST_ASSERT_EQUAL_UINT32(1, f.counters.segments_skipped);
    const ReadResult all = readAll(again);
    TEST_ASSERT_EQUAL_UINT(1, all.count);  // only the surviving segment's event
    TEST_ASSERT_EQUAL_UINT32(273, all.first_seq);
    const auto next = again.append(makeEvent(0, 274));
    TEST_ASSERT_TRUE(next.ok());
    TEST_ASSERT_EQUAL_UINT32(274, next.value);
}

// --- T14: queue capacity -----------------------------------------------------

void test_store_queue_full_refused_nothing_consumed(void) {
    StoreFixture f;
    const size_t depth = bw::storage::kStorageQueueDepth;
    for (size_t i = 0; i < depth; ++i) {
        TEST_ASSERT_TRUE(f.store.append(makeEvent(0, static_cast<uint32_t>(i + 1))).ok());
    }
    const auto overflow = f.store.append(makeEvent(0, 99));
    TEST_ASSERT_FALSE(overflow.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::Err::Full),
                            static_cast<uint8_t>(overflow.err));
    TEST_ASSERT_EQUAL_UINT32(1, f.store.queueDrops());  // refused push counted
    f.store.pump(1000);                                 // applies the 16 queued
    const ReadResult all = readAll(f.store);
    TEST_ASSERT_EQUAL_UINT(depth, all.count);  // the refused one never landed
    const auto retry = f.store.append(makeEvent(0, 99));
    TEST_ASSERT_TRUE(retry.ok());  // CR-004: retry after freeing succeeds
    TEST_ASSERT_EQUAL_UINT32(depth + 1, retry.value);
}

// --- T6: labels --------------------------------------------------------------

void test_store_label_applied_in_read_and_persists(void) {
    StoreFixture f;
    for (uint32_t i = 1; i <= 3; ++i) {
        TEST_ASSERT_TRUE(f.store.append(makeEvent(0, i)).ok());
    }
    f.store.pump(1000);
    TEST_ASSERT_TRUE(f.store.setLabel(2, bw::UserLabel::Confirmed).ok());
    f.store.pump(2000);
    bw::Event out[3];
    TEST_ASSERT_EQUAL_UINT(3, f.store.readFrom(1, out, 3).value);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::UserLabel::Confirmed),
                            static_cast<uint8_t>(out[1].label));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::UserLabel::Unlabeled),
                            static_cast<uint8_t>(out[0].label));

    LittleFsEventStore again{f.fs, f.counters};  // reboot
    TEST_ASSERT_TRUE(again.mount().ok());
    bw::Event after[3];
    TEST_ASSERT_EQUAL_UINT(3, again.readFrom(1, after, 3).value);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::UserLabel::Confirmed),
                            static_cast<uint8_t>(after[1].label));
}

void test_store_label_unknown_seq_and_bad_label_rejected(void) {
    StoreFixture f;
    TEST_ASSERT_TRUE(f.store.append(makeEvent(0, 1)).ok());
    f.store.pump(1000);
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(bw::Err::NotFound),
        static_cast<uint8_t>(f.store.setLabel(99, bw::UserLabel::Confirmed).err));
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(bw::Err::NotFound),
        static_cast<uint8_t>(f.store.setLabel(0, bw::UserLabel::Confirmed).err));
    const auto bad = f.store.setLabel(1, static_cast<bw::UserLabel>(99));
    TEST_ASSERT_FALSE(bad.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::Err::InvalidArg),
                            static_cast<uint8_t>(bad.err));
}

void test_store_torn_label_patch_reverts_after_reboot(void) {
    StoreFixture f;
    TEST_ASSERT_TRUE(f.store.append(makeEvent(0, 1)).ok());
    f.store.pump(1000);
    f.fs.armTornWrite(6);  // tear the 17 B LabelPatch write
    TEST_ASSERT_TRUE(f.store.setLabel(1, bw::UserLabel::Rejected).ok());
    f.store.pump(2000);  // patch write fails -> reported, not silent
    TEST_ASSERT_TRUE(f.faults.countOf(bw::FaultCode::StorageDegraded) >= 1);

    LittleFsEventStore again{f.fs, f.counters};  // reboot
    TEST_ASSERT_TRUE(again.mount().ok());
    bw::Event out[2];
    TEST_ASSERT_EQUAL_UINT(1, again.readFrom(1, out, 2).value);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::UserLabel::Unlabeled),
                            static_cast<uint8_t>(out[0].label));  // honest revert
}

// --- T7: sync cursor ---------------------------------------------------------

void test_store_mark_synced_idempotent_and_persists(void) {
    StoreFixture f;
    for (uint32_t i = 1; i <= 5; ++i) {
        TEST_ASSERT_TRUE(f.store.append(makeEvent(0, i)).ok());
    }
    f.store.pump(1000);
    TEST_ASSERT_TRUE(f.store.markSyncedThrough(3).ok());
    TEST_ASSERT_EQUAL_UINT32(3, f.store.lastSyncedSeq());
    TEST_ASSERT_TRUE(f.store.markSyncedThrough(3).ok());  // idempotent re-mark
    TEST_ASSERT_EQUAL_UINT32(3, f.store.lastSyncedSeq());
    f.store.pump(0);  // first checkpoint: due immediately (D5)

    LittleFsEventStore again{f.fs, f.counters};  // reboot
    TEST_ASSERT_TRUE(again.mount().ok());
    TEST_ASSERT_EQUAL_UINT32(3, again.lastSyncedSeq());
}

void test_store_mark_synced_rejects_backwards_and_beyond(void) {
    StoreFixture f;
    for (uint32_t i = 1; i <= 5; ++i) {
        TEST_ASSERT_TRUE(f.store.append(makeEvent(0, i)).ok());
    }
    f.store.pump(1000);
    TEST_ASSERT_TRUE(f.store.markSyncedThrough(3).ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::Err::InvalidArg),
                            static_cast<uint8_t>(f.store.markSyncedThrough(2).err));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::Err::InvalidArg),
                            static_cast<uint8_t>(f.store.markSyncedThrough(99).err));
    TEST_ASSERT_EQUAL_UINT32(3, f.store.lastSyncedSeq());
}

void test_store_sync_cursor_conservative_after_power_cut(void) {
    StoreFixture f;
    for (uint32_t i = 1; i <= 5; ++i) {
        TEST_ASSERT_TRUE(f.store.append(makeEvent(0, i)).ok());
    }
    f.store.pump(1000);
    TEST_ASSERT_TRUE(f.store.markSyncedThrough(5).ok());
    f.store.pump(1000);  // checkpoint flushed (first one is due)
    for (uint32_t i = 6; i <= 8; ++i) {
        TEST_ASSERT_TRUE(f.store.append(makeEvent(0, i)).ok());
    }
    f.store.pump(1500);
    TEST_ASSERT_TRUE(f.store.markSyncedThrough(8).ok());
    f.store.pump(1600);  // within kSyncCheckpointIntervalMs: marker NOT flushed
    TEST_ASSERT_EQUAL_UINT32(8, f.store.lastSyncedSeq());  // RAM cursor advanced

    LittleFsEventStore again{f.fs, f.counters};  // power cut here
    TEST_ASSERT_TRUE(again.mount().ok());
    // Conservative: the last PERSISTED marker (5) wins — F20 re-syncs the
    // tail idempotently (D5, DEC-011). Never a cursor past stored data.
    TEST_ASSERT_EQUAL_UINT32(5, again.lastSyncedSeq());
}

// --- T5: full flash + eviction order ----------------------------------------

namespace {

size_t countAll(LittleFsEventStore& store) {
    return readAll(store).count;
}

}  // namespace

void test_store_full_refuses_unsynced_then_evicts_synced(void) {
    StoreFixture f;
    // Fill all 16 segment slots: 16 x 272 = 4352 events, none synced.
    const uint32_t kFill = bw::storage::kMaxSegments * 272;
    for (uint32_t i = 1; i <= kFill; ++i) {
        TEST_ASSERT_TRUE(f.store.append(makeEvent(0, i)).ok());
        f.store.pump(1000);
    }
    TEST_ASSERT_EQUAL_UINT(bw::storage::kMaxSegments, f.store.segments().segmentCount());

    // Everything unsynced: append is REFUSED, nothing consumed (§5/§10).
    const auto full = f.store.append(makeEvent(0, 999));
    TEST_ASSERT_FALSE(full.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::Err::Full), static_cast<uint8_t>(full.err));
    // F-06: the refusal is also REPORTED — without it only the caller's
    // Result shows it, and a caller that drops Results silences storage
    // exhaustion from F23 entirely. (Fixture drops its mount-time report, so
    // this count is purely the refusal's own fault.)
    TEST_ASSERT_TRUE(f.faults.countOf(bw::FaultCode::StorageDegraded) >= 1);
    TEST_ASSERT_EQUAL_UINT(kFill, countAll(f.store));  // no silent data loss

    // Sync the old data: now rotation may evict the OLDEST synced segment.
    TEST_ASSERT_TRUE(f.store.markSyncedThrough(kFill - 400).ok());
    TEST_ASSERT_TRUE(f.store.append(makeEvent(0, 999)).ok());
    f.store.pump(1000);
    TEST_ASSERT_EQUAL_UINT32(1, f.counters.evicted_synced_segments);
    const ReadResult all = readAll(f.store);
    TEST_ASSERT_EQUAL_UINT32(273, all.first_seq);        // segment 0 gone
    TEST_ASSERT_EQUAL_UINT(kFill - 272 + 1, all.count);  // 273..4353
}

// --- segment rotation boundary ------------------------------------------------

void test_store_rotation_at_exact_segment_boundary(void) {
    StoreFixture f;
    // 272 records x 60 B + 16 B header = 16336 <= 16384; #273 does not fit.
    // Pump per append: the op queue holds only kStorageQueueDepth entries.
    for (uint32_t i = 1; i <= 272; ++i) {
        TEST_ASSERT_TRUE(f.store.append(makeEvent(0, i)).ok());
        f.store.pump(1000);
    }
    TEST_ASSERT_EQUAL_UINT(1, f.store.segments().segmentCount());
    TEST_ASSERT_EQUAL_UINT32(16336, f.store.segments().currentUsedBytes());

    TEST_ASSERT_TRUE(f.store.append(makeEvent(0, 273)).ok());
    f.store.pump(1000);
    TEST_ASSERT_EQUAL_UINT(2, f.store.segments().segmentCount());
    const ReadResult all = readAll(f.store);
    TEST_ASSERT_EQUAL_UINT(273, all.count);
    TEST_ASSERT_EQUAL_UINT32(273, all.last_seq);
}

// --- T11: op-level fault injection -------------------------------------------

void test_store_op_failures_reported_and_store_survives(void) {
    StoreFixture f;
    TEST_ASSERT_TRUE(f.store.append(makeEvent(0, 1)).ok());
    f.store.pump(1000);
    // Every fs op fails while we drain one append: the op is consumed and a
    // fault is reported — never a crash, never a silent success.
    f.fs.failOpsEveryN(1);
    TEST_ASSERT_TRUE(f.store.append(makeEvent(0, 2)).ok());
    f.store.pump(2000);
    TEST_ASSERT_TRUE(f.faults.countOf(bw::FaultCode::StorageDegraded) >= 1);
    f.fs.failOpsEveryN(0);  // storage recovers
    TEST_ASSERT_TRUE(f.store.append(makeEvent(0, 3)).ok());
    f.store.pump(3000);
    const ReadResult all = readAll(f.store);
    TEST_ASSERT_TRUE(all.count >= 1);  // first event never lost
    TEST_ASSERT_EQUAL_UINT32(1, all.first_seq);
}

// --- T12: health provider (CR-015) -------------------------------------------

void test_store_health_false_before_pump_true_after(void) {
    StoreFixture f;
    bw::StorageHealth health{};
    TEST_ASSERT_FALSE(f.store.latest(health));
    TEST_ASSERT_TRUE(f.store.append(makeEvent(0, 1)).ok());
    f.store.pump(1000);
    TEST_ASSERT_TRUE(f.store.latest(health));
    TEST_ASSERT_EQUAL_UINT64(1000, health.t_ms);
    const uint32_t free_bytes = f.fs.freeBytes().value;
    TEST_ASSERT_EQUAL_UINT32(free_bytes, health.free_bytes);
    TEST_ASSERT_EQUAL_UINT32(f.fs.totalBytes().value, health.total_bytes);
}

void test_store_health_counters_accumulate(void) {
    StoreFixture f;
    for (uint32_t i = 1; i <= 3; ++i) {
        TEST_ASSERT_TRUE(f.store.append(makeEvent(0, i)).ok());
    }
    f.store.pump(1000);
    f.fs.corruptByte("evt_000.bin", bw::storage::kSegmentHeaderBytes + 70);
    LittleFsEventStore again{f.fs, f.counters};
    TEST_ASSERT_TRUE(again.mount().ok());
    TEST_ASSERT_TRUE(again.append(makeEvent(0, 4)).ok());
    again.pump(5000);
    bw::StorageHealth health{};
    TEST_ASSERT_TRUE(again.latest(health));
    // A hole with valid data after it (D4 forward resync) is corrupt_records,
    // NOT a torn tail: nothing was truncated, so the repair counter stays 0.
    TEST_ASSERT_TRUE(health.corrupt_records >= 1);
    TEST_ASSERT_EQUAL_UINT32(0, health.torn_tails_repaired);
    TEST_ASSERT_EQUAL_UINT32(f.counters.kv_discards, health.kv_discards);
}

// --- T15: idle pump does no IO ------------------------------------------------

void test_store_pump_idle_returns_zero_and_no_flash_ops(void) {
    StoreFixture f;
    const size_t ops_before = f.fs.opCount();
    const bw::TimeMs t = f.store.pump(1234);
    TEST_ASSERT_EQUAL_UINT(0, static_cast<unsigned>(t));
    TEST_ASSERT_EQUAL_UINT(ops_before, f.fs.opCount());  // no reads, no writes
}

// --- audit F-01: the seq watermark is reserve-only ----------------------------

void test_store_reserved_seq_survives_failed_pump_apply(void) {
    StoreFixture f;
    // Three appends RESERVE 1,2,3 at ENQUEUE (D3): next_seq_ == 4 before any
    // of them reaches flash. A later failed apply must not rewind it.
    TEST_ASSERT_EQUAL_UINT32(1, f.store.append(makeEvent(0, 1)).value);
    TEST_ASSERT_EQUAL_UINT32(2, f.store.append(makeEvent(0, 2)).value);
    TEST_ASSERT_EQUAL_UINT32(3, f.store.append(makeEvent(0, 3)).value);
    // Writes during this pump: segment header, rec1, rec2, rec3 — skip the
    // first three and tear rec3: 1,2 persist, seq 3 burns its reserved value.
    f.fs.armTornWrite(10, 3);
    TEST_ASSERT_EQUAL_UINT(3, f.store.pump(1000));  // 3 ops consumed, 1 failed
    // Revert check (F-01 deleted `next_seq_ = event.seq + 1` in appendEvent):
    // the pump's last SUCCESSFUL apply was seq 2, so a rewinding watermark
    // would hand seq 3 out again — reserved-once seqs are never reused.
    const auto r4 = f.store.append(makeEvent(0, 4));
    TEST_ASSERT_TRUE(r4.ok());
    TEST_ASSERT_EQUAL_UINT32(4, r4.value);  // F-01 reverted: returns 3
    f.store.pump(2000);
    const ReadResult all = readAll(f.store);
    TEST_ASSERT_EQUAL_UINT(3, all.count);  // 1,2,4 — seq 3 is a counted gap (D3)
    TEST_ASSERT_EQUAL_UINT32(4, all.last_seq);
}

// --- audit F-02: decode scratch lives on the stack, never on the object --------

void test_segment_manager_state_within_budget(void) {
    // Structural gate: re-adding the 76 B record + 204 B resync scratch as
    // members (pre-F-02 state, ~800 B) busts this documented budget — the
    // shared-buffer race against a concurrent readFrom would be back.
    TEST_ASSERT_TRUE(sizeof(bw::storage::SegmentManager) <=
                     bw::storage::kSegmentManagerStateBudgetBytes);
    // F-09 pins: Op = 64 B, so the op queue is 16 x 64 B = 1024 B (spec §15).
    TEST_ASSERT_EQUAL_UINT(64, sizeof(LittleFsEventStore::Op));
}

// --- audit F-03: a fallback format is reported, never silent ------------------

void test_store_mount_unformatted_reports_and_counts(void) {
    StoreFixture f;
    for (uint32_t i = 1; i <= 3; ++i) {
        TEST_ASSERT_TRUE(f.store.append(makeEvent(0, i)).ok());
    }
    f.store.pump(1000);
    TEST_ASSERT_EQUAL_UINT(3, readAll(f.store).count);  // data really present
    const uint32_t formats_before = f.counters.format_recoveries;

    f.fs.forceUnformatted();  // medium unreadable, FILES STILL PRESENT
    LittleFsEventStore again{f.fs, f.counters};
    bw::testkit::RecordingFaultReporter<8> mount_faults;
    again.attachFaultReporter(&mount_faults);  // BEFORE mount, as wire() does
    TEST_ASSERT_TRUE(again.mount().ok());      // read-only fails -> report -> format
    TEST_ASSERT_TRUE(mount_faults.countOf(bw::FaultCode::StorageDegraded) >= 1);
    TEST_ASSERT_EQUAL_UINT32(formats_before + 1, f.counters.format_recoveries);
    TEST_ASSERT_EQUAL_UINT(0, f.fs.fileCount());      // the wipe is real
    TEST_ASSERT_EQUAL_UINT(0, readAll(again).count);  // honest empty, no ghosts
}

// --- audit F-04: a failed READ never deletes ----------------------------------

void test_store_mount_read_failure_keeps_segment(void) {
    StoreFixture f;
    for (uint32_t i = 1; i <= 5; ++i) {
        TEST_ASSERT_TRUE(f.store.append(makeEvent(0, i)).ok());
        f.store.pump(1000);
    }
    const uint32_t skipped_before = f.counters.segments_skipped;

    f.fs.failNextReadFor("evt_000.bin");  // transient IO on the pass-1 probe
    LittleFsEventStore again{f.fs, f.counters};
    TEST_ASSERT_TRUE(again.mount().ok());          // §10: boot never bricks
    TEST_ASSERT_TRUE(f.fs.exists("evt_000.bin"));  // F-04 reverted: deleted!
    TEST_ASSERT_EQUAL_UINT32(skipped_before + 1, f.counters.segments_skipped);
    TEST_ASSERT_EQUAL_UINT(0, readAll(again).count);  // not indexed this boot

    // Clean remount: the skipped segment is served in full — nothing lost.
    LittleFsEventStore third{f.fs, f.counters};
    TEST_ASSERT_TRUE(third.mount().ok());
    const ReadResult all = readAll(third);
    TEST_ASSERT_EQUAL_UINT(5, all.count);
    TEST_ASSERT_EQUAL_UINT32(5, all.last_seq);
}

// --- audit F-07: producer guard wraps the three entry points -------------------

void test_store_queue_guard_wraps_entry_points(void) {
    StoreFixture f;
    CountingGuard guard;
    f.store.attachQueueGuard(&guard);

    TEST_ASSERT_TRUE(f.store.append(makeEvent(0, 1)).ok());  // enter #1
    f.store.pump(1000);  // single consumer: pump must NOT take the guard
    TEST_ASSERT_TRUE(f.store.setLabel(1, bw::UserLabel::Confirmed).ok());  // #2
    TEST_ASSERT_TRUE(f.store.markSyncedThrough(1).ok());                   // #3
    f.store.pump(2000);  // marker flush: still no guard near flash (D3)

    // F-07 reverted: enters stays 0 (the interface survives a call-site
    // revert, so this stays a runtime failure, not a compile error).
    TEST_ASSERT_EQUAL_INT(3, guard.enters);
    TEST_ASSERT_EQUAL_INT(3, guard.leaves);
    const ReadResult all = readAll(f.store);
    TEST_ASSERT_EQUAL_UINT(1, all.count);  // guarded flow works end to end
}

// --- audit F-10: label patches — miss keeps in-band, evicted patch still wins --

void test_store_label_patch_wins_after_cache_eviction(void) {
    StoreFixture f;
    // 20 events then 20 patches: the 16-entry patch cache evicts seq 1..4,
    // so their labels can only come from the disk records seen in the walk.
    for (uint32_t i = 1; i <= 20; ++i) {
        TEST_ASSERT_TRUE(f.store.append(makeEvent(0, i)).ok());
        f.store.pump(1000);
    }
    for (uint32_t i = 1; i <= 20; ++i) {
        const bw::UserLabel want =
            (i % 2 == 1) ? bw::UserLabel::Confirmed : bw::UserLabel::Rejected;
        TEST_ASSERT_TRUE(f.store.setLabel(i, want).ok());
        f.store.pump(1000);
    }
    bw::Event out[24];
    // Headroom > 20 events: the walk must REACH the tail patch records to
    // apply them (it stops at `max` events otherwise). A consumer paging with
    // exact max gets the cache answer; patches beyond both cache and walk are
    // the documented 16-entry bound (status log).
    const auto got = f.store.readFrom(1, out, 24);
    TEST_ASSERT_EQUAL_UINT(20, got.value);
    for (uint32_t i = 1; i <= 20; ++i) {
        const bw::UserLabel want =
            (i % 2 == 1) ? bw::UserLabel::Confirmed : bw::UserLabel::Rejected;
        TEST_ASSERT_EQUAL_UINT32(i, out[i - 1].seq);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(
            static_cast<uint8_t>(want), static_cast<uint8_t>(out[i - 1].label),
            "labels correct incl. cache-evicted seqs (F-10: walk applies disk patches)");
    }
}

void test_store_inband_label_survives_read_without_patch(void) {
    StoreFixture f;
    bw::Event in = makeEvent(0, 1);
    in.label = bw::UserLabel::Confirmed;  // producer persisted a label IN-BAND
    TEST_ASSERT_TRUE(f.store.append(in).ok());
    f.store.pump(1000);
    bw::Event out[2];
    TEST_ASSERT_EQUAL_UINT(1, f.store.readFrom(1, out, 2).value);
    // F-10 reverted: labelFor() returned Unlabeled on a cache miss and the
    // visitor clobbered the record's own label with it.
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::UserLabel::Confirmed),
                            static_cast<uint8_t>(out[0].label));

    // A later tail patch overrides the in-band label (latest wins, D4)...
    TEST_ASSERT_TRUE(f.store.setLabel(1, bw::UserLabel::Rejected).ok());
    f.store.pump(2000);
    TEST_ASSERT_EQUAL_UINT(1, f.store.readFrom(1, out, 2).value);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::UserLabel::Rejected),
                            static_cast<uint8_t>(out[0].label));

    // ...including a clear back to Unlabeled — verified from disk after a
    // simulated reboot (both patches are on disk; the walk applies them in
    // write order, so the last one wins even for evicted cache entries).
    TEST_ASSERT_TRUE(f.store.setLabel(1, bw::UserLabel::Unlabeled).ok());
    f.store.pump(3000);
    LittleFsEventStore again{f.fs, f.counters};
    TEST_ASSERT_TRUE(again.mount().ok());
    bw::Event after[2];
    TEST_ASSERT_EQUAL_UINT(1, again.readFrom(1, after, 2).value);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::UserLabel::Unlabeled),
                            static_cast<uint8_t>(after[0].label));
}

// --- audit NEW-M1: patch-cache pairs are published atomically ----------------

namespace {

// One-shot hook context for the NEW-M1 tests: the hook fires exactly when a
// test drives the manager into the seam window and then clears its target, so
// a stale hook left behind by an early test failure can never dereference a
// dead manager (it just no-ops).
bw::storage::SegmentManager* g_newm1_mgr = nullptr;

// Write-side seam: runs inside cachePatch's republish window and asks the
// question the concurrent reader asks — "what does patchFor(S_new) see NOW?"
bool g_newm1_write_hit = false;
bw::UserLabel g_newm1_write_seen = bw::UserLabel::Unlabeled;
uint32_t g_newm1_write_query = 0;

void newm1WriteWindowHook() {
    if (g_newm1_mgr == nullptr) {
        return;
    }
    bw::UserLabel out{};
    g_newm1_write_hit = g_newm1_mgr->patchFor(g_newm1_write_query, out);
    g_newm1_write_seen = out;
    g_newm1_mgr = nullptr;
}

// Read-side seam: runs inside patchFor between its seq and label loads and
// plays the producer — it republishes the entry patchFor just matched (the
// ring evicts that slot) before patchFor reads its label.
uint32_t g_newm1_read_evict_seq = 0;
bw::UserLabel g_newm1_read_evict_label = bw::UserLabel::Unlabeled;

void newm1ReadWindowHook() {
    if (g_newm1_mgr == nullptr) {
        return;
    }
    g_newm1_mgr->notePatch(g_newm1_read_evict_seq, g_newm1_read_evict_label);
    g_newm1_mgr = nullptr;
}

}  // namespace

void test_store_patch_cache_republish_never_pairs_new_seq_with_old_label(void) {
    // Audit NEW-M1 (write side): cachePatch republishes a ring slot with the
    // invalidate/commit protocol — seq killed first with kInvalidPatchSeq,
    // new seq written last — so a reader inside the publish window sees an
    // INVALID slot and misses. A plain {seq, label} overwrite let it pair the
    // new seq with the previous entry's label (the reported bug).
    RamFsFake fs{4096};
    StorageCounters counters{};
    bw::storage::SegmentManager mgr{fs, counters};
    for (uint32_t s = 1; s <= 16; ++s) {  // fill the 16-entry ring
        mgr.notePatch(s, bw::UserLabel::Confirmed);
    }
    g_newm1_mgr = &mgr;
    g_newm1_write_query = 17;
    mgr.setPatchWriteHookForTest(newm1WriteWindowHook);

    mgr.notePatch(17, bw::UserLabel::Rejected);  // ring full -> republish slot 0

    mgr.setPatchWriteHookForTest(nullptr);
    // Fixed: the reader in the window saw kInvalidPatchSeq → miss.
    // Reverted writer (new seq committed first): sees {17, Confirmed} → a
    // hit pairing seq 17 with slot 0's previous label → assertion fails.
    TEST_ASSERT_FALSE_MESSAGE(g_newm1_write_hit,
                              "reader inside the republish window must miss (NEW-M1)");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(static_cast<uint8_t>(bw::UserLabel::Confirmed),
                                  static_cast<uint8_t>(g_newm1_write_seen),
                                  "never the previous entry's label for the new seq (NEW-M1)");

    // The completed republish is visible and correct afterwards.
    bw::UserLabel out{};
    TEST_ASSERT_TRUE(mgr.patchFor(17, out));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::UserLabel::Rejected),
                            static_cast<uint8_t>(out));
    g_newm1_mgr = nullptr;
}

void test_store_patch_for_validates_entry_across_a_concurrent_republish(void) {
    // Audit NEW-M1 (read side): patchFor loads seq, then label, then seq
    // again; if the entry was republished between the loads, the label may
    // belong to a different seq — the re-validate turns that torn view into
    // a miss instead of a wrong pairing.
    RamFsFake fs{4096};
    StorageCounters counters{};
    bw::storage::SegmentManager mgr{fs, counters};
    for (uint32_t s = 1; s <= 16; ++s) {
        mgr.notePatch(s, bw::UserLabel::Confirmed);
    }
    // The hook fires after patchFor matched slot 0 (seq 1) and republishes
    // that slot with seq 17 before patchFor reads its label: the label load
    // then returns Rejected — a label never written for seq 1.
    g_newm1_mgr = &mgr;
    g_newm1_read_evict_seq = 17;
    g_newm1_read_evict_label = bw::UserLabel::Rejected;
    mgr.setPatchReadHookForTest(newm1ReadWindowHook);

    bw::UserLabel out{};
    const bool hit = mgr.patchFor(1, out);

    mgr.setPatchReadHookForTest(nullptr);
    // Reverted (no seq re-validate): hit == true with Rejected for seq 1.
    TEST_ASSERT_FALSE_MESSAGE(
        hit, "a mid-scan republish must turn patchFor into a miss, not a wrong label (NEW-M1)");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(
        static_cast<uint8_t>(bw::UserLabel::Unlabeled), static_cast<uint8_t>(out),
        "out must stay untouched on a miss (F-10/NEW-M1: keep the in-band label)");
    // The republished entry answers correctly after the fact.
    g_newm1_mgr = nullptr;
    TEST_ASSERT_TRUE(mgr.patchFor(17, out));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(bw::UserLabel::Rejected),
                            static_cast<uint8_t>(out));
}
