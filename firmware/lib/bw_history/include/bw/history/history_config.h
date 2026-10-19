#pragma once

// bw/history/history_config.h — F16's named constants (spec §8, decisions
// D5–D7). Every value carries a tag and a rationale (00 §7.5); nothing here
// is a magic number.

#include <cstddef>
#include <cstdint>

namespace bw::history {

// ---- logical-day geometry (§6 D5, D6) ---------------------------------------

// [USER] Why 4: a "day" is [04:00, next 04:00) so night sleep starting at
// 23:30 belongs to the day that ends at 04:00 (D5). Runtime-tunable later
// via F18 settings; default [ASSUMED] per D6.
inline constexpr uint8_t kDayBoundaryHour = 4;

// [USER] Why 9/21: D6's daytime-sleep window [09:00, 21:00), start-time
// attribution. Clinical validity of the window is out of scope (DEC-019); it
// is a mechanical, replaceable definition behind these two constants.
// [ASSUMED] defaults, tunable via F18 settings later.
inline constexpr uint8_t kDaytimeStartHour = 9;
inline constexpr uint8_t kDaytimeEndHour = 21;

// [DERIVED] Why 86400/3600/24/1000: pure day-key arithmetic (D5);
// day_key = floor((unix + (kHoursPerDay - kDayBoundaryHour) * kSecPerHour)
// / kSecPerDay) — the shift numbers a logical day by the calendar day it
// ENDS on, so [04:00, 04:00) days come out as consecutive keys. The §6 D4
// clock derivation converts monotonic ms to seconds with kMsPerSec.
inline constexpr uint32_t kSecPerDay = 86400;
inline constexpr uint32_t kSecPerHour = 3600;
inline constexpr uint32_t kMsPerSec = 1000;
inline constexpr uint8_t kHoursPerDay = 24;

// [DERIVED] Why UINT32_MAX: F17 assigns monotonic seqs starting at 1, so
// UINT32_MAX is the exhaustion sentinel — a cursor at it must not advance
// via cursor + 1 (that wraps to 0 and replays the whole log; spec §10).
inline constexpr uint32_t kSeqMax = UINT32_MAX;

// ---- ring and tick geometry (§8, §11 budget) --------------------------------

// [EXPERIMENTAL] Why 7: the device keeps a rolling week of summaries; the
// phone holds the long history (D1). Bounds flash (7 x 33 B) and RAM.
inline constexpr size_t kDeviceSummaryDays = 7;

// [DERIVED] Why 8: 8 x 48 B = 384 B batch keeps the tick's stack use inside
// the §11 budget (< 1 KB total); the event rate is a few dozen per day, so
// the log drains within ~1 h of ticks after a backlog.
inline constexpr size_t kAggMaxEventsPerTick = 8;

// [DERIVED] Why 60000: aggregation is not latency-critical; one tick per
// minute bounds the worst-case staleness of the open day at negligible CPU.
inline constexpr uint32_t kAggTickPeriodMs = 60000;

// [DERIVED] Why 2: two rebuilt days cover the realistic confirmation delay
// (D8: F14 labels arrive minutes-to-hours after the close) at trivial boot
// cost. Older days keep the limitation documented in D8 — the companion
// recompute from synced events (with labels) is authoritative.
inline constexpr size_t kRebuildDays = 2;

// ---- persistence geometry (§6 D2, D3) --------------------------------------

// [DERIVED] Why "f16.meta"/"f16.day.N": F18 KV keys, <= 32 chars, charset
// [A-Za-z0-9_.-] (F18 §kKvKeyMaxLen). One meta blob + one blob per ring slot.
inline constexpr const char* kKvKeyMeta = "f16.meta";

// [DERIVED] Why 10: "f16.day." (8) + one digit + NUL; the ring is bounded at
// kDeviceSummaryDays = 7, so one digit always suffices.
inline constexpr size_t kKvDayKeyBytes = 10;

inline void dayKvKey(char* out, size_t cap, uint8_t slot) {
    static const char kPrefix[] = "f16.day.";
    if (out == nullptr || cap < kKvDayKeyBytes) {
        return;
    }
    size_t n = 0;
    for (; kPrefix[n] != '\0'; ++n) {
        out[n] = kPrefix[n];
    }
    out[n++] = static_cast<char>('0' + (slot % 10));
    out[n] = '\0';
}

// [DERIVED] Why 10: meta blob = 1 schema byte + cursor (4) + clock_regressions
// (4) + ring count (1). Unknown schema or wrong size ⇒ discard (§10).
inline constexpr size_t kMetaBlobBytes = 10;
inline constexpr uint8_t kMetaSchemaVersion = 1;

// ---- SleepEpisode payload decode (§6 D7 mirror) -----------------------------

// [DERIVED] Why 0/1/2: F08's payload layout mirrored — F16 must not include
// bw_sleep headers (01 §1.1), so the indices/flag bits are duplicated here
// with a comment citing bw/sleep/sleep_config.h (kPayloadFlagsIndex/
// ClassIndex/ShareIndex, kFlagOpenMarker 0x01, kFlagRecovered 0x02).
// [ASSUMED] drift item: if F08's layout ever changes, H9 review catches it.
inline constexpr uint8_t kSleepPayloadFlagsIndex = 0;
inline constexpr uint8_t kSleepPayloadClassIndex = 1;
inline constexpr uint8_t kSleepPayloadShareIndex = 2;
inline constexpr uint8_t kSleepFlagOpen = 0x01;
inline constexpr uint8_t kSleepFlagRecovered = 0x02;

// [ASSUMED] Why 8: detector id mirrored from bw/sleep/sleep_config.h
// (SleepEpisodeConfig::kSleepEpisodeDetectorId == 8, pinned there by a
// static_assert against the detector registry). The SleepEpisode payload
// this record decodes is F08's layout, so only detector 8's SleepEpisode
// events may aggregate — a future second producer must not be
// double-counted with mis-decoded payload bytes (spec §6 D7). Same mirror
// pattern as the payload constants above; drift is an H9 review item.
inline constexpr uint8_t kSleepDetectorIdMirror = 8;

}  // namespace bw::history
