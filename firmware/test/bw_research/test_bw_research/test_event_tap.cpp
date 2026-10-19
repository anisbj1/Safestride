// F28 §9.4 — event/marker tap (D8): a session contains events that happen
// INSIDE it (pre-stored events are baseline-skipped), new events become one E
// row, UserMarked additionally becomes an M row, and back-pressure defers rows
// instead of losing them.

#include <cstdio>
#include <cstring>
#include <string>

#include <bw/containers/latest_value.h>
#include <bw/events.h>
#include <bw/research/research_config.h>
#include <bw/research/research_streamer.h>
#include <bw/testkit/mem_event_store.h>

#include <unity.h>

namespace {

bw::Event makeEvent(bw::EventType type, bw::TimeMs t_start_ms, float confidence = 0.5f) {
    bw::Event event{};
    event.t_start_ms = t_start_ms;
    event.duration_ms = 1000;
    event.t_start_unix = 1700000000;
    event.confidence = confidence;
    event.type = type;
    event.label = bw::UserLabel::Unlabeled;
    event.context = bw::ActivityContext::Stationary;
    event.alertness_at_start = bw::AlertnessState::Awake;
    event.quality = {bw::DataQuality::Good, bw::DataQuality::Good};
    event.schema_version = bw::Event::kSchemaVersion;
    return event;
}

struct EventFixture {
    bw::research::ResearchStreamer streamer{true};
    bw::testkit::InMemoryEventStore<64> store;
    bw::LatestValue<bw::MotionFeatures> motion;
    std::string out;

    EventFixture() {
        bw::research::StreamSources sources;
        sources.motion = &motion;
        sources.firmware_version = "0.1.0";
        sources.events = &store;
        streamer.setSources(sources);
    }

    void activate(bw::TimeMs t_ms = 0) {
        streamer.runTick(t_ms);
        drainAll();
        out.clear();
    }

    void drainAll() {
        char chunk[256];
        size_t n = 0;
        while ((n = streamer.drain(chunk, sizeof(chunk))) > 0) {
            out.append(chunk, n);
        }
    }

    size_t countLines(const char* prefix) const {
        size_t count = 0;
        size_t pos = out.find(prefix, 0);
        while (pos != std::string::npos) {
            ++count;
            pos = out.find(prefix, pos + 1);
        }
        return count;
    }
};

}  // namespace

void test_events_stored_before_session_are_not_replayed(void) {
    // D8 baseline-drain: everything already in the store predates the session
    // (possibly by days) and must not leak into this capture.
    EventFixture f;
    TEST_ASSERT_TRUE(f.store.append(makeEvent(bw::EventType::SleepEpisode, 100)).ok());
    TEST_ASSERT_TRUE(f.store.append(makeEvent(bw::EventType::NapPlanned, 200)).ok());

    f.streamer.runTick(0);
    f.drainAll();
    TEST_ASSERT_EQUAL_UINT(0, f.countLines("E,"));
}

void test_new_event_emitted_exactly_once(void) {
    EventFixture f;
    TEST_ASSERT_TRUE(f.store.append(makeEvent(bw::EventType::SleepEpisode, 100)).ok());
    f.activate();

    const uint32_t seq =
        f.store.append(makeEvent(bw::EventType::DrowsinessEvent, 5000, 0.75f)).value;
    TEST_ASSERT_EQUAL_UINT(2, seq);  // one pre-session event took seq 1

    f.streamer.runTick(100);
    f.drainAll();
    TEST_ASSERT_EQUAL_UINT(1, f.countLines("E,2,"));
    TEST_ASSERT_TRUE(f.out.find("E,2,5000,") != std::string::npos);
    TEST_ASSERT_TRUE(f.out.find(",0.750,") != std::string::npos);
    TEST_ASSERT_TRUE(f.out.find("DrowsinessEvent") != std::string::npos);

    f.out.clear();
    f.streamer.runTick(200);
    f.drainAll();
    TEST_ASSERT_EQUAL_UINT(0, f.countLines("E,"));  // cursor advanced: no repeat
}

void test_user_marked_emits_event_and_marker_rows(void) {
    EventFixture f;
    f.activate();
    const uint32_t seq = f.store.append(makeEvent(bw::EventType::UserMarked, 8000)).value;

    f.streamer.runTick(100);
    f.drainAll();
    TEST_ASSERT_EQUAL_UINT(1, f.countLines("E,"));
    TEST_ASSERT_EQUAL_UINT(1, f.countLines("M,"));
    char needle[32];
    std::snprintf(needle, sizeof(needle), "M,8000,user_marked#%u", seq);
    TEST_ASSERT_TRUE(f.out.find(needle) != std::string::npos);
}

void test_event_rows_deferred_on_full_ring_are_not_lost(void) {
    // Back-pressure: the cursor only advances when the row is really out, so a
    // full ring delays the event to a later tick instead of swallowing it.
    EventFixture f;
    f.activate();

    bw::TimeMs t = 0;
    for (int i = 0; i < 40; ++i) {  // fill the ring without draining
        t += 1000;
        bw::MotionFeatures features{};
        features.t_ms = t;
        f.motion.set(features);
        f.streamer.runTick(t);
    }
    TEST_ASSERT_TRUE(f.store.append(makeEvent(bw::EventType::AlertRaised, 41000)).ok());
    f.streamer.runTick(t + 100);
    f.drainAll();
    TEST_ASSERT_EQUAL_UINT(0, f.countLines("E,"));
    // Deferral, not loss: nothing was dropped, the row is only late (§10).
    TEST_ASSERT_EQUAL_UINT(0, f.streamer.lineDrops());

    f.streamer.runTick(t + 200);
    f.drainAll();
    TEST_ASSERT_EQUAL_UINT(1, f.countLines("E,"));
}

void test_event_cursor_resets_for_a_new_activation(void) {
    // Deactivate/activate (e.g. mode toggling): the new session re-baselines,
    // so events from the previous session do not appear in the new one.
    EventFixture f;
    f.activate();
    TEST_ASSERT_TRUE(f.store.append(makeEvent(bw::EventType::BatteryWarning, 5000)).ok());
    f.streamer.runTick(100);
    (void)f.drainAll();
    TEST_ASSERT_EQUAL_UINT(1, f.countLines("E,"));
    f.out.clear();  // session 1 observed; now watch session 2 only

    f.streamer.setEnabled(false);
    f.streamer.runTick(200);  // deactivates
    TEST_ASSERT_TRUE(f.store.append(makeEvent(bw::EventType::TimeSync, 6000)).ok());

    f.streamer.setEnabled(true);
    f.streamer.runTick(300);  // re-activates: baseline skips 5000 and 6000
    (void)f.drainAll();
    TEST_ASSERT_EQUAL_UINT(0, f.countLines("E,"));

    f.streamer.runTick(400);
    (void)f.drainAll();
    TEST_ASSERT_EQUAL_UINT(0, f.countLines("E,"));  // still nothing new
}

void test_no_event_store_produces_no_event_rows(void) {
    // Absent source: E/M rows are simply absent, never fabricated (00 §1).
    bw::research::ResearchStreamer streamer(true);
    bw::research::StreamSources sources;
    sources.firmware_version = "0.1.0";
    streamer.setSources(sources);

    streamer.runTick(0);
    char out[1024];
    const size_t n = streamer.drain(out, sizeof(out));
    TEST_ASSERT_TRUE(n > 0);
    out[n] = '\0';
    TEST_ASSERT_NULL(std::strstr(out, "\nE,"));
    TEST_ASSERT_NULL(std::strstr(out, "\nM,"));
}
