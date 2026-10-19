#include <bw/research/research_streamer.h>

#include <cstring>
#include <type_traits>

#include <bw/research/line_format.h>
#include <bw/research/raw_tee.h>

namespace bw::research {

ResearchStreamer::ResearchStreamer(bool enabled_by_config) : flag_enabled_(enabled_by_config) {}

void ResearchStreamer::setSources(const StreamSources& sources) {
    sources_ = sources;
}

void ResearchStreamer::setEnabled(bool enabled) {
    // F28 A4 revert guard: this flag is written here (research task / future
    // F11/F24 control path) and read cross-core by SensorTask (core 1) via
    // rawTee() -> active(). A plain bool would be a data race (UB) the
    // single-threaded host harness cannot observe deterministically, so a
    // compile-time check pins the type instead (see the header comment).
    static_assert(std::is_same_v<decltype(flag_enabled_), std::atomic<bool>>,
                  "flag_enabled_ is read cross-core (SensorTask core 1 via rawTee): "
                  "keep it std::atomic<bool> with relaxed ordering (F28 A4)");
    flag_enabled_.store(enabled, std::memory_order_relaxed);
}

bool ResearchStreamer::active() const {
    if (flag_enabled_.load(std::memory_order_relaxed)) {
        return true;
    }
    if (sources_.state == nullptr) {
        return false;
    }
    bw::StateSnapshot snapshot{};
    if (!sources_.state->latest(snapshot)) {
        return false;  // no state yet: not Research, so not active (00 §1)
    }
    return snapshot.mode == bw::OperatingMode::Research;
}

bool ResearchStreamer::appendLine(const char* line, size_t len) {
    if (len == 0 || kLineRingBytes - ring_used_ < len) {
        ++line_drops_;  // F28 §10: counted, never a stall and never a partial line
        return false;
    }
    size_t tail = (ring_head_ + ring_used_) % kLineRingBytes;
    size_t first = kLineRingBytes - tail;
    if (first > len) {
        first = len;
    }
    std::memcpy(ring_ + tail, line, first);
    if (len > first) {
        std::memcpy(ring_, line + first, len - first);
    }
    ring_used_ += len;
    ++rows_emitted_;
    return true;
}

bool ResearchStreamer::produceHeader(bw::TimeMs now_ms) {
    if (kLineRingBytes - ring_used_ < kLineRingReserveBytes) {
        return false;  // retry next tick; last_header_ms_ stays put (D10)
    }
    HeaderInfo info{};
    info.schema_version = kSessionSchemaVersion;
    info.boot_id = sources_.boot_id;
    info.firmware_version = sources_.firmware_version;
    info.imu_drops = sources_.tee != nullptr ? sources_.tee->imuDrops() : 0;
    info.ppg_drops = sources_.tee != nullptr ? sources_.tee->ppgDrops() : 0;
    info.line_drops = line_drops_;
    info.feature_missed = missed_feature_rows_;

    char line[kMaxLineLen];
    const size_t n = formatHeader(line, sizeof(line), info);
    if (n == 0 || !appendLine(line, n)) {
        return false;
    }
    header_emitted_ = true;
    last_header_ms_ = now_ms;
    return true;
}

void ResearchStreamer::produceMotion() {
    if (sources_.motion == nullptr) {
        return;
    }
    // sequence() before get(): if a set() races in between, we may see the row
    // once more next tick — the t_ms de-dup below turns that into a no-op, so
    // the race can neither duplicate nor lose a row.
    const uint32_t seq = sources_.motion->sequence();
    if (seq == last_motion_seq_) {
        return;
    }
    bw::MotionFeatures features{};
    if (!sources_.motion->get(features)) {
        return;
    }
    if (have_motion_ && features.t_ms == last_motion_t_ms_) {
        last_motion_seq_ = seq;  // same window seen again (raced set): not a row
        return;
    }
    char line[kMaxLineLen];
    const size_t n = formatMotionFeatures(line, sizeof(line), features);
    if (n == 0 || kLineRingBytes - ring_used_ < kLineRingReserveBytes || !appendLine(line, n)) {
        return;  // ring nearly full: value stays unread-by-this-tick, retried
    }
    if (have_motion_) {
        const uint32_t delta = seq - last_motion_seq_;
        if (delta > 1) {
            missed_feature_rows_ += delta - 1;  // overwritten before we read
        }
    }
    last_motion_seq_ = seq;
    last_motion_t_ms_ = features.t_ms;
    have_motion_ = true;
}

void ResearchStreamer::producePpg() {
    if (sources_.ppg == nullptr) {
        return;
    }
    const uint32_t seq = sources_.ppg->sequence();
    if (seq == last_ppg_seq_) {
        return;
    }
    bw::PpgFeatures features{};
    if (!sources_.ppg->get(features)) {
        return;
    }
    if (have_ppg_ && features.t_ms == last_ppg_t_ms_) {
        last_ppg_seq_ = seq;
        return;
    }
    char line[kMaxLineLen];
    const size_t n = formatPpgFeatures(line, sizeof(line), features);
    if (n == 0 || kLineRingBytes - ring_used_ < kLineRingReserveBytes || !appendLine(line, n)) {
        return;
    }
    if (have_ppg_) {
        const uint32_t delta = seq - last_ppg_seq_;
        if (delta > 1) {
            missed_feature_rows_ += delta - 1;
        }
    }
    last_ppg_seq_ = seq;
    last_ppg_t_ms_ = features.t_ms;
    have_ppg_ = true;
}

void ResearchStreamer::produceActivity() {
    if (sources_.activity == nullptr) {
        return;
    }
    const uint32_t seq = sources_.activity->sequence();
    if (seq == last_activity_seq_) {
        return;
    }
    bw::ActivityEstimate estimate{};
    if (!sources_.activity->get(estimate)) {
        return;
    }
    if (have_activity_ && estimate.t_ms == last_activity_t_ms_) {
        last_activity_seq_ = seq;
        return;
    }
    char line[kMaxLineLen];
    const size_t n = formatContext(line, sizeof(line), estimate);
    if (n == 0 || kLineRingBytes - ring_used_ < kLineRingReserveBytes || !appendLine(line, n)) {
        return;
    }
    if (have_activity_) {
        const uint32_t delta = seq - last_activity_seq_;
        if (delta > 1) {
            missed_feature_rows_ += delta - 1;
        }
    }
    last_activity_seq_ = seq;
    last_activity_t_ms_ = estimate.t_ms;
    have_activity_ = true;
}

void ResearchStreamer::produceRaw() {
    if (sources_.tee == nullptr) {
        return;
    }
    // One row per call so the loop's ring-reserve check stays valid for every
    // row. IMU first, then PPG; the two kinds land in different session files,
    // so the interleaving order carries no meaning for the host (D6).
    bw::ImuSample sample{};
    if (sources_.tee->popImu(sample)) {
        char line[kMaxLineLen];
        const size_t n = formatImu(line, sizeof(line), sample);
        if (n == 0) {
            ++line_drops_;  // defensive: a raw row longer than kMaxLineLen
            return;
        }
        appendLine(line, n);  // failure counted inside (already popped: the loss is real)
        return;
    }
    bw::PpgSample ppg{};
    if (sources_.tee->popPpg(ppg)) {
        char line[kMaxLineLen];
        const size_t n = formatPpg(line, sizeof(line), ppg);
        if (n == 0) {
            ++line_drops_;
            return;
        }
        appendLine(line, n);
    }
}

void ResearchStreamer::baselineEvents() {
    // D8: a session contains events that happen INSIDE it. Scan forward once
    // at activation and start the cursor after everything already stored —
    // capped by kEventBaselineMaxEvents so activation cannot stall (§13).
    events_baselined_ = true;
    if (sources_.events == nullptr) {
        return;
    }
    bw::Event batch[kEventPollBatch];
    size_t scanned = 0;
    uint32_t highest = event_cursor_;
    while (scanned < kEventBaselineMaxEvents) {
        const Result<size_t> read = sources_.events->readFrom(highest + 1, batch, kEventPollBatch);
        if (!read.ok() || read.value == 0) {
            break;
        }
        for (size_t i = 0; i < read.value; ++i) {
            if (batch[i].seq > highest) {
                highest = batch[i].seq;
            }
        }
        scanned += read.value;
        if (read.value < kEventPollBatch) {
            break;
        }
    }
    event_cursor_ = highest;
}

void ResearchStreamer::produceEvents() {
    if (sources_.events == nullptr || !events_baselined_) {
        return;
    }
    bw::Event event{};
    const Result<size_t> read = sources_.events->readFrom(event_cursor_ + 1, &event, 1);
    if (!read.ok() || read.value == 0) {
        return;  // no new event
    }
    char line[kMaxLineLen];
    const size_t n = formatEvent(line, sizeof(line), event);
    if (n == 0) {
        ++line_drops_;
        return;
    }
    if (kLineRingBytes - ring_used_ < kLineRingReserveBytes || !appendLine(line, n)) {
        return;  // ring nearly full: cursor unchanged, retried next tick
    }
    event_cursor_ = event.seq;  // advanced only once the row is actually out
    if (event.type == bw::EventType::UserMarked) {
        char marker[kMaxLineLen];
        const size_t m = formatMarker(marker, sizeof(marker), event.t_start_ms, event.seq);
        if (m == 0) {
            ++line_drops_;
        } else if (!appendLine(marker, m)) {
            // counted in line_drops_; the E row for the same event is already
            // out, so only the marker row is lost (visible as a gap in
            // markers.csv, never as a wrong value).
        }
    }
}

void ResearchStreamer::produceTap(Tap tap) {
    switch (tap) {
    case Tap::Raw:
        produceRaw();
        break;
    case Tap::Motion:
        produceMotion();
        break;
    case Tap::Ppg:
        producePpg();
        break;
    case Tap::Activity:
        produceActivity();
        break;
    case Tap::Events:
        produceEvents();
        break;
    case Tap::Count:
    default:
        break;  // not a producer; guarded so adding a Tap cannot fall through
    }
}

void ResearchStreamer::runTick(bw::TimeMs now_ms) {
    const bool on = active();
    if (sources_.tee != nullptr) {
        sources_.tee->setEnabled(on);  // single sync point: the producer-side
    }  // push is gated by the same D4 condition
    if (!on) {
        was_active_ = false;  // next activation re-emits #HDR and re-baselines
        return;
    }
    if (!was_active_) {
        was_active_ = true;
        have_motion_ = false;
        have_ppg_ = false;
        have_activity_ = false;
        last_motion_seq_ = 0;
        last_ppg_seq_ = 0;
        last_activity_seq_ = 0;
        next_tap_ = 0;  // each session starts round-robin at Raw (normal order)
        baselineEvents();
        produceHeader(now_ms);
    } else if (now_ms >= last_header_ms_ && now_ms - last_header_ms_ >= kHeaderRepeatMs) {
        produceHeader(now_ms);
    }

    // Bounded production (kMaxLinesPerTick) and pause-on-back-pressure
    // (kLineRingReserveBytes): rows the ring cannot take stay where they are —
    // in the mailbox, in the tee queues or in the event store — and are either
    // retried or counted as missed/dropped next time (F28 §10).
    //
    // Fairness (F2, fixture root cause): each pass starts at next_tap_ and
    // walks the five producers in round-robin; the cursor stops one past the
    // tap that emitted. The previous strict order (produceRaw first, always)
    // let a tee that never empties win every pass, so Motion/Ppg/Activity/
    // Events were never even attempted (0 F rows in the 10-minute capture,
    // feature_missed stayed 0 because no row was ever emitted). Round-robin
    // puts every producer at the head of a pass within ≤ Tap::Count passes,
    // while a pending raw row still wins every pass it is tried in — same
    // kMaxLinesPerTick budget, same ring reserve, so raw throughput does not
    // regress (test_fairness.cpp pins both properties).
    size_t produced = 0;
    while (produced < kMaxLinesPerTick && kLineRingBytes - ring_used_ >= kLineRingReserveBytes) {
        const size_t before = rows_emitted_;
        for (size_t k = 0; k < static_cast<size_t>(Tap::Count); ++k) {
            const size_t tap =
                (static_cast<size_t>(next_tap_) + k) % static_cast<size_t>(Tap::Count);
            produceTap(static_cast<Tap>(tap));
            if (rows_emitted_ != before) {
                ++produced;
                next_tap_ = static_cast<uint8_t>((tap + 1) % static_cast<size_t>(Tap::Count));
                break;
            }
        }
        if (rows_emitted_ == before) {
            break;  // no stream has a new value this tick
        }
    }
}

size_t ResearchStreamer::drain(char* out, size_t max) {
    if (max == 0 || ring_used_ == 0) {
        return 0;
    }
    size_t n = ring_used_ < max ? ring_used_ : max;
    size_t first = kLineRingBytes - ring_head_;
    if (first > n) {
        first = n;
    }
    std::memcpy(out, ring_ + ring_head_, first);
    if (n > first) {
        std::memcpy(out + first, ring_, n - first);
    }
    ring_head_ = (ring_head_ + n) % kLineRingBytes;
    ring_used_ -= n;
    return n;
}

}  // namespace bw::research
