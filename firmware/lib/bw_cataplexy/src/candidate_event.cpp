// bw/cataplexy/candidate_event.cpp — F09 §3 event construction.

#include <bw/cataplexy/candidate_event.h>

#include <cstring>

#include <bw/cataplexy/cataplexy_config.h>

namespace bw::cataplexy {

Event makeSuspectedCataplexyEvent(const CandidateOutput& candidate) {
    Event event{};  // zero-fills: seq, boot_id and t_start_unix stay 0 (see header)

    event.t_start_ms = candidate.t_start_ms;
    event.duration_ms = candidate.duration_ms;
    event.confidence = candidate.confidence;
    event.reasons = candidate.reasons;

    // payload[0] bit0: ask F18 to persist the raw window around this event
    // (spec §3 "event-window API"); payload[1] carries the confounder flags
    // until CR-009 gives them ReasonCode values (spec §6 D4).
    // Research-mode gating belongs to F18: this builder receives only a
    // CandidateOutput (no OperatingMode), so the bit is a *request* and F18
    // dumps bit0 windows solely in Research mode (spec §6 D3 handshake item).
    std::memset(event.payload, 0, sizeof(event.payload));
    event.payload[kPayloadWindowFlagIndex] = kWindowRequestFlag;
    event.payload[kPayloadConfounderIndex] = candidate.confounders;

    event.quality = candidate.quality;
    event.type = EventType::SuspectedCataplexy;
    event.label = UserLabel::Unlabeled;
    event.context = candidate.context;
    event.alertness_at_start = candidate.alertness_at_start;
    event.detector_id = kCataplexyDetectorId;
    event.schema_version = Event::kSchemaVersion;
    return event;
}

}  // namespace bw::cataplexy
