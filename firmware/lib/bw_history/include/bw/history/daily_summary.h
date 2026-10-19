#pragma once

// bw/history/daily_summary.h — F16's DailySummary output record (spec §6 D2,
// layout pinned in §7: exactly 32 B, trivially copyable).
//
// F16-owned on purpose (D2): only the device aggregator consumes it today;
// the companion computes full history from raw synced events (DoD §14 makes
// that recompute authoritative). CR-020 stays RESERVED, unconsumed — it is
// the moment-to-move trigger if F20 ever syncs device summaries over BLE.

#include <cstdint>
#include <cstring>
#include <type_traits>

namespace bw::history {

struct DailySummary {
    static constexpr uint8_t kSchemaVersion = 1;

    uint32_t day_key;           // floor((unix + (24 - kDayBoundaryHour)*3600)/86400); 0 = session
    uint32_t boot_id;           // session summaries: the boot; calendar days: 0 (§6 D4)
    uint32_t first_seq;         // earliest contributing event — recompute anchor (DoD §14)
    uint32_t last_seq;          // latest contributing event
    uint32_t total_sleep_ms;    // summed duration of counted episodes
    uint32_t daytime_sleep_ms;  // D6 definition: start in [09:00, 21:00)
    uint32_t main_sleep_ms;     // EpisodeClass::MainSleep share of total_sleep_ms
    uint8_t flags;              // bit0 kFlagWallTimeDerived (§6 D4)
    uint8_t episode_count;      // closed SleepEpisode events (recovered -> Uncertain, 0 ms)
    uint8_t nap_count;          // NapCompleted events
    uint8_t confirmed_count;    // counted episodes whose store label == Confirmed
};

// §6 D4: at least one event's wall time was derived from IClock instead of
// t_start_unix — the day key of those events is exact only while both clocks
// stay consistent; the companion recompute is authoritative.
inline constexpr uint8_t kFlagWallTimeDerived = 0x01;

static_assert(sizeof(DailySummary) == 32, "F16 §11 budget: 7*u32 + 4*u8 = 32 B, no padding");
static_assert(std::is_trivially_copyable_v<DailySummary>, "02 §1: queue-safe value type");

// KV blob = 1 schema byte + the 32 B record = kSummaryBlobBytes (33, §8).
inline constexpr size_t kSummaryBlobBytes = 1 + sizeof(DailySummary);

inline void encodeSummaryBlob(const DailySummary& summary, uint8_t* blob) {
    blob[0] = DailySummary::kSchemaVersion;
    std::memcpy(blob + 1, &summary, sizeof(summary));
}

// Wrong size or unknown schema ⇒ false; caller discards the blob (§10).
inline bool decodeSummaryBlob(const uint8_t* blob, size_t len, DailySummary& out) {
    if (blob == nullptr || len != kSummaryBlobBytes || blob[0] != DailySummary::kSchemaVersion) {
        return false;
    }
    std::memcpy(&out, blob + 1, sizeof(out));
    return true;
}

}  // namespace bw::history
