#pragma once

// bw/research/research_streamer.h — F28's device-side capture core (spec §7,
// D3–D10): reads feature/context mailboxes and (optionally) providers, formats
// the rows for the serial line grammar, and buffers them in a fixed line ring.
//
// Pure logic (01 §1 rule 5): no FreeRTOS/Arduino calls, no heap, no clock reads
// — `now` is a parameter. The wiring task calls runTick(now) then drain() and
// writes the bytes to the UART; back-pressure is drop-with-counter (F28 §10),
// never a stall.
//
// §5 gate: rows are produced only while active() is true, i.e. the explicit
// enable flag OR OperatingMode::Research (D4). Default flag comes from
// kResearchCaptureDefaultEnabled (false).

#include <cstddef>
#include <cstdint>

#include <atomic>

#include <bw/containers/latest_value.h>
#include <bw/context.h>
#include <bw/features.h>
#include <bw/providers.h>
#include <bw/research/research_config.h>
#include <bw/types.h>

namespace bw::research {

class RawTee;  // D3 raw tap (include raw_tee.h at the call site)

// Everything the streamer reads, wired once from AppContext (01 §1 rule 3).
// Optional pointers are absent sources: "no data yet" is an ordinary state —
// the row kind is simply not produced, never faked (00 §1).
struct StreamSources {
    const bw::LatestValue<bw::MotionFeatures>* motion = nullptr;
    const bw::LatestValue<bw::PpgFeatures>* ppg = nullptr;
    const bw::LatestValue<bw::ActivityEstimate>* activity = nullptr;
    const bw::IStateProvider* state = nullptr;  // D4 mode gate; null until the
                                                // integrator exposes one (§13)
    // Not const: IEventStore::readFrom is a non-const member (02 §5) — the
    // store may page through flash to satisfy the read.
    bw::IEventStore* events = nullptr;  // D8 event tap; null likewise
    RawTee* tee = nullptr;              // D3 raw rows + drop counters
    const char* firmware_version = nullptr;
    uint32_t boot_id = kUnknownBootId;
};

class ResearchStreamer {
public:
    // enabled_by_config defaults to kResearchCaptureDefaultEnabled so the
    // default-constructed streamer is OFF (§5, gate-default-off test).
    explicit ResearchStreamer(bool enabled_by_config = kResearchCaptureDefaultEnabled);

    void setSources(const StreamSources& sources);

    // Explicit runtime enable (host tests today; F11 EnterResearch / F24 later).
    // Cross-core by design (F28 A4): the flag is written here (research task /
    // a future F11/F24 control path) and read by SensorTask on core 1 through
    // rawTee() -> active(), so it is an atomic relaxed flag. Relaxed is enough
    // because the write publishes a bare value with no associated data, and
    // RawTee::enabled_ uses the same pattern. A revert to plain bool is a data
    // race (UB) that the single-threaded Unity harness cannot observe
    // deterministically, and TSan needs build flags F28 does not own
    // (firmware/platformio.ini is integrator-owned), so the guard is the
    // static_assert in research_streamer.cpp::setEnabled plus the behavioral
    // gate tests in test_streamer.cpp.
    void setEnabled(bool enabled);
    bool enabled() const { return flag_enabled_.load(std::memory_order_relaxed); }

    // D4: flag OR Research mode. Const: reads providers only (state may be null).
    bool active() const;

    // Produces this tick's rows into the line ring. Never blocks; may defer
    // rows to the next tick when the ring is nearly full (drop/pause, §10).
    // Fairness (F2): producers are tried round-robin per pass, so a saturated
    // raw tee can no longer starve the feature/context/event rows.
    void runTick(bw::TimeMs now_ms);

    // Copies up to `max` formatted bytes out of the ring (FIFO). Returns the
    // byte count; 0 = nothing to send. Caller-side, never blocks.
    size_t drain(char* out, size_t max);

    // Diagnostics carried in #HDR (D10) and read by the wiring/tests.
    uint32_t lineDrops() const { return line_drops_; }
    uint32_t missedFeatureRows() const { return missed_feature_rows_; }
    uint64_t rowsEmitted() const { return rows_emitted_; }
    bool headerEmitted() const { return header_emitted_; }

private:
    // Appends one finished line; false (and counted) if it does not fit.
    bool appendLine(const char* line, size_t len);
    // Emits #HDR now (schema/identity/counters) if the ring has room; false =
    // retried on the next tick (last_header_ms_ only advances on success).
    bool produceHeader(bw::TimeMs now_ms);
    // Per-source row production with t_ms de-duplication (see .cpp).
    void produceRaw();
    void produceMotion();
    void producePpg();
    void produceActivity();
    // The five row producers, in the tick loop's round-robin order (F2
    // fairness: a saturated raw tee used to win every pass and starve every
    // feature producer — see runTick()).
    enum class Tap : uint8_t { Raw = 0, Motion, Ppg, Activity, Events, Count };
    void produceTap(Tap tap);
    // D8: one-shot forward scan at activation skips events already stored, so
    // a session contains only events that happen inside it; then emits new
    // events (plus an M row for UserMarked).
    void baselineEvents();
    void produceEvents();

    StreamSources sources_{};
    std::atomic<bool> flag_enabled_{false};  // F28 A4: cross-core (core 1 reads)
    bool was_active_ = false;
    bool events_baselined_ = false;
    uint32_t event_cursor_ = 0;  // highest event seq already handled (0 = none)
    uint8_t next_tap_ = 0;       // fairness cursor: tap tried first next pass

    // #HDR cadence (D10).
    bool header_emitted_ = false;
    bw::TimeMs last_header_ms_ = 0;

    // De-duplication: last emitted t_ms per stream plus the mailbox sequence
    // read for missed-value accounting (diagnostic only — t_ms is what makes a
    // row unique; sequence() alone races with a concurrent set()).
    bw::TimeMs last_motion_t_ms_ = 0;
    bw::TimeMs last_ppg_t_ms_ = 0;
    bw::TimeMs last_activity_t_ms_ = 0;
    uint32_t last_motion_seq_ = 0;
    uint32_t last_ppg_seq_ = 0;
    uint32_t last_activity_seq_ = 0;
    bool have_motion_ = false;
    bool have_ppg_ = false;
    bool have_activity_ = false;

    // Fixed line ring: produced by runTick, consumed by drain(), both on the
    // research task, so plain indices are enough (no atomics needed).
    char ring_[kLineRingBytes]{};
    size_t ring_head_ = 0;  // next byte out
    size_t ring_used_ = 0;  // bytes currently buffered

    uint32_t line_drops_ = 0;
    uint32_t missed_feature_rows_ = 0;
    uint64_t rows_emitted_ = 0;
};

}  // namespace bw::research
