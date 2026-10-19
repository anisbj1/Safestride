// W4 M4 exit evidence (docs/05 step 5): the REAL cross-feature pairings wave
// 4 introduces, on the host over ONE RamFsFake flash medium:
//
//  T1  F16 -> F18: the real HistoryAggregator over the REAL LittleFsEventStore
//      + REAL KvStore (F16's own suite only ever pairs fakes — FaultyEventStore/
//      FaultyKvStore / InMemory*), through a simulated power cut (fresh objects
//      over the same medium): day record byte-identical to an independent
//      recompute, cursor restored, no double count under extra ticks.
//  T2  F20 -> F18: the real SyncSession streaming the REAL store to a phone
//      harness, a mid-transfer link cut (partial sync), device reboot, resume:
//      the markSyncedThrough() marker survives through the store's segment
//      meta, and the resumed session transfers ONLY the remainder
//      (eventsSent == N - marker, zero duplicate deliveries).
//  T3  F16 + F20 over ONE store instance: aggregation and sync interleaved on
//      the same medium, then a reboot where BOTH cursors recover (history via
//      KV meta, sync via segment marker) and both consumers pick up the same
//      2 new events without disturbing each other.
//
// No single-feature suite can make this check: F16 pairs with a fake store,
// F20 pairs with a fake store — only this suite proves the two halves agree
// with the one real medium app.cpp wires at P4 (f16::wireHistoryInit and
// f20::wire both bind &f18::eventStore()).
//
// Application-level suite (integration paths — no feature's Owns list covers
// firmware/test/test_*_dataflow); created at W4 P4 (2026-10-16), modeled on
// test_wave3_dataflow.

#include <unity.h>

#include <bw/history/history_aggregator.h>
#include <bw/storage/constants.h>
#include <bw/storage/event_store.h>
#include <bw/storage/kv_store.h>
#include <bw/sync/sync_session.h>
#include <bw/testkit/fake_clock.h>

#include "../bw_history/test_bw_history/history_test_support.h"
#include "../bw_storage/test_bw_storage/ram_fs_fake.h"
#include "../bw_sync/test_bw_sync/support/lossy_transport_fake.h"
#include "../bw_sync/test_bw_sync/support/phone_harness.h"
// bw_sync's fakes live in THAT suite's directory; PlatformIO compiles only
// this suite's sources, so textually include their TUs here (same-TU unity
// include — their headers resolve relative to their own directory, and the
// bw_sync suite links the same .cpp files into its own binary unchanged).
#include "../bw_sync/test_bw_sync/support/lossy_transport_fake.cpp"
#include "../bw_sync/test_bw_sync/support/phone_harness.cpp"

namespace {

using namespace f16_test;
namespace bh = bw::history;
namespace bs = bw::storage;
namespace bsync = bw::sync;

// A "device generation": the REAL event store + REAL KV over ONE RamFsFake
// medium — app.cpp's wire_storage statics (store{adapter, counters},
// kv{adapter, counters}) minus the task. "Reboot" = a second generation over
// the same medium: flash contents survive, RAM state does not.
struct DeviceGen {
    explicit DeviceGen(RamFsFake& medium) : store{medium, counters}, kv{medium, counters} {
        TEST_ASSERT_TRUE(store.mount().ok());
    }
    bs::StorageCounters counters{};
    bs::LittleFsEventStore store;
    bs::KvStore kv;
};

uint32_t appendReal(bs::LittleFsEventStore& store, const bw::Event& e) {
    const bw::Result<uint32_t> r = store.append(e);
    TEST_ASSERT_TRUE(r.ok());
    return r.value;
}

// StorageTask's job on device (core 0): the store is async — append() queues
// + reserves the seq, pump() writes the flash records readFrom() then serves
// (event_store.h contract).
void flushPending(bs::LittleFsEventStore& store, bw::TimeMs t) {
    for (int i = 0; i < 1000 && store.pendingOps() > 0; ++i) {
        (void)store.pump(t);
    }
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(store.pendingOps()));
}

// markSyncedThrough() marks in RAM; pump() persists it only after
// kSyncCheckpointIntervalMs of dirty time (event_store.cpp D5 — power-cut
// safe, cheap to defer). Advance the pump clock past the interval so the
// marker is durable BEFORE a simulated power cut.
void flushMarker(bs::LittleFsEventStore& store, bw::TimeMs& now) {
    now += bs::kSyncCheckpointIntervalMs + 100;
    (void)store.pump(now);
}

// One device pump cadence (f20::pumpOnce = one session step). drain-first
// ordering: frames sent at step k are drained+acked at round k+1 and applied
// at step k+1 — the loop ends on a step, so the final ack of the run is
// applied before we assert.
void syncRounds(bsync::SyncSession& session, f20test::PhoneHarness& phone, bw::TimeMs& now,
                int rounds) {
    for (int i = 0; i < rounds; ++i) {
        phone.drainAndAck();
        (void)session.step(now);
        now += 10;
    }
}

// Day-5 fixture identical to F16's own T1 (test_aggregation.cpp): Boot anchor,
// nap + MainSleep + Rest on logical day 5, an open marker and an ignored type
// (both excluded), a day-6 nap that finalizes day 5. Returns the last seq.
uint32_t appendDay5Fixture(bs::LittleFsEventStore& store) {
    (void)appendReal(store, bootEvent(1, 100));
    (void)appendReal(store, napCompleted(1, wallUnix(5, 20, 0), 1000));
    (void)appendReal(
        store, sleepClose(wallUnix(5, 23, 50), 2000, 26'400'000, bw::EpisodeClass::MainSleep));
    (void)appendReal(store,
                     sleepClose(wallUnix(5, 10, 0), 3000, 1'800'000, bw::EpisodeClass::Rest));
    (void)appendReal(store, sleepOpenMarker(wallUnix(5, 11, 0), 4000));
    (void)appendReal(store, ignoredDrowsy(wallUnix(5, 12, 0), 5000));
    const uint32_t last = appendReal(store, napCompleted(1, wallUnix(6, 8, 0), 6000));
    flushPending(store, 1000);
    return last;
}

// Byte-compare each ring record against recomputeDay() — the DoD's T5
// independent second implementation (history_test_support.h).
void assertRingMatchesRecompute(bs::LittleFsEventStore& store, const bh::DailySummary* ring,
                                size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const bh::DailySummary fresh =
            recomputeDay(store, ring[i].first_seq, ring[i].last_seq, ring[i].day_key);
        TEST_ASSERT_EQUAL_MEMORY(&fresh, &ring[i], sizeof(bh::DailySummary));
    }
}

}  // namespace

// T1: F16 -> F18 real pairing. The real aggregator boots over the REAL store +
// REAL KV, aggregates a wall-time day, then survives a simulated power cut
// with byte-identical summaries, a restored cursor and no double count.
void test_history_real_store_reboot_persistence(void) {
    RamFsFake fs{512u * 1024u};
    bw::testkit::FakeClock clock;

    DeviceGen gen{fs};
    const uint32_t last_seq = appendDay5Fixture(gen.store);

    bh::HistoryAggregator agg{bh::HistoryConfig{}, &gen.store, &gen.kv, &clock};
    TEST_ASSERT_TRUE(bootToReady(agg).ok());
    drainTo(agg, last_seq);
    TEST_ASSERT_EQUAL_UINT32(last_seq, agg.lastProcessedSeq());
    TEST_ASSERT_EQUAL_UINT32(1, agg.summaryCount());

    bh::DailySummary before[4]{};
    const size_t n1 = agg.summaries(before, 4);
    TEST_ASSERT_EQUAL_UINT32(1, n1);
    assertRingMatchesRecompute(gen.store, before, n1);

    // Power cut: fresh store/KV/aggregator over the SAME medium — no
    // unmount, no cooperation (wave3's reboot model).
    DeviceGen gen2{fs};
    bh::HistoryAggregator agg2{bh::HistoryConfig{}, &gen2.store, &gen2.kv, &clock};
    TEST_ASSERT_TRUE(bootToReady(agg2).ok());
    drainTo(agg2, last_seq);  // no-op when the cursor was restored from KV meta
    TEST_ASSERT_EQUAL_UINT32(last_seq, agg2.lastProcessedSeq());
    TEST_ASSERT_EQUAL_UINT32(n1, agg2.summaryCount());

    bh::DailySummary after[4]{};
    const size_t n2 = agg2.summaries(after, 4);
    TEST_ASSERT_EQUAL_UINT32(n1, n2);
    for (size_t i = 0; i < n2; ++i) {
        TEST_ASSERT_EQUAL_MEMORY(&before[i], &after[i], sizeof(bh::DailySummary));
    }

    // No double count: five more ticks over an unchanged log change nothing.
    for (int i = 0; i < 5; ++i) {
        (void)agg2.step();
    }
    TEST_ASSERT_EQUAL_UINT32(last_seq, agg2.lastProcessedSeq());
    TEST_ASSERT_EQUAL_UINT32(n1, agg2.summaryCount());
    bh::DailySummary after2[4]{};
    const size_t n3 = agg2.summaries(after2, 4);
    TEST_ASSERT_EQUAL_UINT32(n1, n3);
    for (size_t i = 0; i < n3; ++i) {
        TEST_ASSERT_EQUAL_MEMORY(&before[i], &after2[i], sizeof(bh::DailySummary));
    }
}

// T2: F20 -> F18 real pairing. The real session streams the REAL store to a
// phone harness; a mid-transfer link cut leaves a partial marker; the device
// "reboots" and the marker must come back from the store's segment meta; the
// resumed session transfers only the remainder.
void test_sync_real_store_partial_and_resume(void) {
    // > kSyncMaxEventsPerRead (16): one step pumps at most one store read, so
    // the transfer cannot complete before the partial cut below — 0 < marker <
    // kSyncEventCount holds by construction at the first applied ack.
    constexpr uint32_t kSyncEventCount = 60;

    RamFsFake fs{512u * 1024u};
    DeviceGen gen{fs};

    (void)appendReal(gen.store, bootEvent(1, 100));
    for (uint32_t i = 1; i < kSyncEventCount; ++i) {
        (void)appendReal(gen.store, ignoredDrowsy(wallUnix(5, 10, 0), 1000 + i));
        // The op queue is bounded (kStorageQueueDepth = 16) — drain it while
        // events arrive, exactly what StorageTask does on device.
        if ((i % 8u) == 0u) {
            (void)gen.store.pump(1000);
        }
    }
    flushPending(gen.store, 1000);

    f20test::LossyTransportFake transport;
    f20test::PhoneHarness phone{transport};
    bsync::SyncSession session;
    session.bind(&transport, &gen.store);
    transport.setLinkUp(true);
    TEST_ASSERT_TRUE(phone.requestFrom(1));

    bw::TimeMs now = 0;
    uint32_t marker = 0;
    for (int round = 0; round < 40; ++round) {
        phone.drainAndAck();
        (void)session.step(now);
        now += 10;
        marker = gen.store.lastSyncedSeq();
        if (marker > 0 && marker < kSyncEventCount) {
            transport.setLinkUp(false);  // the cut: in-flight frames die, acked state stays
            break;
        }
    }
    // Partial sync actually happened and both sides agree at the cut.
    TEST_ASSERT_TRUE(marker > 0);
    TEST_ASSERT_TRUE(marker < kSyncEventCount);
    TEST_ASSERT_EQUAL_UINT32(marker, phone.highWater());
    TEST_ASSERT_EQUAL_UINT32(marker, gen.store.lastSyncedSeq());

    // Durability: persist the RAM marker through pump(), then power-cut.
    flushMarker(gen.store, now);
    DeviceGen gen2{fs};
    TEST_ASSERT_EQUAL_UINT32(marker, gen2.store.lastSyncedSeq());  // THE claim

    // Resume: the companion keeps its hw_, the device keeps its marker — only
    // the remainder crosses the wire. (A device reboot also drops transport
    // RAM state: reset() + link up, as wire() would.)
    bsync::SyncSession session2;
    session2.bind(&transport, &gen2.store);
    transport.reset();
    transport.setLinkUp(true);
    TEST_ASSERT_TRUE(phone.requestFrom(phone.highWater() + 1));
    syncRounds(session2, phone, now, 40);

    TEST_ASSERT_EQUAL_UINT32(kSyncEventCount - marker, session2.eventsSent());
    TEST_ASSERT_EQUAL_UINT32(kSyncEventCount, phone.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT32(kSyncEventCount, gen2.store.lastSyncedSeq());
    TEST_ASSERT_EQUAL_UINT32(0, phone.duplicateDeliveries());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bsync::SyncPhase::Idle),
                          static_cast<int>(session2.phase()));
}

// T3: F16 + F20 over ONE store instance. Aggregation and sync interleaved on
// the same medium (the P4 wiring: both features hold pointers to the same
// &f18::eventStore()), then a reboot where BOTH cursors recover independently
// and both consumers pick up the same 2 new events without disturbing each
// other's state.
void test_history_and_sync_one_store(void) {
    RamFsFake fs{512u * 1024u};
    bw::testkit::FakeClock clock;

    DeviceGen gen{fs};
    const uint32_t last_seq = appendDay5Fixture(gen.store);

    // History side (as f16::wireHistoryInit wires it) ...
    bh::HistoryAggregator agg{bh::HistoryConfig{}, &gen.store, &gen.kv, &clock};
    TEST_ASSERT_TRUE(bootToReady(agg).ok());

    // ... and sync side (as f20::wire wires it) over the SAME store.
    f20test::LossyTransportFake transport;
    f20test::PhoneHarness phone{transport};
    bsync::SyncSession session;
    session.bind(&transport, &gen.store);
    transport.setLinkUp(true);
    TEST_ASSERT_TRUE(phone.requestFrom(1));

    // Interleave both pumps: aggregate at its per-tick batch bound, sync at
    // the service pump's cadence — neither starves the other.
    bw::TimeMs now = 0;
    for (int i = 0; i < 100; ++i) {
        if (agg.lastProcessedSeq() < last_seq) {
            (void)agg.step();
        }
        phone.drainAndAck();
        (void)session.step(now);
        now += 10;
    }

    // Both completed on the shared medium.
    TEST_ASSERT_EQUAL_UINT32(last_seq, agg.lastProcessedSeq());
    TEST_ASSERT_EQUAL_UINT32(1, agg.summaryCount());
    TEST_ASSERT_EQUAL_UINT32(last_seq, gen.store.lastSyncedSeq());
    TEST_ASSERT_EQUAL_UINT32(last_seq, phone.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT32(0, phone.duplicateDeliveries());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bsync::SyncPhase::Idle),
                          static_cast<int>(session.phase()));

    // Sync's markSyncedThrough() left aggregation's view byte-identical to
    // the independent recompute.
    bh::DailySummary before[4]{};
    const size_t n1 = agg.summaries(before, 4);
    TEST_ASSERT_EQUAL_UINT32(1, n1);
    assertRingMatchesRecompute(gen.store, before, n1);

    // Power cut: BOTH cursors must come back — history via KV meta, sync via
    // the flushed segment marker.
    flushMarker(gen.store, now);
    DeviceGen gen2{fs};
    TEST_ASSERT_EQUAL_UINT32(last_seq, gen2.store.lastSyncedSeq());
    bh::HistoryAggregator agg2{bh::HistoryConfig{}, &gen2.store, &gen2.kv, &clock};
    TEST_ASSERT_TRUE(bootToReady(agg2).ok());
    drainTo(agg2, last_seq);
    TEST_ASSERT_EQUAL_UINT32(last_seq, agg2.lastProcessedSeq());
    bh::DailySummary after[4]{};
    const size_t n2 = agg2.summaries(after, 4);
    TEST_ASSERT_EQUAL_UINT32(n1, n2);
    for (size_t i = 0; i < n2; ++i) {
        TEST_ASSERT_EQUAL_MEMORY(&before[i], &after[i], sizeof(bh::DailySummary));
    }

    // Both consumers pick up the SAME 2 new events on the rebooted medium:
    // ignored types advance the history cursor without touching the
    // finalized day-5 record, and sync transfers exactly the 2 new events.
    const uint32_t s_new1 = appendReal(gen2.store, ignoredDrowsy(wallUnix(6, 9, 0), 7000));
    const uint32_t s_new2 = appendReal(gen2.store, ignoredDrowsy(wallUnix(6, 9, 5), 7100));
    TEST_ASSERT_EQUAL_UINT32(last_seq + 1, s_new1);
    TEST_ASSERT_EQUAL_UINT32(last_seq + 2, s_new2);
    flushPending(gen2.store, now);
    drainTo(agg2, s_new2);
    TEST_ASSERT_EQUAL_UINT32(s_new2, agg2.lastProcessedSeq());
    bh::DailySummary after_new[4]{};
    const size_t n3 = agg2.summaries(after_new, 4);
    TEST_ASSERT_EQUAL_UINT32(n1, n3);
    for (size_t i = 0; i < n3; ++i) {
        TEST_ASSERT_EQUAL_MEMORY(&before[i], &after_new[i], sizeof(bh::DailySummary));
    }

    session.bind(&transport, &gen2.store);  // rewire resets cumulative counters (bind doc)
    transport.reset();
    transport.setLinkUp(true);
    TEST_ASSERT_TRUE(phone.requestFrom(phone.highWater() + 1));
    syncRounds(session, phone, now, 40);

    TEST_ASSERT_EQUAL_UINT32(2, session.eventsSent());  // only the remainder — no re-transfer
    TEST_ASSERT_EQUAL_UINT32(last_seq + 2, phone.highWater());
    TEST_ASSERT_EQUAL_UINT32(last_seq + 2, phone.storedSeqs().size());
    TEST_ASSERT_EQUAL_UINT32(last_seq + 2, gen2.store.lastSyncedSeq());
    TEST_ASSERT_EQUAL_UINT32(0, phone.duplicateDeliveries());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bsync::SyncPhase::Idle),
                          static_cast<int>(session.phase()));
}
