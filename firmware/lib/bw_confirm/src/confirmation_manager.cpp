#include <bw/confirm/confirmation_manager.h>

namespace bw::confirm {

namespace {

// D5 payload layout for the UserMarked label-change notification (Event::
// payload is "type-specific", 02 §4.5 — F14 pins its own 8 bytes):
//   payload[0..3] = labeled seq, little-endian
//   payload[4]    = UserLabel value
//   payload[5..7] = reserved, written as 0
constexpr size_t kPayloadLabelByte = 4;

void writeSeqLe(Event& e, uint32_t seq) {
    for (uint8_t i = 0; i < 4; ++i) {
        e.payload[i] = static_cast<uint8_t>(seq >> (8U * i));
    }
}

}  // namespace

ConfirmationManager::ConfirmationManager(const ConfirmConfig& cfg) : cfg_(cfg) {}

void ConfirmationManager::setEventStore(IEventStore* store) {
    store_ = store;
}

void ConfirmationManager::setEventSink(IEventSink* sink) {
    sink_ = sink;
}

void ConfirmationManager::onEvent(const Event& e) {
    // D5 loop guard: never re-process notifications we published ourselves.
    if (e.detector_id == kDetectorId) {
        return;
    }
    // D6: eligibility is configuration — F14 never decides what an event is
    // (spec §5), it only prompts for the types the wiring named.
    if (!cfg_.isEligible(e.type)) {
        return;
    }

    PendingEntry entry{};
    entry.received_ms = e.t_start_ms;
    entry.seq = e.seq;
    entry.type = e.type;

    PendingEntry evicted{};
    if (queue_.push(entry, evicted)) {
        recordDrop(evicted);  // spec §9: drop oldest with an Unlabeled marker
    }
}

void ConfirmationManager::onAlertStatus(const AlertStatus& status) {
    if (status.acknowledged) {
        // One defer window per ack EDGE: a steady stream of acknowledged=true
        // statuses must not keep restarting the 5 s defer (D1).
        if (!ack_seen_) {
            ack_seen_ = true;
            defer_deadline_ = status.t_ms + cfg_.defer_after_ack_ms;
        }
        return;
    }

    // New alert (or none): prompts wait (D1). An active prompt is withdrawn
    // back to the FRONT of the queue — it was offered first and keeps its
    // place; the next ack re-offers it after one defer window (spec §9).
    ack_seen_ = false;
    defer_deadline_ = 0;
    if (offered_) {
        offered_ = false;
        offer_deadline_ = 0;
        PendingEntry evicted{};
        if (queue_.pushFront(offered_entry_, evicted)) {
            // Waiting queue filled while this prompt was offered: the same
            // drop-oldest rule (spec §9) applies to the oldest waiting entry.
            recordDrop(evicted);
        }
    }
}

bool ConfirmationManager::onCommand(const Command& c) {
    if (c.type != CommandType::ConfirmEvent && c.type != CommandType::RejectEvent) {
        return false;  // not ours — the dispatcher keeps it
    }

    // Plan §15 mapping: arg0 = target Event::seq. F11's button table emits
    // arg0 = 0 (pinned by test/bw_input/test_input_router.cpp:88/98), and 0
    // is never a valid Event::seq (F17 assigns from 1) — so arg0 == 0 means
    // "the currently offered prompt" (spec §13, P3/F-1). With no prompt
    // offered there is nothing to label: the command is not consumed and
    // seq 0 is never written.
    uint32_t seq = c.arg0;
    if (seq == 0) {
        if (!offered_) {
            return false;
        }
        seq = offered_entry_.seq;
    }
    const UserLabel label =
        (c.type == CommandType::ConfirmEvent) ? UserLabel::Confirmed : UserLabel::Rejected;

    if (offered_ && offered_entry_.seq == seq) {
        offered_ = false;
        offer_deadline_ = 0;  // next step() offers the next waiting entry (or Idles)
    } else {
        // Not the active prompt: honor an explicit label on a waiting entry
        // (removing it), or late-label an entry that already timed out / was
        // dropped (D1: late labeling allowed).
        PendingEntry removed{};
        (void)queue_.removeBySeq(seq, removed);
    }

    finalizeLabel(seq, label, c.t_ms);
    return true;
}

void ConfirmationManager::step(TimeMs now) {
    flushRetries();  // spec §10: bounded, lazy store retries

    if (offered_ && now >= offer_deadline_) {
        // kPromptTimeoutMs elapsed with no answer — no penalty, no label
        // write (Unlabeled is already the stored state), next entry follows
        // in the same step.
        ++stats_.unlabeled;
        offered_ = false;
        offer_deadline_ = 0;
    }

    if (!offered_ && ack_seen_ && now >= defer_deadline_ && !queue_.empty()) {
        offerFront(now);
    }

    refreshState(now);
}

bool ConfirmationManager::latest(ConfirmPromptState& out) const {
    if (!has_state_) {
        return false;  // "no state yet" (CR-016) — before the first step()
    }
    out = state_;
    return true;
}

// ---- internals -------------------------------------------------------------

void ConfirmationManager::offerFront(TimeMs now) {
    PendingEntry front{};
    if (!queue_.pop(front)) {
        return;
    }
    offered_entry_ = front;
    offered_ = true;
    offer_deadline_ = now + cfg_.prompt_timeout_ms;
}

void ConfirmationManager::finalizeLabel(uint32_t seq, UserLabel label, TimeMs t_ms) {
    if (label == UserLabel::Confirmed) {
        ++stats_.confirmed;
    } else if (label == UserLabel::Rejected) {
        ++stats_.rejected;
    }
    // Notification first (D5): it is independent of the store outcome, so
    // F17/F21 learn about the label even while the store is retrying.
    publishLabelEvent(seq, label, t_ms);

    if (store_ == nullptr) {
        ++stats_.store_giveups;  // no store attached yet (F18 slot) — counted, not silent
        return;
    }
    if (store_->setLabel(seq, label).ok()) {
        return;
    }
    enqueueRetry(seq, label);  // spec §10: keep the label in RAM, retry later
}

void ConfirmationManager::publishLabelEvent(uint32_t seq, UserLabel label, TimeMs t_ms) {
    Event e{};
    e.t_start_ms = t_ms;  // producers never set seq/boot_id — F17 assigns them
    e.type = EventType::UserMarked;
    e.label = label;
    e.reasons = reasonBit(ReasonCode::UserMarked);
    e.detector_id = kDetectorId;  // D5: marks this as F14's own emission
    e.schema_version = Event::kSchemaVersion;
    writeSeqLe(e, seq);
    e.payload[kPayloadLabelByte] = static_cast<uint8_t>(label);

    if (sink_ == nullptr || !sink_->publish(e).ok()) {
        ++stats_.publish_failures;  // Result is handled (counted), never ignored
    }
}

void ConfirmationManager::recordDrop(const PendingEntry& entry) {
    ++stats_.dropped_unlabeled;
    publishLabelEvent(entry.seq, UserLabel::Unlabeled, entry.received_ms);
}

void ConfirmationManager::enqueueRetry(uint32_t seq, UserLabel label) {
    if (cfg_.retry_count <= 1 || retry_entries_ >= kMaxRetryPending) {
        // retry_count 1 = first attempt was the only one; buffer full = more
        // labels failed than can be held (plan §11 bound) — give up honestly.
        ++stats_.store_giveups;
        return;
    }
    retries_[retry_entries_] = RetryEntry{seq, label, /*attempts=*/1};
    ++retry_entries_;
}

void ConfirmationManager::flushRetries() {
    uint8_t i = 0;
    while (i < retry_entries_) {
        RetryEntry& entry = retries_[i];
        ++stats_.store_retries;  // a retry attempt is performed here, outcome below
        const bool persisted = (store_ != nullptr) && store_->setLabel(entry.seq, entry.label).ok();
        if (persisted) {
            // Done — swap-remove (entry order carries no meaning).
            retries_[i] = retries_[retry_entries_ - 1];
            --retry_entries_;
            continue;
        }
        ++entry.attempts;
        if (entry.attempts >= cfg_.retry_count) {
            ++stats_.store_giveups;  // spec §10: bounded retries, then counted loss
            retries_[i] = retries_[retry_entries_ - 1];
            --retry_entries_;
            continue;
        }
        ++i;
    }
}

void ConfirmationManager::refreshState(TimeMs now) {
    state_.t_ms = now;
    state_.seq = offered_ ? offered_entry_.seq : 0;
    if (offered_) {
        state_.phase = ConfirmPromptPhase::Offered;
        state_.offer_deadline_ms = offer_deadline_;
    } else if (queue_.empty()) {
        state_.phase = ConfirmPromptPhase::Idle;
        state_.offer_deadline_ms = 0;
    } else {
        state_.phase = ConfirmPromptPhase::Deferred;
        state_.offer_deadline_ms = 0;
    }
    // Waiting entries only: the offered one is held outside the queue, so this
    // is exactly "pending events behind the offered one" (CR-016 field comment).
    state_.queue_depth = static_cast<uint8_t>(queue_.size());
    has_state_ = true;
}

}  // namespace bw::confirm
