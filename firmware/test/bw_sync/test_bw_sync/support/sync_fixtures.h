#pragma once

// bw_sync test support — shared fixtures: deterministic event builders and the
// pump/drain loop every flow test drives (FakeClock-style: time is explicit,
// no wall clock, no sleeps).

#include <cstdint>

#include <bw/events.h>
#include <bw/providers.h>
#include <bw/sync/sync_session.h>
#include <bw/testkit/host_only.h>

#include "lossy_transport_fake.h"
#include "phone_harness.h"

namespace f20test {

// A recognizable, fully-populated event; the store assigns seq on append.
inline bw::Event makeEvent(bw::EventType type, bw::TimeMs t_start_ms) {
    bw::Event e{};
    e.t_start_ms = t_start_ms;
    e.boot_id = 42;
    e.duration_ms = 250;
    e.t_start_unix = 0;  // unknown until a TimeSync (§4.5)
    e.confidence = 0.5f;
    e.reasons = 0;
    e.quality = {bw::DataQuality::Good, bw::DataQuality::Good};
    e.type = type;
    e.label = bw::UserLabel::Unlabeled;
    e.context = bw::ActivityContext::Unknown;
    e.alertness_at_start = bw::AlertnessState::Unknown;
    e.detector_id = 9;
    e.schema_version = bw::Event::kSchemaVersion;
    return e;
}

// Appends `count` events starting at t = 1000 ms, 1 s apart. Returns the last
// appended seq (0 if the store refused).
inline std::uint32_t fillStore(bw::IEventStore& store, std::size_t count) {
    std::uint32_t last = 0;
    for (std::size_t i = 0; i < count; ++i) {
        bw::Result<std::uint32_t> r =
            store.append(makeEvent(bw::EventType::DrowsinessEvent, 1000 + i * 1000));
        if (!r.ok()) {
            return 0;
        }
        last = r.value;
    }
    return last;
}

// One exchange: the engine pumps once, then the companion drains and acks.
inline void pumpAndDrain(bw::sync::SyncSession& session, LossyTransportFake& fake,
                         PhoneHarness& harness, bw::TimeMs& now) {
    (void)fake;  // the session holds the transport pointer; the fake needs no per-pump work
    (void)session.step(now);
    ++now;
    (void)harness.drainAndAck();
}

// Runs the loop to quiescence (no pending phone frames, engine Idle or the
// iteration cap hit). Returns iterations used.
inline std::size_t runToQuiescence(bw::sync::SyncSession& session, LossyTransportFake& fake,
                                   PhoneHarness& harness, bw::TimeMs& now,
                                   std::size_t max_iterations = 64) {
    std::size_t i = 0;
    for (; i < max_iterations; ++i) {
        // "Movement" = anything that can unblock the next round: stored events,
        // accepted data frames, status frames, or a written (pending) ack.
        const std::size_t before = harness.storedSeqs().size() + fake.dataFramesAccepted() +
                                   harness.statusFrames() + harness.acksWritten();
        pumpAndDrain(session, fake, harness, now);
        const std::size_t after = harness.storedSeqs().size() + fake.dataFramesAccepted() +
                                  harness.statusFrames() + harness.acksWritten();
        if (after == before && fake.phoneFramesPending() == 0) {
            break;  // nothing moved this round: the scripted run is over
        }
    }
    return i;
}

}  // namespace f20test
