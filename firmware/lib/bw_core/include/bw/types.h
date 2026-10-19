#pragma once

// bw/types.h — docs/02_INTERFACES.md §1 (conventions) and shared scalar aliases.
// One header per 02 section, mapped 1:1 (F00 spec §6 D4). Contract semantics live
// in 02; this file only carries them as code.

#include <cstddef>
#include <cstdint>

namespace bw {

// Monotonic milliseconds since boot; 0 = boot (02 §1).
// uint64 so it never wraps in this product's lifetime (01 §4, F00 D6).
using TimeMs = uint64_t;

// Wall-clock seconds; 0 = unknown until the phone syncs the time (02 §1, DEC-009).
// There is no battery-backed RTC (00 §3), so features must handle 0 explicitly.
using UnixSeconds = uint32_t;

// Probability/confidence values are float in [0,1] (02 §1).
// "Not available" is a validity flag or DataQuality::Insufficient, never a magic
// number (02 §1) — see bw/quality.h and the `valid` fields of the contract structs.

}  // namespace bw
