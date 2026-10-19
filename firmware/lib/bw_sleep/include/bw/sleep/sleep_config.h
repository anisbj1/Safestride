#pragma once

// bw/sleep/sleep_config.h — F08 spec §7/§8: the episode tracker's thresholds,
// marker key, detector id and payload-layout bytes as one value struct, passed
// into the tracker so a host test can vary a single value without touching
// globals (§7; 00 §7.5 — no magic numbers live anywhere else).
//
// Every member keeps its §8 name and tag so tests mirror the spec table row by
// row. Values are compile-time starting points ([EXPERIMENTAL]): promoting
// them to [USER] settings later needs a SettingKey CR (deferred to F18/F12).

#include <cstdint>

#include <bw/state.h>
#include <bw/types.h>

namespace bw::sleep {

struct SleepEpisodeConfig {
    // [EXPERIMENTAL] Why: shorter spans are recorded as drowsiness, not sleep.
    TimeMs kMinEpisodeMs = 300000;

    // [EXPERIMENTAL] Why: brief awakenings do not split an episode.
    TimeMs kMergeGapMs = 120000;

    // [EXPERIMENTAL] Why: longer episodes classified as main sleep.
    TimeMs kNapMaxMs = 10800000;

    // [EXPERIMENTAL] Why: more degraded data than this ⇒ class Uncertain (§6 D2
    // row 1). Compared with `>`: a share of exactly the limit still passes.
    float kMaxDegradedShare = 0.5f;

    // [DERIVED] Why: IKeyValueStore::get/put take `const char*` keys (02 §5);
    // the "f08." prefix namespaces F08 inside the shared store (§6 D4).
    const char* kMarkerKey = "f08.open";

    // [DERIVED] Why: Event::detector_id, derived from the convention "detector
    // id = feature number" (§6 D8); the convention itself is [ASSUMED] (§13).
    uint8_t kSleepEpisodeDetectorId = 8;

    // [DERIVED] Why: payload[0] bit — the episode is still open (§6 D7).
    uint8_t kFlagOpenMarker = 0x01;

    // [DERIVED] Why: payload[0] bit — emitted by recover() after a reboot (§6 D7).
    uint8_t kFlagRecovered = 0x02;

    // [DERIVED] Why: first byte of the stored marker record; a mismatch means
    // "ignore the marker" instead of decoding garbage (§6 D4, §8).
    uint8_t kMarkerSchemaVersion = 1;

    // [DERIVED] Byte indices of the §6 D7 payload layout (flags, class, share
    // percent; bytes 3–7 reserved, written as 0). Not thresholds, but they live
    // here so the whole feature-owned layout is defined in one header.
    static constexpr uint8_t kPayloadFlagsIndex = 0;
    static constexpr uint8_t kPayloadClassIndex = 1;
    static constexpr uint8_t kPayloadShareIndex = 2;
};

// Registry pins (audit L-3; §6 D8, §8): detector_id 8 is pre-allocated to F08
// by the integrator's detector registry, and marker schema 1 is the first
// on-disk marker version. Compile-time twins of the Unity pin
// (test_config_registry_literals_pinned) — a drift in these defaults must
// fail the BUILD, not merely one suite.
static_assert(SleepEpisodeConfig{}.kSleepEpisodeDetectorId == 8,
              "detector_id 8 is allocated to F08 (registry, F08 §6 D8); a collision needs a CR");
static_assert(SleepEpisodeConfig{}.kMarkerSchemaVersion == 1,
              "marker schema 1 is the first on-disk version; bumping it needs a deliberate "
              "migration (F08 §8)");

}  // namespace bw::sleep
