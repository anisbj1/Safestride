#pragma once

// bw/cataplexy/candidate_event.h — F09 §3: the Event a raised candidate turns
// into, built as a pure function so the layout F09 controls is host-testable.
//
// Ownership (02 §4.5/§5): F09 fills what the detector knows and leaves seq and
// boot_id at 0 for F17 to assign; t_start_unix stays 0 because monotonic time
// has no wall-clock mapping until the phone syncs (02 §1, DEC-009).

#include <bw/cataplexy/candidate_detector.h>
#include <bw/events.h>

namespace bw::cataplexy {

// Builds EventType::SuspectedCataplexy from a detector conclusion.
// Precondition: candidate.raised == true — F10 decides about alerts, F09 only
// publishes (spec §5); callers never publish a below-threshold candidate, it
// belongs to the research stream instead (spec §8).
Event makeSuspectedCataplexyEvent(const CandidateOutput& candidate);

}  // namespace bw::cataplexy
