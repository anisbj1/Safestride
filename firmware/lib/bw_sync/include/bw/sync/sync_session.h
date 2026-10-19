#pragma once

// bw/sync/sync_session.h — the F20 sync engine (spec §6 D1/D4, SYNC_PROTOCOL.md §4).
// Pure logic: dependencies injected by constructor-style bind(); step(now) takes
// time explicitly, so the whole state machine runs deterministically on the host
// against a lossy fake transport (D2). The wearable never blocks on the phone and
// never deletes events — errors leave lastSyncedSeq untouched (§5, §10).
//
// Phases (spec §7): Idle -> Streaming -> WaitAck -> (complete => Idle |
// aborted => Idle with the transfer_aborted status flag). One caller only: the
// sync pump (wire_sync::pumpOnce, D6) — the ISyncTransport contract's call
// context (bw/sync_transport.h).

#include <cstdint>

#include <bw/events.h>
#include <bw/providers.h>
#include <bw/result.h>
#include <bw/sync/chunk_packetizer.h>
#include <bw/sync/sync_config.h>
#include <bw/sync_transport.h>
#include <bw/types.h>

namespace bw::sync {

enum class SyncPhase : std::uint8_t { Idle, Streaming, WaitAck };

class SyncSession final : public bw::ISyncStatusProvider {
public:
    // Attaches/detaches both seams. nullptr detaches: any in-flight attempt is
    // aborted (marker untouched) and step() becomes an honest no-op. Resets the
    // per-wiring state (cumulative counters included) so a rewire starts clean.
    void bind(bw::ISyncTransport* transport, bw::IEventStore* store);

    // One bounded pump (D6): drain inbound Control frames, apply acks (D4),
    // pump one store read into the flow-control window, check ack timeouts,
    // flush a pending STATUS. Never blocks; never reads more than
    // kMaxEventsPerRead events.
    bw::Result<void> step(bw::TimeMs now);

    // bw::ISyncStatusProvider (CR-019 part B): false until the first step().
    bool latest(bw::SyncStatus& out) const override;

    // -- diagnostics (host tests + wiring checks; no protocol meaning) --------
    SyncPhase phase() const { return phase_; }
    std::size_t inFlightChunks() const { return window_count_; }
    std::uint32_t eventsSent() const { return events_sent_; }
    std::uint32_t chunksResent() const { return chunks_resent_; }
    std::uint32_t framesDropped() const { return frames_dropped_; }
    std::uint32_t acksIgnored() const { return acks_ignored_; }

private:
    // In-flight chunk bookkeeping: enough to resend the exact same frame after
    // an ack timeout (same chunk_no, §4.2) without re-reading the whole window.
    struct WindowEntry {
        std::uint32_t first_seq = 0;
        std::uint8_t num_events = 0;
        std::uint16_t chunk_no = 0;
        TimeMs sent_at_ms = 0;
    };

    void resetTransfer();
    void startRequest(const CursorRequest& request);
    void applyAck(std::uint32_t ack_seq);
    void drainControlFrames();
    void pumpStreaming(bw::TimeMs now, std::uint16_t usable_bytes);
    void checkAckTimeout(bw::TimeMs now, std::uint16_t usable_bytes);
    void completeTransfer();
    void failAttempt();
    void flushStatus(bw::TimeMs now);
    void publishStatus(bw::TimeMs now);

    // SINGLE source of truth for the STATUS flag byte (SYNC_PROTOCOL.md §3.2):
    // both the outgoing STATUS frame (flushStatus) and the CR-019 provider
    // snapshot (publishStatus) build their flags through this helper, so the
    // two representations can never drift apart (audit F-2).
    std::uint8_t statusFlags() const;
    bw::Result<void> sendOrCount(bw::SyncChannel ch, const std::uint8_t* data, std::uint16_t len);

    bw::ISyncTransport* transport_ = nullptr;
    bw::IEventStore* store_ = nullptr;

    // -- transfer state -------------------------------------------------------
    SyncPhase phase_ = SyncPhase::Idle;
    WindowEntry window_[kMaxUnackedChunks];
    std::size_t window_count_ = 0;
    std::uint32_t next_read_seq_ = 1;  // cursor: next seq to read from the store
    std::uint32_t request_max_ = 0;    // CURSOR.max for the active request; 0 = unbounded
    std::uint32_t sent_this_request_ = 0;
    std::uint16_t next_chunk_no_ = 1;  // per-transfer counter, restarts at 1 (§3.3)
    std::uint8_t timeouts_since_ack_ = 0;
    std::uint8_t send_failures_ = 0;  // consecutive non-Busy send failures

    // -- observed state -------------------------------------------------------
    std::uint32_t last_seq_ = 0;  // newest seq seen in a store read (STATUS.last_seq)
    bool link_up_last_ = false;
    bool mtu_too_low_ = false;
    bool aborted_ = false;      // latched into STATUS bit3 until the next request
    bool status_dirty_ = true;  // a STATUS indication is owed (sent at next opportunity)
    bool have_status_ = false;

    // -- cumulative-this-boot counters (diagnostics, SyncStatus) --------------
    std::uint32_t events_sent_ = 0;
    std::uint32_t chunks_resent_ = 0;
    std::uint32_t frames_dropped_ = 0;  // malformed/unknown/oversize inbound frames (D7)
    std::uint32_t acks_ignored_ = 0;    // duplicate/backward/unknown acks (§4.3)

    bw::SyncStatus status_{};

    // -- static buffers (no heap; spec §11 budget) ---------------------------
    bw::Event read_buf_[kMaxEventsPerRead];    // 768 B
    std::uint8_t tx_[bw::kMaxSyncFrameBytes];  // 252 B staging
    std::uint8_t rx_[bw::kMaxSyncFrameBytes];  // 252 B inbound Control
};

}  // namespace bw::sync
