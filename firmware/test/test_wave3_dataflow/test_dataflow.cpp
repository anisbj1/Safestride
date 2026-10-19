// the REAL F17 -> F18 pairing on the host — EventLogger drains through
// LittleFsEventStore (RamFsFake behind it), and events survive a simulated
// reboot (power cut: no unmount, fresh objects on the same medium) with the
// seq/boot_id recovery proven end to end. This is the cross-feature dataflow
// check no single-feature suite can make: F17 is tested against a fake store
// and F18 against synthetic appends, so only this suite proves the two real
// halves agree (append-returned seq, fan-out-after-append, recovery probe).
//
// Application-level suite (integration paths — no feature's Owns list
// covers it); created at W3 P4 (2026-10-13).

#include <unity.h>

#include <bw/eventlog/event_logger.h>
#include <bw/storage/event_store.h>

#include "../bw_storage/test_bw_storage/ram_fs_fake.h"

using bw::storage::LittleFsEventStore;
using bw::storage::StorageCounters;

namespace {

struct StepClock final : bw::IClock {
    bw::TimeMs t = 0;
    bw::UnixSeconds unix = 0;
    bw::TimeMs nowMs() const override { return t; }
    bw::UnixSeconds unixSeconds() const override { return unix; }
};

bw::Event makeEvent(bw::EventType type, uint8_t tag) {
    bw::Event e{};
    e.t_start_ms = 100000u + tag;
    e.duration_ms = 500u + tag;
    e.confidence = 0.5f;
    e.reasons = 0x01;
    e.payload[0] = tag;
    e.type = type;
    e.label = bw::UserLabel::Unlabeled;
    e.alertness_at_start = bw::AlertnessState::Awake;
    e.detector_id = 9;
    e.schema_version = bw::Event::kSchemaVersion;
    return e;
}

// Fan-out probe: F17 promises subscribers see every event whose append
// succeeded (fan-out after append, F17 §10).
struct RecordingSink final : bw::IEventSink {
    bw::Event got[8]{};
    size_t count = 0;
    bw::Result<void> publish(const bw::Event& e) override {
        if (count < 8) {
            got[count] = e;
        }
        ++count;
        return {};
    }
};

}  // namespace

// T-A: publish -> drain -> store append -> readFrom returns every event in
// order with store-assigned seqs — the exact pairing app.cpp wires at P4.
void test_dataflow_logger_to_store_in_order(void) {
    StepClock clock;
    RamFsFake fs{512u * 1024u};
    StorageCounters counters{};
    LittleFsEventStore store{fs, counters};
    TEST_ASSERT_TRUE(store.mount().ok());

    bw::eventlog::EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    clock.t = 1000;
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::DrowsinessEvent, 1)).ok());
    clock.t = 2000;
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SuspectedCataplexy, 2)).ok());
    clock.t = 3000;
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::AlertRaised, 3)).ok());

    // On device the pump (f17::eventLogTask, 100 ms) paces one drain cycle
    // per tick; here the integrator paces the same code path by hand.
    TEST_ASSERT_EQUAL(3, static_cast<int>(logger.drain(clock.t)));
    // StorageTask's job on device (core 0, wire_storage): the store is
    // async — append() queues + reserves the seq, pump() writes the flash
    // records that readFrom() then serves.
    (void)store.pump(clock.t);

    bw::Event out[8]{};
    const bw::Result<size_t> read = store.readFrom(1, out, 8);
    TEST_ASSERT_TRUE(read.ok());
    TEST_ASSERT_EQUAL(3, static_cast<int>(read.value));
    TEST_ASSERT_EQUAL_UINT32(1, out[0].seq);
    TEST_ASSERT_EQUAL_UINT32(2, out[1].seq);
    TEST_ASSERT_EQUAL_UINT32(3, out[2].seq);
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::DrowsinessEvent),
                      static_cast<int>(out[0].type));
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::SuspectedCataplexy),
                      static_cast<int>(out[1].type));
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::AlertRaised), static_cast<int>(out[2].type));
    TEST_ASSERT_EQUAL_UINT8(1, out[0].payload[0]);  // persisted verbatim (F18 DoD)
    TEST_ASSERT_EQUAL_UINT8(3, out[2].payload[0]);
    TEST_ASSERT_EQUAL_UINT32(100001u, out[0].t_start_ms);
    TEST_ASSERT_EQUAL_UINT32(100003u, out[2].t_start_ms);
}

// T-B: M3 exit — events survive a reboot. Power cut = no unmount and no
// flush: the inner scope just ends, then fresh store/logger objects mount
// the same medium (F18 torn-tail + seq recovery) and the dataflow continues
// with a NEW boot_id and a CONTINUED seq (audit F-01's rewind fix).
void test_dataflow_events_survive_reboot(void) {
    StepClock clock;
    RamFsFake fs{512u * 1024u};
    {
        StorageCounters counters{};
        LittleFsEventStore store{fs, counters};
        TEST_ASSERT_TRUE(store.mount().ok());
        bw::eventlog::EventLogger logger{&clock, nullptr};
        logger.attachStore(&store);
        logger.emitBoot();  // exactly what f17::wire publishes once per boot
        TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SleepEpisode, 7)).ok());
        TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::BatteryWarning, 8)).ok());
        TEST_ASSERT_EQUAL(3, static_cast<int>(logger.drain(5000)));
        (void)store.pump(5000);  // StorageTask on device
        // power cut: no unmount, no flush — inner scope just ends (applied
        // records persist; the queue is empty, so nothing is lost here).
    }
    // --- reboot ---
    StorageCounters counters2{};
    LittleFsEventStore store2{fs, counters2};
    TEST_ASSERT_TRUE(store2.mount().ok());
    bw::eventlog::EventLogger logger2{&clock, nullptr};
    logger2.attachStore(&store2);  // boot_id/seq recovery probe runs here
    logger2.emitBoot();            // the new boot's Boot event
    TEST_ASSERT_TRUE(logger2.publish(makeEvent(bw::EventType::DrowsinessEvent, 9)).ok());
    TEST_ASSERT_EQUAL(2, static_cast<int>(logger2.drain(6000)));
    (void)store2.pump(6000);

    bw::Event out[8]{};
    const bw::Result<size_t> read = store2.readFrom(1, out, 8);
    TEST_ASSERT_TRUE(read.ok());
    TEST_ASSERT_EQUAL(5, static_cast<int>(read.value));
    // Boot-1 events, verbatim and in order:
    TEST_ASSERT_EQUAL_UINT32(1, out[0].seq);
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::Boot), static_cast<int>(out[0].type));
    TEST_ASSERT_EQUAL_UINT32(1, out[0].boot_id);
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::SleepEpisode), static_cast<int>(out[1].type));
    TEST_ASSERT_EQUAL_UINT8(7, out[1].payload[0]);
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::BatteryWarning),
                      static_cast<int>(out[2].type));
    TEST_ASSERT_EQUAL_UINT8(8, out[2].payload[0]);
    // Boot-2 events: seq continues (never rewound or reused), boot_id bumped:
    TEST_ASSERT_EQUAL_UINT32(4, out[3].seq);
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::Boot), static_cast<int>(out[3].type));
    TEST_ASSERT_EQUAL_UINT32(2, out[3].boot_id);
    TEST_ASSERT_EQUAL_UINT32(5, out[4].seq);
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::DrowsinessEvent),
                      static_cast<int>(out[4].type));
    TEST_ASSERT_EQUAL_UINT32(2, out[4].boot_id);
}

// T-C: fan-out agrees with the store — subscribers see the same events, in
// the same order, whose append succeeded (nothing delivered that was not
// stored; nothing stored that was not delivered).
void test_dataflow_fanout_matches_store(void) {
    StepClock clock;
    RamFsFake fs{512u * 1024u};
    StorageCounters counters{};
    LittleFsEventStore store{fs, counters};
    TEST_ASSERT_TRUE(store.mount().ok());
    bw::eventlog::EventLogger logger{&clock, nullptr};
    logger.attachStore(&store);

    RecordingSink probe;
    TEST_ASSERT_TRUE(logger.addSubscriber(&probe));

    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::DrowsinessEvent, 1)).ok());
    TEST_ASSERT_TRUE(logger.publish(makeEvent(bw::EventType::SuspectedCataplexy, 2)).ok());
    TEST_ASSERT_EQUAL(2, static_cast<int>(logger.drain(1000)));
    (void)store.pump(1000);

    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(probe.count));
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::DrowsinessEvent),
                      static_cast<int>(probe.got[0].type));
    TEST_ASSERT_EQUAL(static_cast<int>(bw::EventType::SuspectedCataplexy),
                      static_cast<int>(probe.got[1].type));

    bw::Event out[4]{};
    const bw::Result<size_t> read = store.readFrom(1, out, 4);
    TEST_ASSERT_TRUE(read.ok());
    TEST_ASSERT_EQUAL(2, static_cast<int>(read.value));
    TEST_ASSERT_EQUAL_UINT32(1, out[0].seq);
    TEST_ASSERT_EQUAL_UINT32(2, out[1].seq);
}
