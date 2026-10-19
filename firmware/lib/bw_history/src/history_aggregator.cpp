// bw/history/src/history_aggregator.cpp — F16 device-side aggregation
// (spec §6 D2–D10). Pure logic: no FreeRTOS/Arduino calls, no heap (00 §7.2).

#include <bw/history/history_aggregator.h>

#include <cstring>

namespace bw::history {

namespace {

// [DERIVED] Saturating arithmetic: durations are uint32 ms and a pathological
// episode could overflow a running total; saturating keeps the record honest
// instead of wrapping to a fabricated small value (00 §1: never fabricate).
uint32_t satAddU32(uint32_t a, uint32_t b) {
    return (UINT32_MAX - a < b) ? UINT32_MAX : a + b;
}

uint8_t satAddU8(uint8_t a, uint8_t b) {
    return static_cast<uint8_t>((255 - a < b) ? 255 : a + b);
}

// Meta blob fields are little-endian bytes — explicit encode/decode so the
// blob layout never depends on host struct padding (02 §1: trivially
// copyable records, but no implicit padding may leak into persistence).
void writeLe32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

uint32_t readLe32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

}  // namespace

HistoryAggregator::HistoryAggregator(const HistoryConfig& cfg, IEventStore* events,
                                     IKeyValueStore* kv, const IClock* clock)
    : cfg_(cfg), events_(events), kv_(kv), clock_(clock) {
    // [USER] Out-of-range hours are a caller contract violation; clamping
    // keeps day keys well-defined instead of mis-keying every day (§8).
    if (cfg_.day_boundary_hour >= kHoursPerDay) {
        cfg_.day_boundary_hour = kDayBoundaryHour;
    }
    if (cfg_.daytime_start_hour >= kHoursPerDay) {
        cfg_.daytime_start_hour = kDaytimeStartHour;
    }
    if (cfg_.daytime_end_hour >= kHoursPerDay) {
        cfg_.daytime_end_hour = kDaytimeEndHour;
    }
}

// ---- boot --------------------------------------------------------------------

void HistoryAggregator::beginBoot() {
    boot_phase_ = BootPhase::Probe;
    booted_ = false;
    ring_count_ = 0;
    open_valid_ = false;
    cursor_ = 0;
    clock_regressions_ = 0;
    last_boot_seq_ = 0;
    boot_anchor_known_ = false;
    boot_anchor_stale_ = false;
    probe_from_ = 1;
    probe_last_boot_id_ = 0;
    probe_max_boot_id_ = 0;
    probe_exhausted_ = false;
    saved_cursor_ = 0;
    saved_count_ = 0;
    slot_index_ = 0;
    meta_ok_ = false;
    rebuild_index_ = 0;
    rebuilt_count_ = 0;
    rebuild_from_ = 0;
    rebuild_loaded_ = DailySummary{};
    rebuild_fresh_ = DailySummary{};
    rebuild_active_ = false;
    rebuild_have_ = false;
    rebuild_range_done_ = false;
    boot_result_ = Err::Ok;
    for (DailySummary& s : ring_) {
        s = DailySummary{};
    }
}

Result<void> HistoryAggregator::boot() {
    if (events_ == nullptr || kv_ == nullptr) {
        return {Err::NotReady};
    }
    if (boot_phase_ == BootPhase::Idle || boot_phase_ == BootPhase::Ready) {
        beginBoot();
    }
    return {Err::Busy};
}

Result<void> HistoryAggregator::advanceBoot() {
    switch (boot_phase_) {
    case BootPhase::Idle:
        beginBoot();
        return {Err::Busy};
    case BootPhase::Probe:
        return probeCurrentBootId();
    case BootPhase::Meta: {
        // One KV operation per call keeps startup restoration bounded.
        uint8_t blob[kMetaBlobBytes] = {};
        const Result<size_t> got = kv_->get(kKvKeyMeta, blob, sizeof(blob));
        if (!got.ok() && got.err != Err::NotFound && got.err != Err::Corrupt) {
            return {got.err};  // retry this exact phase; do not discard persisted state
        }
        meta_ok_ = got.ok() && got.value == kMetaBlobBytes && blob[0] == kMetaSchemaVersion;
        if (meta_ok_) {
            saved_cursor_ = readLe32(blob + 1);
            clock_regressions_ = readLe32(blob + 5);
            saved_count_ = blob[9];
            if (saved_count_ > kDeviceSummaryDays) {
                meta_ok_ = false;
            }
        }
        if (!meta_ok_) {
            // Corrupt/missing meta means cursor loss and a full event rescan.
            ring_count_ = 0;
            cursor_ = 0;
            clock_regressions_ = 0;
            boot_phase_ = BootPhase::Ready;
            booted_ = true;
            return {Err::Ok};
        }
        slot_index_ = 0;
        if (saved_count_ == 0) {
            cursor_ = saved_cursor_;
            ring_count_ = 0;
            rebuild_index_ = 0;
            boot_phase_ = BootPhase::Rebuild;
        } else {
            boot_phase_ = BootPhase::Slots;
        }
        return {Err::Busy};
    }
    case BootPhase::Slots: {
        // Read one slot at a time. A read failure retries without mutating the
        // loaded ring; a corrupt/missing slot discards the whole persisted view.
        uint8_t blob[kSummaryBlobBytes] = {};
        char key[kKvDayKeyBytes] = {};
        dayKvKey(key, sizeof(key), slot_index_);
        const Result<size_t> got = kv_->get(key, blob, sizeof(blob));
        if (!got.ok() && got.err != Err::NotFound && got.err != Err::Corrupt) {
            return {got.err};
        }
        DailySummary summary{};
        const bool decoded = got.ok() && decodeSummaryBlob(blob, got.value, summary);
        const bool identity_ok = decoded &&
                                 ((summary.day_key != 0 && summary.boot_id == 0) ||
                                  (summary.day_key == 0 && summary.boot_id != 0)) &&
                                 summary.first_seq <= summary.last_seq;
        if (!identity_ok) {
            for (DailySummary& s : ring_) {
                s = DailySummary{};
            }
            ring_count_ = 0;
            cursor_ = 0;
            clock_regressions_ = 0;
            boot_phase_ = BootPhase::Ready;
            booted_ = true;
            return {Err::Ok};
        }
        ring_[slot_index_] = summary;
        ++slot_index_;
        if (slot_index_ == saved_count_) {
            ring_count_ = saved_count_;
            cursor_ = saved_cursor_;
            rebuild_index_ = ring_count_;
            boot_phase_ = BootPhase::Rebuild;
        }
        return {Err::Busy};
    }
    case BootPhase::Rebuild:
        while (rebuild_index_ > 0 && rebuilt_count_ < kRebuildDays) {
            --rebuild_index_;
            if (ring_[rebuild_index_].day_key == 0) {
                continue;  // session summaries are not rebuilt (D8 concerns labels)
            }
            ++rebuilt_count_;
            rebuild_loaded_ = ring_[rebuild_index_];
            rebuild_fresh_ = DailySummary{};
            rebuild_from_ = rebuild_loaded_.first_seq;
            rebuild_have_ = false;
            rebuild_range_done_ = false;
            rebuild_active_ = true;
            break;
        }
        if (!rebuild_active_) {
            boot_phase_ = BootPhase::Ready;
            booted_ = true;
            return {boot_result_};
        }
        return rebuildDayBatch();
    case BootPhase::Ready:
        return {boot_result_};
    }
    return {Err::HwFault};
}

Result<void> HistoryAggregator::probeCurrentBootId() {
    Event batch[kAggMaxEventsPerTick];
    const Result<size_t> read = events_->readFrom(probe_from_, batch, kAggMaxEventsPerTick);
    if (!read.ok()) {
        // Best effort: unknown anchor means D4 falls back to a session summary,
        // never deriving a calendar date from an untrusted boot mapping.
        boot_anchor_known_ = false;
        boot_phase_ = BootPhase::Meta;
        return {Err::Busy};
    }
    if (read.value == 0) {
        boot_anchor_stale_ = boot_anchor_known_ && probe_last_boot_id_ < probe_max_boot_id_;
        boot_phase_ = BootPhase::Meta;
        return {Err::Busy};
    }
    for (size_t i = 0; i < read.value; ++i) {
        if (batch[i].boot_id > probe_max_boot_id_) {
            probe_max_boot_id_ = batch[i].boot_id;
        }
        if (batch[i].type == EventType::Boot) {
            current_boot_id_ = batch[i].boot_id;
            probe_last_boot_id_ = batch[i].boot_id;
            last_boot_seq_ = batch[i].seq;
            boot_anchor_known_ = true;
        }
        if (batch[i].seq == kSeqMax) {
            probe_exhausted_ = true;
            break;
        }
        if (batch[i].seq + 1 > probe_from_) {
            probe_from_ = batch[i].seq + 1;
        }
    }
    if (probe_exhausted_) {
        boot_anchor_stale_ = boot_anchor_known_ && probe_last_boot_id_ < probe_max_boot_id_;
        boot_phase_ = BootPhase::Meta;
    }
    return {Err::Busy};
}

Result<void> HistoryAggregator::rebuildDayBatch() {
    if (rebuild_range_done_ || rebuild_from_ > rebuild_loaded_.last_seq) {
        if (rebuild_have_ && rebuild_fresh_.day_key == rebuild_loaded_.day_key &&
            rebuild_fresh_.boot_id == rebuild_loaded_.boot_id) {
            ring_[rebuild_index_] = rebuild_fresh_;
        }
        rebuild_active_ = false;
        return {Err::Busy};
    }

    Event batch[kAggMaxEventsPerTick];
    const Result<size_t> read = events_->readFrom(rebuild_from_, batch, kAggMaxEventsPerTick);
    if (!read.ok()) {
        // D8 rebuild is best effort: keep the loaded summary and finish startup,
        // while preserving the failure in historyBootStatus().
        boot_result_ = read.err;
        rebuild_range_done_ = true;
        return {Err::Busy};
    }
    if (read.value == 0) {
        rebuild_range_done_ = true;  // evicted/incomplete range: keep loaded row
        return {Err::Busy};
    }

    for (size_t i = 0; i < read.value; ++i) {
        const Event& event = batch[i];
        if (event.seq > rebuild_loaded_.last_seq) {
            rebuild_range_done_ = true;
            break;
        }
        if (event.seq == kSeqMax) {
            rebuild_range_done_ = true;
        } else if (event.seq + 1 > rebuild_from_) {
            rebuild_from_ = event.seq + 1;
        }
        if (!isCounting(event)) {
            continue;  // Boot events do not alter the sticky current boot id.
        }
        bool derived = false;
        UnixSeconds start_unix = 0;
        const Identity identity = identityFor(event, derived, start_unix);
        if (!rebuild_have_) {
            rebuild_fresh_.day_key = identity.day_key;
            rebuild_fresh_.boot_id = identity.boot_id;
            rebuild_have_ = true;
        }
        accumulate(rebuild_fresh_, event, derived, start_unix);
        if (event.seq == rebuild_loaded_.last_seq) {
            rebuild_range_done_ = true;
        }
    }
    if (rebuild_range_done_) {
        if (rebuild_have_ && rebuild_fresh_.day_key == rebuild_loaded_.day_key &&
            rebuild_fresh_.boot_id == rebuild_loaded_.boot_id) {
            ring_[rebuild_index_] = rebuild_fresh_;
        }
        rebuild_active_ = false;
    }
    return {Err::Busy};
}
// ---- tick --------------------------------------------------------------------

Result<void> HistoryAggregator::step() {
    if (events_ == nullptr || kv_ == nullptr) {
        return {Err::NotReady};
    }
    if (!booted_) {
        // Boot restore shares this caller-paced task, one bounded phase per
        // tick. A failed KV operation leaves the phase unchanged for retry.
        if (boot_phase_ == BootPhase::Idle) {
            beginBoot();
        }
        return advanceBoot();
    }
    if (cursor_ == kSeqMax) {
        // Seq exhaustion: cursor_ + 1 would wrap to 0 and replay the whole
        // log (double counting) — the same early-out the probe and rebuild
        // readers apply for an event at kSeqMax (§10).
        return {Err::Ok};
    }

    Event batch[kAggMaxEventsPerTick];
    const Result<size_t> read = events_->readFrom(cursor_ + 1, batch, kAggMaxEventsPerTick);
    if (!read.ok()) {
        return {read.err};  // §10: state unchanged, retried next tick
    }

    for (size_t i = 0; i < read.value; ++i) {
        const Event& e = batch[i];

        if (e.type == EventType::Boot) {
            // Timeline anchor of the current boot (§6 D4): learned as the
            // batch is drained in seq order, so every later event of this
            // boot can be wall-time derived. The seq is recorded for the
            // identityFor() guard — events at/before the anchor are older
            // boots and must never clock-derive.
            current_boot_id_ = e.boot_id;
            last_boot_seq_ = e.seq;
            boot_anchor_known_ = true;
            cursor_ = e.seq;
            continue;
        }
        if (!isCounting(e)) {
            cursor_ = e.seq;  // ignored types never open a day (§10: no
            continue;         // fabricated empty summaries)
        }

        bool derived = false;
        UnixSeconds start_unix = 0;
        const Identity id = identityFor(e, derived, start_unix);

        if (open_valid_ && !sameIdentity(open_, id)) {
            const Result<void> f = finalizeOpen();
            if (!f.ok()) {
                return f;  // e stays unconsumed; the finalize retries next tick
            }
        }
        if (!open_valid_) {
            // §6 D9: a backward day-key jump opens a new slot with the new
            // key (the previous one was finalized just above) and bumps the
            // persisted clock_regressions counter. The counter is written
            // IMMEDIATELY through the same meta write path as cursor/count,
            // so a regression on the final tick before power-down is not
            // lost (§6 D9 / audit A4). Forward jumps simply open the new
            // day — no synthetic empty days (§10).
            if (!id.session) {
                const DailySummary* ref = newestDayRef();
                if (ref != nullptr && ref->day_key != 0 && id.day_key < ref->day_key) {
                    ++clock_regressions_;
                    const Result<void> reg = persistMeta(ring_count_);
                    if (!reg.ok()) {
                        --clock_regressions_;  // §10: RAM == disk again; e retries
                        return reg;
                    }
                }
            }
            open_ = DailySummary{};
            open_.day_key = id.day_key;
            open_.boot_id = id.boot_id;
            open_valid_ = true;
        }
        accumulate(open_, e, derived, start_unix);
        cursor_ = e.seq;
    }

    // KV writes happen only inside finalizeOpen(): between finalizes the
    // open day is replayed on reboot anyway (persisted cursor = its
    // first_seq - 1), so persisting the cursor more often would buy nothing.
    return {Err::Ok};
}

// ---- accumulation ------------------------------------------------------------

bool HistoryAggregator::isCounting(const Event& e) {
    if (e.type == EventType::NapCompleted) {
        return true;
    }
    if (e.type != EventType::SleepEpisode) {
        return false;
    }
    // §6 D7: only F08's producer (detector 8, mirrored as kSleepDetectorIdMirror
    // from bw/sleep/sleep_config.h) has the payload layout this record decodes —
    // a future second SleepEpisode producer must not be double-counted here.
    if (e.detector_id != kSleepDetectorIdMirror) {
        return false;
    }
    // §6 D7: open markers are incomplete (their close event will arrive
    // later); only closed SleepEpisode events aggregate.
    return (e.payload[kSleepPayloadFlagsIndex] & kSleepFlagOpen) == 0;
}

uint32_t HistoryAggregator::dayKeyFor(UnixSeconds unix) const {
    // §6 D5: logical day = [kDayBoundaryHour, kDayBoundaryHour + 24 h), so
    // the key numbers the calendar day the logical day ENDS on — the shift
    // is (24 - kDayBoundaryHour) hours: day_key = floor((unix + 20 h) / 86400)
    // for the 04:00 boundary. (The spec's D5 formula carried a PLAN-stage
    // sign slip — "+ kDayBoundaryHour * 3600" yields [20:00, 20:00) days;
    // corrected in the spec at IMPLEMENT time, semantic rule unchanged.)
    const uint64_t shifted =
        static_cast<uint64_t>(unix) + static_cast<uint64_t>(kHoursPerDay - cfg_.day_boundary_hour) *
                                          static_cast<uint64_t>(kSecPerHour);
    return static_cast<uint32_t>(shifted / kSecPerDay);
}

HistoryAggregator::Identity HistoryAggregator::identityFor(const Event& e, bool& derived,
                                                           UnixSeconds& resolved_unix) const {
    derived = false;
    resolved_unix = e.t_start_unix;
    if (resolved_unix != 0) {
        return Identity{false, dayKeyFor(resolved_unix), 0};
    }

    // §6 D4(c): derive from IClock only inside the boot that produced the
    // event — t_start_ms is monotonic WITHIN that boot's timeline, so any
    // other boot's event must not be mapped through the current clock (that
    // would fabricate a day, §10). Three guards make the anchor trustworthy:
    //  - boot_anchor_known_: without any Boot event (or after a failed probe)
    //    the boot context is unknown — no derived day;
    //  - !boot_anchor_stale_: if the newest boot_id in the log has no Boot
    //    event, current_boot_id_ describes an OLDER boot — no derived day;
    //  - e.seq > last_boot_seq_: an event at/before the last Boot event
    //    belongs to an earlier boot regardless of its boot_id — no derived
    //    day (audit A2's prescribed seq guard).
    if (boot_anchor_known_ && !boot_anchor_stale_ && current_boot_id_ != 0 &&
        e.boot_id == current_boot_id_ && e.seq > last_boot_seq_ && clock_ != nullptr) {
        const UnixSeconds now_unix = clock_->unixSeconds();
        const TimeMs now_ms = clock_->nowMs();
        if (now_unix != 0 && now_ms >= e.t_start_ms) {
            const uint64_t elapsed_s = static_cast<uint64_t>(now_ms - e.t_start_ms) / kMsPerSec;
            if (elapsed_s < now_unix) {
                resolved_unix = static_cast<UnixSeconds>(now_unix - elapsed_s);
                derived = true;
                return Identity{false, dayKeyFor(resolved_unix), 0};
            }
        }
    }
    // §6 D4(a)/§10: flagged per-boot session summary — never a guessed date.
    resolved_unix = 0;
    return Identity{true, 0, e.boot_id};
}

bool HistoryAggregator::sameIdentity(const DailySummary& s, const Identity& id) {
    return id.session ? (s.day_key == 0 && s.boot_id == id.boot_id)
                      : (s.day_key == id.day_key && s.boot_id == 0);
}

bool HistoryAggregator::sameIdentity(const DailySummary& a, const DailySummary& b) {
    return a.day_key == b.day_key && a.boot_id == b.boot_id;
}

void HistoryAggregator::accumulate(DailySummary& s, const Event& e, bool derived,
                                   UnixSeconds start_unix) const {
    if (derived) {
        s.flags = static_cast<uint8_t>(s.flags | kFlagWallTimeDerived);
    }
    if (s.first_seq == 0 || e.seq < s.first_seq) {
        s.first_seq = e.seq;
    }
    if (e.seq > s.last_seq) {
        s.last_seq = e.seq;
    }

    if (e.type == EventType::NapCompleted) {
        s.nap_count = satAddU8(s.nap_count, 1);
        return;
    }

    // Closed SleepEpisode (§6 D7 mirror of F08's payload layout).
    const bool recovered = (e.payload[kSleepPayloadFlagsIndex] & kSleepFlagRecovered) != 0;
    s.episode_count = satAddU8(s.episode_count, 1);
    if (e.label == UserLabel::Confirmed) {
        s.confirmed_count = satAddU8(s.confirmed_count, 1);  // §6 D7 (labels)
    }
    if (recovered) {
        // §6 D7: recovered episodes count as Uncertain with 0 ms — their
        // tail happened where this boot cannot see it; a duration would be
        // a fabricated number (00 §1).
        return;
    }

    const auto cls = static_cast<EpisodeClass>(e.payload[kSleepPayloadClassIndex]);
    s.total_sleep_ms = satAddU32(s.total_sleep_ms, e.duration_ms);
    if (cls == EpisodeClass::MainSleep) {
        s.main_sleep_ms = satAddU32(s.main_sleep_ms, e.duration_ms);
    }
    // §6 D6: daytime window by START time, any class. Session summaries have
    // no wall time (start_unix == 0) and contribute nothing here.
    if (start_unix != 0) {
        const uint32_t hour_of_day = static_cast<uint32_t>(start_unix % kSecPerDay) / kSecPerHour;
        if (hour_of_day >= cfg_.daytime_start_hour && hour_of_day < cfg_.daytime_end_hour) {
            s.daytime_sleep_ms = satAddU32(s.daytime_sleep_ms, e.duration_ms);
        }
    }
}

// ---- persistence -------------------------------------------------------------

const DailySummary* HistoryAggregator::newestDayRef() const {
    for (size_t i = ring_count_; i-- > 0;) {
        if (ring_[i].day_key != 0) {
            return &ring_[i];
        }
    }
    return nullptr;
}

Result<void> HistoryAggregator::persistSlots(const DailySummary* records, uint8_t count) {
    uint8_t blob[kSummaryBlobBytes] = {};
    for (uint8_t i = 0; i < count; ++i) {
        char key[kKvDayKeyBytes] = {};
        dayKvKey(key, sizeof(key), i);
        encodeSummaryBlob(records[i], blob);
        const Result<void> w = kv_->put(key, blob, sizeof(blob));
        if (!w.ok()) {
            // Sequential keys make a partial write benign: the shift-left
            // layout only rewrites records with their left neighbour's old
            // content, so a torn sequence at worst loses the OLDEST day —
            // never duplicates or misorders a record (the meta cursor is
            // still old, so the replay regenerates the missing tail).
            return w;
        }
    }
    return {Err::Ok};
}

Result<void> HistoryAggregator::persistMeta(uint8_t count) {
    // Cursor semantics (D3): everything at or before the cursor is reflected
    // in the persisted slots; the open day is NOT persisted, so a replay
    // restarts at its first event. Callers guarantee no open day is mid-
    // accumulate here (finalizeOpen() clears open_valid_ before this runs).
    // `count` is the number of slots persistSlots() just wrote — NOT
    // ring_count_, which is only updated after this write succeeds (using it
    // here would persist a count one too small and silently drop the newest
    // finalized day on reboot; regression pinned by T4's reboot test).
    const uint32_t replay_cursor = open_valid_ ? open_.first_seq - 1 : cursor_;
    uint8_t blob[kMetaBlobBytes] = {};
    blob[0] = kMetaSchemaVersion;
    writeLe32(blob + 1, replay_cursor);
    writeLe32(blob + 5, clock_regressions_);
    blob[9] = count;
    return kv_->put(kKvKeyMeta, blob, sizeof(blob));
}

Result<void> HistoryAggregator::finalizeOpen() {
    // Build the next ring layout in staging, persist it, and only then
    // mutate RAM: a KV failure leaves the open day open and the whole
    // finalize retries next tick (§10) — no event is double-counted.
    DailySummary next[kDeviceSummaryDays];
    uint8_t next_count = 0;
    for (size_t i = 0; i < ring_count_; ++i) {
        next[i] = ring_[i];
    }
    next_count = ring_count_;

    // Crash-window dedupe (§10 + DoD §14): if a previous finalize wrote the
    // slot but lost the meta write, the replayed day carries the same
    // identity AND the same first_seq — replace it instead of appending a
    // duplicate. A different first_seq keeps D9's "two slots may share a
    // day_key" behavior intact.
    if (next_count > 0 && sameIdentity(next[next_count - 1], open_) &&
        next[next_count - 1].first_seq == open_.first_seq) {
        next[next_count - 1] = open_;
    } else if (next_count < kDeviceSummaryDays) {
        next[next_count] = open_;
        ++next_count;
    } else {
        // §8 kDeviceSummaryDays ring: evict the oldest, append the newest.
        for (size_t i = 1; i < kDeviceSummaryDays; ++i) {
            next[i - 1] = next[i];
        }
        next[kDeviceSummaryDays - 1] = open_;
    }

    const Result<void> slots = persistSlots(next, next_count);
    if (!slots.ok()) {
        return slots;  // RAM untouched: retried next tick
    }
    open_valid_ = false;  // the day is now in the slots: cursor may cover it
    const Result<void> meta = persistMeta(next_count);
    if (!meta.ok()) {
        open_valid_ = true;  // slots are written; retry persists meta next tick
        return meta;
    }
    for (size_t i = 0; i < next_count; ++i) {
        ring_[i] = next[i];
    }
    ring_count_ = next_count;
    return {Err::Ok};
}

// ---- read side ---------------------------------------------------------------

size_t HistoryAggregator::summaries(DailySummary* out, size_t max) const {
    if (!booted_ || out == nullptr || max == 0) {
        return 0;
    }
    const size_t n = (ring_count_ < max) ? static_cast<size_t>(ring_count_) : max;
    for (size_t i = 0; i < n; ++i) {
        out[i] = ring_[i];
    }
    return n;
}

bool HistoryAggregator::latestDailySummary(DailySummary& out) const {
    if (!booted_ || ring_count_ == 0) {
        return false;
    }
    out = ring_[ring_count_ - 1];
    return true;
}

}  // namespace bw::history
