// F18 §15.5 T3 — byte-offset torn-write fault-injection sweeps (DoD §14).
//
// Every write() call in RamFsFake is one atomic flash programming step
// (ram_fs_fake.h); these sweeps cut an append at EVERY byte offset k and
// assert the recovery contract after a simulated reboot (a fresh store over
// the same medium — RAM state gone, bytes kept):
//   * prefix property: records fully committed before the cut survive, the
//     torn one never appears as data (D2/D4),
//   * torn tails are truncated and counted; the next append lands on a clean
//     boundary, records never straddle (D4),
//   * seq never regresses: a failed append leaves a counted gap, reads skip
//     it and never fabricate a substitute (D3/D8),
//   * cuts across the segment-rotation boundary skip/remove the partial
//     header and the store stays usable (§10 never brick).
// k == kEventRecordBytes / kSegmentHeaderBytes / kSyncMarkerRecordBytes is
// the no-tear control: the write fully reached the medium and MUST be
// readable (and counted as such) after the reboot.

#include <unity.h>

#include <cstdio>

#include <bw/testkit/recording_fault_reporter.h>

#include <bw/storage/event_store.h>
#include <bw/storage/record_codec.h>

#include "ram_fs_fake.h"

using bw::storage::encodeRecord;
using bw::storage::kEventRecordBytes;
using bw::storage::kSegmentHeaderBytes;
using bw::storage::kSegmentSizeBytes;
using bw::storage::kSyncMarkerRecordBytes;
using bw::storage::LittleFsEventStore;
using bw::storage::RecordType;
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

struct Fixture {
    RamFsFake fs;
    StorageCounters counters{};
    bw::testkit::RecordingFaultReporter<64> faults;
    LittleFsEventStore store{fs, counters};

    explicit Fixture(uint32_t capacity = 512u * 1024u) : fs(capacity) {
        // Mount BEFORE attaching the reporter (F-03): this fixture's blank
        // first boot is provisioning — its fallback format reports into a
        // null reporter so the exact fault-count assertions below (line ~266)
        // keep measuring only the sweeps' own failures. The F-03 contract
        // itself is tested in test_event_store.cpp with attach-before-mount.
        TEST_ASSERT_TRUE(store.mount().ok());
        store.attachFaultReporter(&faults);
    }
};

struct Rebooted {
    StorageCounters counters{};
    bw::testkit::RecordingFaultReporter<64> faults;
    LittleFsEventStore store;

    // Member-init order matters: counters exists before store binds to it.
    explicit Rebooted(RamFsFake& fs) : store{fs, counters} {
        store.attachFaultReporter(&faults);
        TEST_ASSERT_TRUE(store.mount().ok());
    }
};

// "Reboot": a fresh store over the same medium. RAM state is gone, flash
// content survives — this is the boot-recovery path under test.
Rebooted reboot(RamFsFake& fs) {
    return Rebooted{fs};
}

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

// Append `count` events, pumping per append (the op queue holds only
// kStorageQueueDepth entries).
void fillEvents(LittleFsEventStore& store, uint32_t count, uint32_t first_seq = 1) {
    for (uint32_t i = 0; i < count; ++i) {
        TEST_ASSERT_TRUE(store.append(makeEvent(0, first_seq + i)).ok());
        store.pump(1000);
    }
}

void putU32LE(uint8_t* out, uint32_t v) {
    out[0] = static_cast<uint8_t>(v & 0xFF);
    out[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

// Power cut at the tail of `path`: exactly `nbytes` bytes of an encoded
// record for `e` (seq set exactly as the store would have assigned it)
// reached the medium, then the device died — no store code ran afterwards.
void injectRecordTail(RamFsFake& fs, const char* path, size_t tail_off, const bw::Event& e,
                      size_t nbytes) {
    uint8_t rec[kEventRecordBytes];
    const size_t total =
        encodeRecord(RecordType::Event, reinterpret_cast<const uint8_t*>(&e), bw::kEventBytes, rec);
    TEST_ASSERT_EQUAL_UINT(kEventRecordBytes, total);
    std::vector<uint8_t>* file = fs.file(path);
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_UINT(tail_off, file->size());  // injection lands at the segment tail
    file->insert(file->end(), rec, rec + nbytes);
}

void injectMarkerTail(RamFsFake& fs, const char* path, size_t tail_off, uint32_t seq,
                      size_t nbytes) {
    uint8_t payload[4];
    putU32LE(payload, seq);
    uint8_t rec[kSyncMarkerRecordBytes];
    const size_t total = encodeRecord(RecordType::SyncMarker, payload, sizeof(payload), rec);
    TEST_ASSERT_EQUAL_UINT(kSyncMarkerRecordBytes, total);
    std::vector<uint8_t>* file = fs.file(path);
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_UINT(tail_off, file->size());
    file->insert(file->end(), rec, rec + nbytes);
}

}  // namespace

// --- runtime repair sweep: the pump's own write is torn ---------------------

void test_torn_write_runtime_repair_at_every_byte_offset(void) {
    const uint32_t used_after_two = kSegmentHeaderBytes + 2 * kEventRecordBytes;
    for (size_t k = 0; k < kEventRecordBytes; ++k) {
        char msg[64];
        std::snprintf(msg, sizeof(msg), "runtime k=%u", static_cast<unsigned>(k));

        Fixture f;
        fillEvents(f.store, 2);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(used_after_two, f.store.segments().currentUsedBytes(),
                                         msg);

        f.fs.armTornWrite(static_cast<int64_t>(k));
        TEST_ASSERT_TRUE_MESSAGE(f.store.append(makeEvent(0, 3)).ok(), msg);
        // The op is consumed (D3): pump reports the failure as a fault rather
        // than hiding it, so applied == 1 even though the event was lost.
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1, f.store.pump(2000), msg);
        TEST_ASSERT_TRUE_MESSAGE(f.faults.countOf(bw::FaultCode::StorageDegraded) >= 1, msg);

        // Real repairs only: k == 0 committed no byte, so nothing was torn.
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(k > 0 ? 1u : 0u, f.counters.torn_tails_repaired, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(used_after_two, f.store.segments().currentUsedBytes(),
                                         msg);
        // F-08: the MEDIUM matches the index after a runtime repair — the
        // segment file on the fake is exactly the indexed size, no stray torn
        // tail left behind (currentUsedBytes alone could hide a stale file).
        std::vector<uint8_t>* seg = f.fs.file("evt_000.bin");
        TEST_ASSERT_NOT_NULL_MESSAGE(seg, msg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(used_after_two, seg->size(), msg);

        ReadResult r = readAll(f.store);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(2, r.count, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, r.first_seq, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(2, r.last_seq, msg);

        // Still usable: seq 3 was bound at enqueue and stays burned (D3 —
        // never reused), so the next append continues at 4 — a counted gap.
        const auto r4 = f.store.append(makeEvent(0, 4));
        TEST_ASSERT_TRUE_MESSAGE(r4.ok(), msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(4, r4.value, msg);
        f.store.pump(2000);
        // F-08 again after the follow-up batch: file size grew by exactly one
        // record — the repaired tail is reused in place, nothing straddles.
        TEST_ASSERT_EQUAL_UINT_MESSAGE(used_after_two + kEventRecordBytes, seg->size(), msg);
        r = readAll(f.store);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(3, r.count, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(4, r.last_seq, msg);

        // The gap is skipped on reads, never filled with fabricated data (D8).
        bw::Event gap_buf[8];
        const auto gap = f.store.readFrom(3, gap_buf, 8);
        TEST_ASSERT_TRUE_MESSAGE(gap.ok(), msg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1, gap.value, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(4, gap_buf[0].seq, msg);
    }
}

// --- power-cut sweep: k bytes of a record reach the medium, no repair run ---

void test_torn_write_power_cut_at_every_byte_offset(void) {
    for (size_t k = 0; k <= kEventRecordBytes; ++k) {  // k == 60: full-record control
        char msg[64];
        std::snprintf(msg, sizeof(msg), "powercut k=%u", static_cast<unsigned>(k));

        Fixture f;
        fillEvents(f.store, 2);
        injectRecordTail(f.fs, "evt_000.bin", kSegmentHeaderBytes + 2 * kEventRecordBytes,
                         makeEvent(3, 3), k);

        Rebooted r = reboot(f.fs);
        const size_t expected = (k == kEventRecordBytes) ? 3 : 2;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((k > 0 && k < kEventRecordBytes) ? 1u : 0u,
                                         r.counters.torn_tails_repaired, msg);

        ReadResult got = readAll(r.store);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(expected, got.count, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, got.first_seq, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(static_cast<uint32_t>(expected), got.last_seq, msg);

        const uint32_t next = static_cast<uint32_t>(expected + 1);
        const auto append = r.store.append(makeEvent(0, next));
        TEST_ASSERT_TRUE_MESSAGE(append.ok(), msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(next, append.value, msg);
        r.store.pump(3000);
        got = readAll(r.store);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(expected + 1, got.count, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(next, got.last_seq, msg);
    }
}

// --- rotation boundary: the new segment's 16-byte header write is torn ------

void test_torn_write_rotation_header_at_every_byte_offset(void) {
    // 272 events fill one segment exactly: 16 + 272*60 = 16336 <= 16384 and
    // #273 must rotate before it can be written (records never straddle).
    const uint32_t kFill =
        static_cast<uint32_t>((kSegmentSizeBytes - kSegmentHeaderBytes) / kEventRecordBytes);
    for (size_t k = 0; k <= kSegmentHeaderBytes; ++k) {  // k == 16: full-header control
        char msg[64];
        std::snprintf(msg, sizeof(msg), "rotation-header k=%u", static_cast<unsigned>(k));
        const bool control = (k == kSegmentHeaderBytes);

        Fixture f;
        fillEvents(f.store, kFill);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1, f.store.segments().segmentCount(), msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(kSegmentHeaderBytes + kFill * kEventRecordBytes,
                                         f.store.segments().currentUsedBytes(), msg);

        f.fs.armTornWrite(static_cast<int64_t>(k));
        TEST_ASSERT_TRUE_MESSAGE(f.store.append(makeEvent(0, kFill + 1)).ok(), msg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1, f.store.pump(4000), msg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(control ? 0u : 1u,
                                       f.faults.countOf(bw::FaultCode::StorageDegraded), msg);

        // Reboot: a partial segment header is skipped AND removed (D4) —
        // never guessed at — while segment 0 keeps every event.
        Rebooted r = reboot(f.fs);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(control ? 0u : 1u, r.counters.segments_skipped, msg);
        ReadResult got = readAll(r.store);
        const size_t expected = control ? kFill + 1 : kFill;
        TEST_ASSERT_EQUAL_UINT_MESSAGE(expected, got.count, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, got.first_seq, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(static_cast<uint32_t>(expected), got.last_seq, msg);

        // Append continues and the retry rotation succeeds.
        const uint32_t next = static_cast<uint32_t>(expected + 1);
        const auto append = r.store.append(makeEvent(0, next));
        TEST_ASSERT_TRUE_MESSAGE(append.ok(), msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(next, append.value, msg);
        r.store.pump(5000);
        got = readAll(r.store);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(expected + 1, got.count, msg);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(2, r.store.segments().segmentCount(), msg);
    }
}

// --- rotation boundary: first record of the NEW segment is torn -------------

void test_torn_write_new_segment_first_record_at_every_byte_offset(void) {
    const size_t tail = kSegmentHeaderBytes + kEventRecordBytes;  // header + event 273
    for (size_t k = 0; k <= kEventRecordBytes; ++k) {             // k == 60: control
        char msg[64];
        std::snprintf(msg, sizeof(msg), "new-segment-record k=%u", static_cast<unsigned>(k));

        Fixture f;
        fillEvents(f.store, 272);
        TEST_ASSERT_TRUE_MESSAGE(f.store.append(makeEvent(0, 273)).ok(), msg);
        f.store.pump(4000);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(2, f.store.segments().segmentCount(), msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(tail, f.store.segments().currentUsedBytes(), msg);

        injectRecordTail(f.fs, "evt_001.bin", tail, makeEvent(274, 274), k);

        Rebooted r = reboot(f.fs);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, r.counters.segments_skipped, msg);  // header intact
        const size_t expected = (k == kEventRecordBytes) ? 274 : 273;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((k > 0 && k < kEventRecordBytes) ? 1u : 0u,
                                         r.counters.torn_tails_repaired, msg);
        ReadResult got = readAll(r.store);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(expected, got.count, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, got.first_seq, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(static_cast<uint32_t>(expected), got.last_seq, msg);

        const uint32_t next = static_cast<uint32_t>(expected + 1);
        const auto append = r.store.append(makeEvent(0, next));
        TEST_ASSERT_TRUE_MESSAGE(append.ok(), msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(next, append.value, msg);
        r.store.pump(5000);
        got = readAll(r.store);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(expected + 1, got.count, msg);
        // The record fits the repaired/new segment: no rotation was needed.
        TEST_ASSERT_EQUAL_UINT_MESSAGE(2, r.store.segments().segmentCount(), msg);
        const uint32_t used_after = (k == kEventRecordBytes)
                                        ? static_cast<uint32_t>(tail + 2 * kEventRecordBytes)
                                        : static_cast<uint32_t>(tail + kEventRecordBytes);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(used_after, r.store.segments().currentUsedBytes(), msg);
    }
}

// --- straddle: the torn record sits in the LAST slot of segment 0 -----------

void test_torn_write_straddles_segment_end_at_every_byte_offset(void) {
    const uint32_t kFill = 271;
    const size_t tail = kSegmentHeaderBytes + kFill * kEventRecordBytes;  // 16276
    for (size_t k = 0; k <= kEventRecordBytes; ++k) {                     // k == 60: control
        char msg[64];
        std::snprintf(msg, sizeof(msg), "straddle k=%u", static_cast<unsigned>(k));

        Fixture f;
        fillEvents(f.store, kFill);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1, f.store.segments().segmentCount(), msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(tail, f.store.segments().currentUsedBytes(), msg);

        injectRecordTail(f.fs, "evt_000.bin", tail, makeEvent(272, 272), k);

        Rebooted r = reboot(f.fs);
        const size_t expected = (k == kEventRecordBytes) ? kFill + 1 : kFill;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((k > 0 && k < kEventRecordBytes) ? 1u : 0u,
                                         r.counters.torn_tails_repaired, msg);
        ReadResult got = readAll(r.store);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(expected, got.count, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, got.first_seq, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(static_cast<uint32_t>(expected), got.last_seq, msg);

        const uint32_t next = static_cast<uint32_t>(expected + 1);
        const auto append = r.store.append(makeEvent(0, next));
        TEST_ASSERT_TRUE_MESSAGE(append.ok(), msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(next, append.value, msg);
        r.store.pump(5000);
        got = readAll(r.store);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(expected + 1, got.count, msg);
        if (k == kEventRecordBytes) {
            // 272 records now fill segment 0; #273 rotated into a new one.
            TEST_ASSERT_EQUAL_UINT_MESSAGE(2, r.store.segments().segmentCount(), msg);
        } else {
            // The repaired tail is reusable in place: no straddling, no
            // premature rotation.
            TEST_ASSERT_EQUAL_UINT_MESSAGE(1, r.store.segments().segmentCount(), msg);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(static_cast<uint32_t>(tail + kEventRecordBytes),
                                             r.store.segments().currentUsedBytes(), msg);
        }
    }
}

// --- sync marker (D5): a torn checkpoint never advances the cursor ----------

void test_torn_write_sync_marker_at_every_byte_offset(void) {
    const size_t tail = kSegmentHeaderBytes + 5 * kEventRecordBytes + kSyncMarkerRecordBytes;
    for (size_t k = 0; k <= kSyncMarkerRecordBytes; ++k) {  // k == 16: full-marker control
        char msg[64];
        std::snprintf(msg, sizeof(msg), "sync-marker k=%u", static_cast<unsigned>(k));
        const bool control = (k == kSyncMarkerRecordBytes);

        Fixture f;
        fillEvents(f.store, 5);
        TEST_ASSERT_TRUE_MESSAGE(f.store.markSyncedThrough(3).ok(), msg);
        f.store.pump(500);  // first checkpoint flushes immediately (D5)
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(tail, f.store.segments().currentUsedBytes(), msg);

        // The next checkpoint (cursor 5) is cut mid-write.
        injectMarkerTail(f.fs, "evt_000.bin", tail, /*seq=*/5, k);

        Rebooted r = reboot(f.fs);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((k > 0 && !control) ? 1u : 0u,
                                         r.counters.torn_tails_repaired, msg);
        // Conservative cursor after power cut: the torn checkpoint never
        // counts, the previous one does (control: the full marker does).
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(control ? 5u : 3u, r.store.lastSyncedSeq(), msg);
        // Events are untouched by a torn checkpoint.
        ReadResult got = readAll(r.store);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(5, got.count, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, got.first_seq, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(5, got.last_seq, msg);

        if (!control) {
            // The cursor advances again after recovery (monotonic, idempotent).
            TEST_ASSERT_TRUE_MESSAGE(r.store.markSyncedThrough(5).ok(), msg);
            TEST_ASSERT_EQUAL_UINT_MESSAGE(1, r.store.pump(1000), msg);
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(5, r.store.lastSyncedSeq(), msg);
            got = readAll(r.store);
            TEST_ASSERT_EQUAL_UINT_MESSAGE(5, got.count, msg);  // markers are not events
        }
    }
}
