#pragma once

// firmware/test/bw_confirm/test_bw_confirm/confirm_fixtures.h — shared
// builders and the per-test fixture for the F14 suite (plan §7). Everything is
// deterministic: fixed timestamps, no wall clock — timelines are scripted by

#include <bw/alerts.h>
#include <bw/commands.h>
#include <bw/confirm/confirmation_manager.h>
#include <bw/events.h>

#include "fake_clock.h"
#include "fake_event_sink.h"
#include "fake_event_store.h"

namespace bw_confirm_test {

// A detector event requiring confirmation (seq assigned "by F17" in prod —
// tests pick it; detector 0 = someone else's producer).
inline bw::Event makeEvent(uint32_t seq, bw::EventType type, bw::TimeMs t_ms,
                           uint8_t detector_id = 0) {
    bw::Event e{};
    e.t_start_ms = t_ms;
    e.seq = seq;
    e.type = type;
    e.detector_id = detector_id;
    e.schema_version = bw::Event::kSchemaVersion;
    return e;
}

// Alert status edge for the D3 ack seam (F10 produces, composition root binds).
inline bw::AlertStatus makeAlert(bool acknowledged, bw::TimeMs t_ms) {
    bw::AlertStatus s{};
    s.t_ms = t_ms;
    s.level = acknowledged ? bw::AlertLevel::Warning : bw::AlertLevel::Active;
    s.acknowledged = acknowledged;
    s.since_ms = 100;
    return s;
}

// ConfirmEvent / RejectEvent command (plan §15: arg0 = target Event::seq).
inline bw::Command makeCommand(bw::CommandType type, uint32_t seq, bw::TimeMs t_ms) {
    bw::Command c{};
    c.type = type;
    c.t_ms = t_ms;
    c.source = 0;  // Button
    c.arg0 = seq;
    c.arg1 = 0;
    return c;
}

inline bw::Command makeConfirm(uint32_t seq, bw::TimeMs t_ms) {
    return makeCommand(bw::CommandType::ConfirmEvent, seq, t_ms);
}

inline bw::Command makeReject(uint32_t seq, bw::TimeMs t_ms) {
    return makeCommand(bw::CommandType::RejectEvent, seq, t_ms);
}

// Manager + fakes, store/sink already attached (plan §7). One instance per
// test — no shared state between test functions.
struct ConfirmFixture {
    bw::confirm::ConfirmationManager mgr{};
    FakeEventStore store;
    FakeEventSink sink;

    ConfirmFixture() {
        mgr.setEventStore(&store);
        mgr.setEventSink(&sink);
    }

    explicit ConfirmFixture(const bw::confirm::ConfirmConfig& cfg) : mgr(cfg) {
        mgr.setEventStore(&store);
        mgr.setEventSink(&sink);
    }

    bw::ConfirmPromptState state() const {
        bw::ConfirmPromptState s{};
        (void)mgr.latest(s);  // tests call this only after a step()
        return s;
    }
};

}  // namespace bw_confirm_test
