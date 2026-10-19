#pragma once

// F16 test support (spec §9): event builders, a fault-injecting store
// wrapper, an independent recompute implementation for the DoD byte-for-byte
// check (T5) and small helpers shared by the suite.
//
// The recomputeDay() below is DELIBERATELY a second, independent
// implementation of the attribution rules (D5/D6/D7): T5 compares it
// byte-for-byte against the aggregator's incremental records, so sharing the
// production accumulate() here would make the check vacuous.

#include <bw/events.h>
#include <bw/history/daily_summary.h>
#include <bw/history/history_aggregator.h>
#include <bw/history/history_config.h>
#include <bw/providers.h>
#include <bw/result.h>
#include <bw/state.h>
#include <bw/testkit/fake_clock.h>
#include <bw/testkit/mem_event_store.h>
#include <bw/testkit/mem_kv_store.h>
#include <bw/types.h>

#include <cstring>

namespace f16_test {

namespace bh = bw::history;

// [USER-boundary mirror] Test-side copy of the §8 defaults — if the firmware
// constants drift, tests below that pin the spec values fail loudly.
constexpr uint8_t kBoundaryHour = 4;
constexpr uint8_t kDaytimeStartHour = 9;
constexpr uint8_t kDaytimeEndHour = 21;

constexpr uint32_t kSleepDetectorId = 8;  // F08's detector id (spec §6 D7)
constexpr uint32_t kSecPerDay = 86400;
constexpr uint32_t kSecPerHour = 3600;
constexpr uint8_t kHoursPerDay = 24;

// Unix instant inside logical day `day_key` at wall-clock (hour, minute).
// day_key = floor((unix + 20 h)/86400) numbers the calendar day the logical
// day ENDS on (D5 corrected formula), so wall 00:00–03:59 of calendar day D
// belongs to logical day D while wall 04:00–23:59 of calendar day D-1 does
// too — the helper reproduces exactly that (spec §6 D5).
constexpr bw::UnixSeconds wallUnix(uint32_t day_key, uint8_t hour, uint8_t minute = 0,
                                   uint8_t second = 0) {
    const uint32_t cal_day = (hour >= kBoundaryHour) ? day_key - 1 : day_key;
    return cal_day * kSecPerDay + hour * kSecPerHour + minute * 60 + second;
}

inline bw::Event baseEvent(bw::EventType type, uint32_t boot_id, bw::UnixSeconds unix,
                           bw::TimeMs t_ms) {
    bw::Event e{};
    e.t_start_ms = t_ms;
    e.boot_id = boot_id;
    e.t_start_unix = unix;
    e.type = type;
    e.schema_version = bw::Event::kSchemaVersion;
    return e;
}

// Closed SleepEpisode (F08 payload mirror, spec §6 D7): flags byte has no
// open bit unless the caller passes extra_flags. `detector_id` defaults to
// F08's id 8 so tests can also build a FOREIGN producer's SleepEpisode
// (spec §6 D7: only detector 8 aggregates).
inline bw::Event sleepClose(bw::UnixSeconds start_unix, bw::TimeMs t_start_ms, uint32_t duration_ms,
                            bw::EpisodeClass cls, uint32_t boot_id = 1,
                            bw::UserLabel label = bw::UserLabel::Unlabeled, uint8_t extra_flags = 0,
                            uint8_t detector_id = kSleepDetectorId) {
    bw::Event e = baseEvent(bw::EventType::SleepEpisode, boot_id, start_unix, t_start_ms);
    e.duration_ms = duration_ms;
    e.detector_id = detector_id;
    e.label = label;
    e.payload[bh::kSleepPayloadFlagsIndex] = extra_flags;  // 0 = closed
    e.payload[bh::kSleepPayloadClassIndex] = static_cast<uint8_t>(cls);
    e.payload[bh::kSleepPayloadShareIndex] = 0;
    return e;
}

// Open-marker SleepEpisode: skipped by aggregation (its close will arrive).
inline bw::Event sleepOpenMarker(bw::UnixSeconds start_unix, bw::TimeMs t_start_ms,
                                 uint32_t boot_id = 1) {
    bw::Event e = baseEvent(bw::EventType::SleepEpisode, boot_id, start_unix, t_start_ms);
    e.detector_id = kSleepDetectorId;
    e.payload[bh::kSleepPayloadFlagsIndex] = bh::kSleepFlagOpen;
    return e;
}

// Recovered SleepEpisode: counts as Uncertain with 0 ms (spec §6 D7).
inline bw::Event sleepRecovered(bw::UnixSeconds start_unix, bw::TimeMs t_start_ms,
                                uint32_t boot_id = 1) {
    return sleepClose(start_unix, t_start_ms, 7200000, bw::EpisodeClass::Uncertain, boot_id,
                      bw::UserLabel::Unlabeled, bh::kSleepFlagRecovered);
}

inline bw::Event napCompleted(uint32_t boot_id, bw::UnixSeconds unix, bw::TimeMs t_ms) {
    bw::Event e = baseEvent(bw::EventType::NapCompleted, boot_id, unix, t_ms);
    return e;
}

inline bw::Event bootEvent(uint32_t boot_id, bw::TimeMs t_ms) {
    return baseEvent(bw::EventType::Boot, boot_id, 0, t_ms);
}

inline bw::Event ignoredDrowsy(bw::UnixSeconds unix, bw::TimeMs t_ms, uint32_t boot_id = 1) {
    return baseEvent(bw::EventType::DrowsinessEvent, boot_id, unix, t_ms);
}

// Append and return the assigned seq (tests run only on success).
template <size_t N>
uint32_t appendEvent(bw::testkit::InMemoryEventStore<N>& store, const bw::Event& e) {
    const bw::Result<uint32_t> r = store.append(e);
    return r.value;
}

// Step until the aggregator's cursor covers `last_seq` (bounded: a hang would
// fail the test, not the run).
inline void drainTo(bw::history::HistoryAggregator& agg, uint32_t last_seq,
                    size_t max_ticks = 256) {
    for (size_t i = 0; i < max_ticks && agg.lastProcessedSeq() < last_seq; ++i) {
        (void)agg.step();
    }
}

// [DERIVED] Why 512 ticks: even a full test event log needs at most one probe
// read per 8 events, one meta read, 7 slot reads, and two 8-event rebuilds per
// day; this cap turns an accidental non-progressing boot phase into a test
// failure instead of an unbounded loop.
constexpr size_t kBootStepTestLimit = 512;

inline bw::Result<void> bootToReady(bw::history::HistoryAggregator& agg) {
    bw::Result<void> result = agg.boot();
    if (!result.ok() && result.err != bw::Err::Busy) {
        return result;
    }
    for (size_t tick = 0; tick < kBootStepTestLimit && !agg.ready(); ++tick) {
        result = agg.step();
        if (!result.ok() && result.err != bw::Err::Busy) {
            return result;
        }
    }
    return agg.ready() ? bw::Result<void>{bw::Err::Ok} : bw::Result<void>{bw::Err::Timeout};
}

template <size_t N> class CountingEventStore final : public bw::IEventStore {
public:
    explicit CountingEventStore(bw::testkit::InMemoryEventStore<N>& inner) : inner_(inner) {}

    bw::Result<uint32_t> append(const bw::Event& event) override { return inner_.append(event); }
    bw::Result<size_t> readFrom(uint32_t from, bw::Event* out, size_t max) override {
        ++read_calls;
        if (max > largest_read_limit) {
            largest_read_limit = max;
        }
        return inner_.readFrom(from, out, max);
    }
    bw::Result<void> setLabel(uint32_t seq, bw::UserLabel label) override {
        return inner_.setLabel(seq, label);
    }
    bw::Result<void> markSyncedThrough(uint32_t seq) override {
        return inner_.markSyncedThrough(seq);
    }
    uint32_t lastSyncedSeq() const override { return inner_.lastSyncedSeq(); }

    size_t read_calls = 0;
    size_t largest_read_limit = 0;

private:
    bw::testkit::InMemoryEventStore<N>& inner_;
};

// IEventStore wrapper that can fail readFrom on demand (T4: §10 error path).
template <size_t N> class FaultyEventStore final : public bw::IEventStore {
public:
    explicit FaultyEventStore(bw::testkit::InMemoryEventStore<N>& inner) : inner_(inner) {}

    bw::Result<uint32_t> append(const bw::Event& e) override { return inner_.append(e); }
    bw::Result<size_t> readFrom(uint32_t from, bw::Event* out, size_t max) override {
        if (fail_reads_) {
            return {bw::Err::BusError, 0};
        }
        return inner_.readFrom(from, out, max);
    }
    bw::Result<void> setLabel(uint32_t seq, bw::UserLabel label) override {
        return inner_.setLabel(seq, label);
    }
    bw::Result<void> markSyncedThrough(uint32_t seq) override {
        return inner_.markSyncedThrough(seq);
    }
    uint32_t lastSyncedSeq() const override { return inner_.lastSyncedSeq(); }

    void failReads(bool fail) { fail_reads_ = fail; }

private:
    bw::testkit::InMemoryEventStore<N>& inner_;
    bool fail_reads_ = false;
};

// IKeyValueStore wrapper that can fail get and/or put on demand (spec §10:
// read errors must be retried failures, write errors must leave the
// aggregator consistent). The key filter lets a test fail ONLY f16.meta's
// put — that is how the "power loss after persistSlots, before persistMeta"
// crash state is built (audit finding 4).
class FaultyKvStore final : public bw::IKeyValueStore {
public:
    explicit FaultyKvStore(bw::testkit::InMemoryKeyValueStore<>& inner) : inner_(inner) {}

    bw::Result<size_t> get(const char* key, void* buf, size_t cap) override {
        if (fail_reads_) {
            return {bw::Err::BusError, 0};
        }
        return inner_.get(key, buf, cap);
    }
    bw::Result<void> put(const char* key, const void* data, size_t len) override {
        if (fail_all_puts_) {
            return {bw::Err::BusError};
        }
        if (fail_put_key_ != nullptr && std::strcmp(key, fail_put_key_) == 0) {
            return {bw::Err::BusError};
        }
        return inner_.put(key, data, len);
    }

    void failReads(bool fail) { fail_reads_ = fail; }
    void failPuts(bool fail) { fail_all_puts_ = fail; }
    // Fail puts for ONE key only (e.g. bh::kKvKeyMeta); nullptr clears.
    void failPutFor(const char* key) { fail_put_key_ = key; }

private:
    bw::testkit::InMemoryKeyValueStore<>& inner_;
    bool fail_reads_ = false;
    bool fail_all_puts_ = false;
    const char* fail_put_key_ = nullptr;
};

// Independent one-shot recompute of ONE day record from raw events over
// [first_seq, last_seq] (DoD §14, T5). Only valid for records built from
// real wall time (t_start_unix != 0) — the T5/T10 fixtures guarantee that.
inline bw::history::DailySummary recomputeDay(bw::IEventStore& store, uint32_t first_seq,
                                              uint32_t last_seq, uint32_t day_key) {
    bw::history::DailySummary out{};
    out.day_key = day_key;
    bw::Event batch[16];
    uint32_t from = first_seq;
    while (from <= last_seq) {
        const bw::Result<size_t> read = store.readFrom(from, batch, 16);
        if (!read.ok() || read.value == 0) {
            break;
        }
        for (size_t i = 0; i < read.value && from <= last_seq; ++i) {
            const bw::Event& e = batch[i];
            if (e.seq > last_seq) {
                break;
            }
            from = e.seq + 1;
            const uint32_t key = static_cast<uint32_t>(
                (static_cast<uint64_t>(e.t_start_unix) +
                 static_cast<uint64_t>(kHoursPerDay - kBoundaryHour) * kSecPerHour) /
                kSecPerDay);  // D5 corrected formula (see wallUnix above)
            if (key != day_key) {
                continue;
            }
            if (e.type == bw::EventType::SleepEpisode) {
                if (e.detector_id != kSleepDetectorId) {
                    continue;  // D7 mirror: only F08's detector 8 aggregates
                }
                if ((e.payload[0] & 0x01u) != 0) {
                    continue;  // open marker
                }
                if (out.first_seq == 0) {
                    out.first_seq = e.seq;
                }
                out.last_seq = e.seq;
                out.episode_count++;
                if (e.label == bw::UserLabel::Confirmed) {
                    out.confirmed_count++;
                }
                if ((e.payload[0] & 0x02u) != 0) {
                    continue;  // recovered: 0 ms
                }
                out.total_sleep_ms += e.duration_ms;
                if (e.payload[1] == static_cast<uint8_t>(bw::EpisodeClass::MainSleep)) {
                    out.main_sleep_ms += e.duration_ms;
                }
                const uint32_t hour = (e.t_start_unix % kSecPerDay) / kSecPerHour;
                if (hour >= kDaytimeStartHour && hour < kDaytimeEndHour) {
                    out.daytime_sleep_ms += e.duration_ms;
                }
            } else if (e.type == bw::EventType::NapCompleted) {
                if (out.first_seq == 0) {
                    out.first_seq = e.seq;
                }
                out.last_seq = e.seq;
                out.nap_count++;
            }
        }
    }
    return out;
}

}  // namespace f16_test
