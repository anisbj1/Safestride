#include <bw/sync/sync_session.h>

namespace bw::sync {

void SyncSession::bind(bw::ISyncTransport* transport, bw::IEventStore* store) {
    transport_ = transport;
    store_ = store;
    // Clean slate per wiring (F08 rewire pattern): an in-flight attempt from a
    // previous binding is abandoned — the marker is F18's, untouched by us.
    resetTransfer();
    link_up_last_ = false;
    mtu_too_low_ = false;
    aborted_ = false;
    status_dirty_ = true;
    have_status_ = false;
    last_seq_ = 0;
    events_sent_ = 0;
    chunks_resent_ = 0;
    frames_dropped_ = 0;
    acks_ignored_ = 0;
}

void SyncSession::resetTransfer() {
    phase_ = SyncPhase::Idle;
    window_count_ = 0;
    next_read_seq_ = 1;
    request_max_ = 0;
    sent_this_request_ = 0;
    next_chunk_no_ = 1;
    timeouts_since_ack_ = 0;
    send_failures_ = 0;
}

Result<void> SyncSession::step(TimeMs now) {
    if (transport_ == nullptr || store_ == nullptr) {
        return {Err::NotReady};  // detached: honest no-op until wire() binds both seams
    }
    const bool link = transport_->linkUp();
    const uint16_t usable = link ? transport_->maxSendBytes() : 0;
    const bool link_edge = link && !link_up_last_;

    if (link_up_last_ && !link) {
        // Link-down edge (§4.6, F04): abort the attempt immediately, marker
        // untouched; the owed STATUS goes out on the next link-up edge.
        failAttempt();
    }
    link_up_last_ = link;

    if (link) {
        drainControlFrames();  // acks first: a combined CURSOR acks, then (re)starts
        if (link_edge) {
            status_dirty_ = true;  // §3.2: STATUS on link up
        }
        mtu_too_low_ = (usable < kMinSyncUsableBytes);
        if (mtu_too_low_) {
            failAttempt();  // never stream below the floor; no-op when already Idle
        } else {
            checkAckTimeout(now, usable);  // resend unacked suffix or give up
            if (phase_ == SyncPhase::Streaming) {
                pumpStreaming(now, usable);
            }
        }
        flushStatus(now);
    } else {
        mtu_too_low_ = false;  // only meaningful while the link is up (contract)
    }

    publishStatus(now);
    return {Err::Ok};
}

bool SyncSession::latest(SyncStatus& out) const {
    if (!have_status_) {
        return false;
    }
    out = status_;
    return true;
}

void SyncSession::drainControlFrames() {
    for (size_t drained = 0; drained < kMaxControlFramesPerPump; ++drained) {
        SyncChannel ch = SyncChannel::Control;
        Result<size_t> r = transport_->receive(&ch, rx_, sizeof rx_);
        if (!r.ok()) {
            ++frames_dropped_;  // Err::Overflow: oversized head frame dropped by transport
            continue;
        }
        if (r.value == 0) {
            return;  // queue empty
        }
        if (ch != SyncChannel::Control) {
            ++frames_dropped_;  // phone->device is only legal on Control (§2)
            continue;
        }
        CursorRequest request;
        Result<void> parsed = parseCursorFrame(rx_, r.value, &request);
        if (!parsed.ok()) {
            ++frames_dropped_;  // D7: unknown version/type, wrong length — dropped, no NACK
            continue;
        }
        // §3.1: fields are independent; an ack is applied before a new request
        // so one frame may do both.
        if (request.ack_seq != 0) {
            applyAck(request.ack_seq);
        }
        if (request.from_seq != 0) {
            startRequest(request);  // newest CURSOR is authoritative mid-stream (T13)
        } else if (request.isStatusQuery()) {
            status_dirty_ = true;
        }
    }
}

void SyncSession::applyAck(uint32_t ack_seq) {
    // D4 / §4.3: applied only if monotonic AND present in the store — the
    // exists-check doubles as the head bound (IEventStore has no head query).
    if (ack_seq <= store_->lastSyncedSeq()) {
        ++acks_ignored_;  // duplicate or backward ack
        return;
    }
    Event probe{};
    Result<size_t> exists = store_->readFrom(ack_seq, &probe, 1);
    if (!exists.ok() || exists.value != 1 || probe.seq != ack_seq) {
        ++acks_ignored_;  // unknown seq or beyond store head
        return;
    }
    bw::Result<void> marked = store_->markSyncedThrough(ack_seq);  // once per valid ack (T08)
    (void)marked;  // IEventStore marks monotonically; a refusal would be an F18 bug

    // Slide the window: every chunk starting at or below ack_seq is covered.
    // A partly covered chunk is NOT resent here — the phone's own high-water
    // drives its next request (§4.1), so nothing is lost (D4).
    size_t kept = 0;
    for (size_t i = 0; i < window_count_; ++i) {
        if (window_[i].first_seq > ack_seq) {
            window_[kept++] = window_[i];
        }
    }
    window_count_ = kept;
    timeouts_since_ack_ = 0;
    send_failures_ = 0;

    if (window_count_ == 0 && phase_ == SyncPhase::WaitAck) {
        if (request_max_ > 0 && sent_this_request_ >= request_max_) {
            completeTransfer();  // request cap reached and everything acked
        } else {
            phase_ = SyncPhase::Streaming;  // room again: pump resumes this step
        }
    }
}

void SyncSession::startRequest(const CursorRequest& request) {
    resetTransfer();
    next_read_seq_ = request.from_seq;  // T03: mid-history start honored
    request_max_ = request.max;         // T04: 0 = unbounded-by-request
    aborted_ = false;                   // a fresh request clears the give-up latch
    phase_ = SyncPhase::Streaming;
}

void SyncSession::pumpStreaming(TimeMs now, uint16_t usable_bytes) {
    const size_t per_chunk = recordsPerChunk(usable_bytes);
    if (per_chunk == 0) {
        return;  // defensive: step() already gates on the MTU floor
    }
    // One store read per pump, bounded by kMaxEventsPerRead (spec §11 budget).
    Result<size_t> read = store_->readFrom(next_read_seq_, read_buf_, kMaxEventsPerRead);
    if (!read.ok()) {
        failAttempt();  // §4.7: store read error abandons the attempt, marker unchanged
        return;
    }
    const size_t batch = read.value;
    if (batch == 0) {
        if (window_count_ == 0) {
            completeTransfer();  // everything streamed AND acked
        } else {
            phase_ = SyncPhase::WaitAck;  // stream done; window still in flight
        }
        return;
    }

    size_t i = 0;
    while (i < batch && window_count_ < kMaxUnackedChunks) {
        size_t count = per_chunk;
        if (count > batch - i) {
            count = batch - i;
        }
        if (request_max_ > 0) {
            const size_t remaining = request_max_ - sent_this_request_;
            if (count > remaining) {
                count = remaining;  // T04: CURSOR.max honored exactly
            }
        }
        if (count == 0) {
            break;
        }
        Result<size_t> enc =
            encodeChunkFrame(next_chunk_no_, &read_buf_[i], count, tx_, sizeof tx_);
        if (!enc.ok()) {
            failAttempt();  // caller-bug grade: abort rather than spin
            return;
        }
        Result<void> sent = sendOrCount(SyncChannel::Data, tx_, static_cast<uint16_t>(enc.value));
        if (!sent.ok()) {
            return;  // Busy/NotReady/failure: next_read_seq_ untouched, no loss (F09)
        }
        WindowEntry& entry = window_[window_count_];
        ++window_count_;
        entry.first_seq = read_buf_[i].seq;
        entry.num_events = static_cast<uint8_t>(count);
        entry.chunk_no = next_chunk_no_;
        entry.sent_at_ms = now;
        ++next_chunk_no_;
        events_sent_ += static_cast<uint32_t>(count);
        sent_this_request_ += static_cast<uint32_t>(count);
        const uint32_t last_of_chunk = read_buf_[i + count - 1].seq;
        next_read_seq_ = last_of_chunk + 1;
        if (last_of_chunk > last_seq_) {
            last_seq_ = last_of_chunk;  // STATUS.last_seq = newest seq seen (§3.2)
        }
        i += count;
    }

    if (request_max_ > 0 && sent_this_request_ >= request_max_) {
        if (window_count_ == 0) {
            completeTransfer();
        } else {
            phase_ = SyncPhase::WaitAck;
        }
        return;
    }
    if (window_count_ >= kMaxUnackedChunks) {
        phase_ = SyncPhase::WaitAck;  // flow control (T07): window full
    }
    // else stay Streaming; an exhausted store completes on the next read==0.
}

Result<void> SyncSession::sendOrCount(SyncChannel ch, const uint8_t* data, uint16_t len) {
    Result<void> sent = transport_->send(ch, data, len);
    if (sent.ok()) {
        send_failures_ = 0;
        return sent;
    }
    switch (sent.err) {
    case Err::Busy:
        return sent;  // congestion: nothing consumed, retry on a later pump (§10)
    case Err::NotReady:
        failAttempt();  // §10: should not be observed (linkUp is polled first)
        return sent;
    default:
        if (++send_failures_ >= kMaxAckTimeoutsPerTransfer) {
            failAttempt();  // persistent send failure (F07): bounded, then give up
        }
        return sent;
    }
}

void SyncSession::checkAckTimeout(TimeMs now, uint16_t usable_bytes) {
    if (window_count_ == 0) {
        return;
    }
    const TimeMs deadline = window_[0].sent_at_ms + kAckTimeoutMs;
    if (now < deadline) {
        return;
    }
    if (++timeouts_since_ack_ >= kMaxAckTimeoutsPerTransfer) {
        failAttempt();  // F07: bounded resends, then abort; the phone re-requests
        return;
    }
    // Resend the unacked suffix with the ORIGINAL chunk numbers (§4.2, F01/F08).
    for (size_t i = 0; i < window_count_; ++i) {
        WindowEntry& entry = window_[i];
        const size_t frame_bytes =
            static_cast<size_t>(entry.num_events) * kEventRecordBytes + kChunkFramingBytes;
        if (frame_bytes > usable_bytes) {
            failAttempt();  // MTU shrank below the window: re-request rebuilds it
            return;
        }
        Result<size_t> read = store_->readFrom(entry.first_seq, read_buf_, entry.num_events);
        if (!read.ok() || read.value != entry.num_events || read_buf_[0].seq != entry.first_seq) {
            failAttempt();  // store changed under the window (append-only: defensive)
            return;
        }
        Result<size_t> enc =
            encodeChunkFrame(entry.chunk_no, read_buf_, entry.num_events, tx_, sizeof tx_);
        if (!enc.ok()) {
            failAttempt();
            return;
        }
        Result<void> sent = sendOrCount(SyncChannel::Data, tx_, static_cast<uint16_t>(enc.value));
        if (!sent.ok()) {
            return;  // Busy/NotReady/failure counted; this entry retries next pump
        }
        entry.sent_at_ms = now;  // restart this entry's window
        ++chunks_resent_;
    }
}

void SyncSession::completeTransfer() {
    phase_ = SyncPhase::Idle;
    window_count_ = 0;
    aborted_ = false;
    status_dirty_ = true;  // §3.2: STATUS on transfer complete (T06)
}

void SyncSession::failAttempt() {
    if (phase_ == SyncPhase::Idle && window_count_ == 0) {
        return;  // nothing in flight (e.g. link flap while idle): keep honest Idle
    }
    phase_ = SyncPhase::Idle;
    window_count_ = 0;
    aborted_ = true;       // STATUS bit3: the last attempt gave up (§3.2)
    status_dirty_ = true;  // §3.2: STATUS on abort — sent if the link is still up
}

std::uint8_t SyncSession::statusFlags() const {
    uint8_t flags = 0;
    if (link_up_last_) {
        flags |= kStatusFlagLinkUp;
    }
    if (phase_ == SyncPhase::Streaming) {
        flags |= kStatusFlagStreaming;
    }
    if (mtu_too_low_) {
        flags |= kStatusFlagMtuTooLow;
    }
    if (aborted_) {
        flags |= kStatusFlagTransferAborted;
    }
    return flags;
}

void SyncSession::flushStatus(TimeMs now) {
    (void)now;
    if (!status_dirty_) {
        return;
    }
    // flushStatus runs only while the link is up (step()), and link_up_last_
    // already mirrors that — statusFlags() therefore yields the same byte the
    // provider publishes for this step (asserted in the host suite).
    encodeStatusFrame(last_seq_, store_->lastSyncedSeq(), statusFlags(), tx_);
    Result<void> sent =
        transport_->send(SyncChannel::Control, tx_, static_cast<uint16_t>(kStatusFrameBytes));
    if (sent.ok() || sent.err == Err::NotReady) {
        // NotReady: the link vanished mid-flush; the next link-up edge re-owes it.
        status_dirty_ = false;
    }
    // Busy (previous indication unconfirmed) or a hard failure keeps the flag:
    // retried on the next pump, bounded by pump cadence (§10).
}

void SyncSession::publishStatus(TimeMs now) {
    status_.t_ms = now;
    status_.last_seq = last_seq_;
    status_.last_synced_seq = (store_ != nullptr) ? store_->lastSyncedSeq() : 0;
    status_.events_sent = events_sent_;
    status_.chunks_resent = chunks_resent_;
    status_.protocol_version = kSyncProtocolVersion;
    status_.flags = statusFlags();  // same helper as the STATUS frame (§3.2)
    have_status_ = true;
}

}  // namespace bw::sync
