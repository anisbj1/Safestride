#pragma once

// bw/confirm/confirmation_manager.h — F14 §1-§5: prompt the user to
// confirm/reject a detected event and store the label with it.
//
// Pure decision-layer logic (01 §1): no UI rendering, no event interpretation,
// no blocking — inputs and outputs are bw_core value types only (plan D2).
// The composition root binds the seams at P4:
//   onEvent        <- F17 subscriber fan-out (adapter, wiring-level)
//   onAlertStatus  <- F10 ack source via AppContext::alert_status (plan D3)
//   onCommand      <- dispatcher routes CommandType::ConfirmEvent/RejectEvent
//   step(now)      <- app loop (no dedicated task, plan §15)

#include <cstddef>
#include <cstdint>

#include <bw/alerts.h>
#include <bw/commands.h>
#include <bw/confirm/confirm_config.h>
#include <bw/confirm/pending_queue.h>
#include <bw/events.h>
#include <bw/providers.h>
#include <bw/types.h>

namespace bw::confirm {

// Spec §14 (DoD): label counters. This is the in-RAM view for host tests and
// diagnostics; the persisted view F28 exports is Event.label itself, written
// through IEventStore::setLabel (plan §14 — no separate contract needed).
struct ConfirmLabelStats {
    uint32_t confirmed = 0;          // user confirmed (incl. late labels)
    uint32_t rejected = 0;           // user rejected (incl. late labels)
    uint32_t unlabeled = 0;          // prompt timed out with no answer (spec §9)
    uint32_t dropped_unlabeled = 0;  // queue overflow dropped, never prompted
    uint32_t store_retries = 0;      // setLabel retry attempts performed (spec §10)
    uint32_t store_giveups = 0;      // labels lost to the store after kRetryCount
    uint32_t publish_failures = 0;   // label-change notifications the sink rejected
};

class ConfirmationManager : public IConfirmPromptProvider {
public:
    explicit ConfirmationManager(const ConfirmConfig& cfg = ConfirmConfig{});

    // F17/F18 arrive in parallel this wave (plan §15): attach when they land;
    // nullptr means labels/notifications are dropped AND COUNTED (spec §10,
    // F09 D11 precedent) — never silently lost, never blocking.
    void setEventStore(IEventStore* store);
    void setEventSink(IEventSink* sink);

    // ---- D2 entry points (bw_core value types only) ------------------------

    // Event requiring confirmation. Ignores non-eligible types (D6) and F14's
    // own notifications (D5 loop guard). Never blocks (spec §5).
    void onEvent(const Event& e);

    // Alert status edge (plan D3): acknowledged==true starts one defer window
    // (D1); acknowledged==false withdraws any active prompt back to the queue —
    // a prompt must never ride over an alert (spec §5).
    void onAlertStatus(const AlertStatus& status);

    // ConfirmEvent/RejectEvent; arg0 = Event::seq (plan §15); arg0 == 0 =
    // currently offered prompt, false when none (spec §13). Returns true iff
    // the command was consumed. Late labeling (D1): a seq that is no longer
    // queued (timed out or dropped) is still written to the store.
    bool onCommand(const Command& c);

    // Drives defer/offer/timeout/retry and refreshes the provider state.
    // Call from the app loop at a regular cadence (no dedicated task, plan §15).
    void step(TimeMs now);

    // ---- outputs ------------------------------------------------------------

    // CR-016 provider for F12: false before the first step() ("no state yet").
    bool latest(ConfirmPromptState& out) const override;

    ConfirmLabelStats labelStats() const { return stats_; }

    // Observability for tests/diagnostics (no contract impact).
    size_t waitingCount() const { return queue_.size(); }
    bool hasOfferedPrompt() const { return offered_; }

private:
    // One label waiting for the store (spec §10): attempts counts tries made,
    // the first of which already failed when the entry was enqueued.
    struct RetryEntry {
        uint32_t seq;
        UserLabel label;
        uint8_t attempts;
    };

    void offerFront(TimeMs now);  // pop the oldest waiting entry into Offered
    void finalizeLabel(uint32_t seq, UserLabel label, TimeMs t_ms);
    void publishLabelEvent(uint32_t seq, UserLabel label, TimeMs t_ms);
    void recordDrop(const PendingEntry& entry);  // overflow -> Unlabeled marker (§9)
    void enqueueRetry(uint32_t seq, UserLabel label);
    void flushRetries();
    void refreshState(TimeMs now);

    ConfirmConfig cfg_;
    IEventStore* store_ = nullptr;
    IEventSink* sink_ = nullptr;

    PendingEventQueue queue_;
    PendingEntry offered_entry_{};  // held OUTSIDE the queue while Offered:
                                    // the active prompt is never dropped by an
                                    // overflow (plan §9.2)
    bool offered_ = false;

    bool ack_seen_ = false;      // current alert acknowledged (edge-latched)
    TimeMs defer_deadline_ = 0;  // 0 = no defer window running
    TimeMs offer_deadline_ = 0;  // prompt auto-resolves here; 0 unless Offered

    RetryEntry retries_[kMaxRetryPending]{};
    uint8_t retry_entries_ = 0;

    ConfirmLabelStats stats_{};
    ConfirmPromptState state_{};
    bool has_state_ = false;
};

}  // namespace bw::confirm
