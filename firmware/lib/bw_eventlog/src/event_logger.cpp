#include <bw/eventlog/event_logger.h>

#include <bw/platform_sync.h>

namespace bw::eventlog {

EventLogger::EventLogger(const bw::IClock* clock, bw::IFaultReporter* faults)
    : clock_(clock), faults_(faults) {}

bw::Result<void> EventLogger::publish(const bw::Event& incoming) {
    bw::Event event = incoming;
    const bw::TimeMs clock_now = (clock_ != nullptr) ? clock_->nowMs() : 0;

    // §15.2 D10 stamping at creation: fill only what the producer left empty,
    // never overwrite a producer-captured value (F08 captures t_start_unix at
    // candidate start; 0 stays 0 = "unknown at creation", DEC-009).
    if (event.t_start_ms == 0) {
        event.t_start_ms = clock_now;  // may stay 0 → drain() falls back (D10)
    }
    if (event.t_start_unix == 0 && clock_ != nullptr) {
        const bw::UnixSeconds wall = clock_->unixSeconds();
        if (wall != 0) {
            event.t_start_unix = wall;
        }
    }
    event.schema_version = bw::Event::kSchemaVersion;  // F17 owns the record format

    // One critical section for de-dup + admission: array work only, no foreign
    // code inside (D5), so publish() never blocks.
    bw::CriticalSection guard;
    const bw::TimeMs reference = event.t_start_ms != 0 ? event.t_start_ms : clock_now;
    if (!rate_limiter_.allow(event.type, reference)) {
        ++stats_.rate_limited;  // §15.2 D7: de-dup, not loss → Ok, not Err
        return {bw::Err::Ok};
    }

    if (count_ == kEventQueueDepth) {
        const size_t victim = lowestPriorityResident();
        const EventPriority victim_priority = priorityFor(queue_[victim].type);
        if (victim_priority <= priorityFor(event.type)) {
            // No resident strictly less important than the incoming event
            // (equal priority → incoming waits behind its peers' turn, §15.3:
            // "rejected + counted"): refuse, count, leave the queue untouched.
            ++stats_.dropped_full;
            return {bw::Err::Full};
        }
        removeAt(victim);  // order among survivors preserved
        ++stats_.evicted_lower_priority;
    }

    queue_[(head_ + count_) % kEventQueueDepth] = event;
    ++count_;
    if (count_ > stats_.queue_high_water) {
        stats_.queue_high_water = static_cast<uint32_t>(count_);
    }
    return {bw::Err::Ok};
}

void EventLogger::emitBoot() {
    if (boot_emitted_) {
        return;  // D10: exactly one Boot per logger (per boot)
    }
    boot_emitted_ = true;  // set even if admission later refuses (counted, not retried)
    bw::Event event{};
    event.type = bw::EventType::Boot;
    event.detector_id = kEventlogDetectorId;
    event.confidence = 1.0f;  // factual record, not a detection (wording rule N/A)
    event.quality = {bw::DataQuality::Insufficient, bw::DataQuality::Insufficient};
    const bw::Result<void> accepted = publish(event);
    (void)accepted;
}

void EventLogger::attachStore(bw::IEventStore* store) {
    attachStore(store, 0, false);
}

void EventLogger::attachStore(bw::IEventStore* store, uint32_t last_seq_hint) {
    attachStore(store, last_seq_hint, true);
}

void EventLogger::attachStore(bw::IEventStore* store, uint32_t last_seq_hint, bool has_hint) {
    if (store == nullptr) {
        bw::CriticalSection guard;
        store_ = nullptr;  // detaching holds queued events (drain returns 0)
        return;
    }
    if (recovered_) {
        bw::CriticalSection guard;
        store_ = store;  // same-boot re-attach: boot_id stays, no second probe
        return;
    }

    // §15.2 D3 recovery — runs BEFORE store_ becomes visible to the pump:
    // find the persisted tail by binary search on the predicate "an event with
    // seq >= s exists" (readFrom serves seq >= from_seq, oldest first — F00-
    // documented semantics, mem_event_store.h:24). Stored seqs are contiguous
    // and evicted from the head, so the predicate is monotone: <= 32 probes.
    uint32_t recovered_boot = 1;  // empty store or read error → first boot (§15.7 best-effort)
    bw::Event probe{};
    bool found_tail = false;
    if (has_hint && last_seq_hint == 0) {
        // The composition root has an authoritative mounted-store snapshot.
        // Empty is known without touching IEventStore.
    } else if (has_hint) {
        const bw::Result<size_t> hinted = store->readFrom(last_seq_hint, &probe, 1);
        if (hinted.ok() && hinted.value == 1 && probe.seq == last_seq_hint) {
            recovered_boot = probe.boot_id + 1;
            found_tail = true;
        }
    }

    if (!found_tail && !(has_hint && last_seq_hint == 0)) {
        const bw::Result<size_t> first = store->readFrom(1, &probe, 1);
        if (first.ok() && first.value == 1) {
            uint32_t lo = 1;
            uint32_t hi = kMaxSeq;
            uint32_t best = 1;
            while (lo <= hi) {
                const uint32_t mid = lo + (hi - lo) / 2;
                const bw::Result<size_t> hit = store->readFrom(mid, &probe, 1);
                if (!hit.ok()) {
                    break;  // store read failure mid-probe → keep the best so far
                }
                if (hit.value == 1) {
                    best = mid;
                    if (mid == kMaxSeq) {
                        break;  // ceiling reached: mid + 1 would wrap
                    }
                    lo = mid + 1;
                } else {
                    if (mid == 1) {
                        break;  // cannot happen below a non-empty store; stop safely
                    }
                    hi = mid - 1;
                }
            }
            const bw::Result<size_t> tail = store->readFrom(best, &probe, 1);
            if (tail.ok() && tail.value == 1) {
                recovered_boot = probe.boot_id + 1;  // "increments each boot" (02 §4.5)
            }
        }
    }
    {
        bw::CriticalSection guard;
        recovered_ = true;
        boot_id_ = recovered_boot;
        store_ = store;  // published together: no drain can see store without boot_id
    }
}

bool EventLogger::addSubscriber(bw::IEventSink* subscriber) {
    bw::CriticalSection guard;
    return subscribers_.add(subscriber);
}

size_t EventLogger::drain(bw::TimeMs now_ms) {
    checkTimeSync();  // may publish() Event{TimeSync} — takes its own guard

    bw::IEventStore* store;
    uint32_t boot_id;
    {
        bw::CriticalSection guard;
        store = store_;
        boot_id = boot_id_;
    }
    if (store == nullptr) {
        return 0;  // §15.5: nothing to drain against yet — the queue holds
    }
    if (retry_after_ms_ != 0 && now_ms < retry_after_ms_) {
        return 0;  // §10 backoff: no attempt before the horizon
    }

    size_t appended = 0;
    for (;;) {
        bw::Event event{};
        uint32_t epoch_at_peek = 0;
        {
            bw::CriticalSection guard;
            if (count_ == 0) {
                break;
            }
            event = queue_[head_];  // peek: leave it at the head until success
            epoch_at_peek = head_epoch_;
        }
        if (event.t_start_ms == 0) {
            // D10 fallback: published before any clock existed (empty ctx).
            event.t_start_ms = (clock_ != nullptr) ? clock_->nowMs() : now_ms;
        }
        event.seq = 0;            // §15.2 D3: F17 owns the field; the store assigns
        event.boot_id = boot_id;  // stamped post-recovery, at drain

        const bw::Result<uint32_t> seq = store->append(event);  // outside the guard (D5)
        if (!seq.ok()) {
            // §10: the event stays at the head; count, report once per
            // episode (report itself outside the guard — foreign code), back off.
            bool report_now = false;
            {
                bw::CriticalSection guard;
                ++stats_.append_retries;
                if (!fault_reported_) {
                    fault_reported_ = true;
                    ++stats_.store_fault_episodes;
                    report_now = true;
                }
            }
            if (report_now && faults_ != nullptr) {
                faults_->report(bw::FaultCode::StorageDegraded, bw::FaultSeverity::Warning, now_ms);
            }
            retry_after_ms_ = now_ms + kStoreRetryBackoffMs;
            break;
        }

        event.seq = seq.value;  // adopt the store-assigned seq (D3) for fan-out
        {
            bw::CriticalSection guard;
            if (head_epoch_ == epoch_at_peek) {
                head_ = (head_ + 1) % kEventQueueDepth;
                --count_;
            }
            // else: while append() ran outside the guard, publish() evicted the
            // very head we peeked (removeAt(0)) and counted it in
            // evicted_lower_priority. The queue has already shifted forward, so
            // advancing here would orphan the element now at head_ — a queued
            // event lost with no counter (audit M-1). The peeked event itself
            // is stored exactly once, just above.
            fault_reported_ = false;  // episode over → the next failure re-reports
        }
        retry_after_ms_ = 0;
        ++appended;

        const size_t rejected = subscribers_.fanout(event);  // outside the guard (D5)
        if (rejected != 0) {
            bw::CriticalSection guard;
            stats_.subscriber_rejected += static_cast<uint32_t>(rejected);
        }
    }
    return appended;
}

EventLogStats EventLogger::stats() const {
    bw::CriticalSection guard;
    return stats_;
}

size_t EventLogger::queued() const {
    bw::CriticalSection guard;
    return count_;
}

void EventLogger::checkTimeSync() {
    if (clock_ == nullptr) {
        return;
    }
    const bw::UnixSeconds wall = clock_->unixSeconds();
    if (wall == 0) {
        return;  // still unknown (02 §2): nothing to record (D10 i)
    }
    const bw::TimeMs now = clock_->nowMs();
    if (!have_sync_state_) {
        emitTimeSync(wall, now);  // D10 i: unknown → known
        have_sync_state_ = true;
        last_unix_ = wall;
        sync_anchor_ms_ = now;
        return;
    }
    if (wall == last_unix_) {
        return;  // unchanged since the last check: a static wall clock is not a
                 // new sync (F00 §13: the device IClock may not tick wall time)
    }
    // Changed: ticking or a phone SetTime? Expected = last value + the
    // monotonic seconds since it was last observed. Static-clock SetTime has a
    // huge elapsed expectation, so its delta still exceeds the tolerance.
    const bw::TimeMs elapsed_s =
        (now >= sync_anchor_ms_) ? (now - sync_anchor_ms_) / kMsPerSecond : 0;
    const bw::TimeMs expected = static_cast<bw::TimeMs>(last_unix_) + elapsed_s;
    const bw::TimeMs drift = (wall > expected) ? (wall - expected) : (expected - wall);
    if (drift > kWallResyncToleranceS) {
        emitTimeSync(wall, now);  // D10 ii: beyond tick jitter → a real re-sync
    }
    last_unix_ = wall;
    sync_anchor_ms_ = now;
}

void EventLogger::emitTimeSync(bw::UnixSeconds unix_s, bw::TimeMs now_ms) {
    bw::Event event{};
    event.type = bw::EventType::TimeSync;
    event.t_start_ms = now_ms;
    event.t_start_unix = unix_s;  // the newly-known wall time travels in the record
    event.detector_id = kEventlogDetectorId;
    event.confidence = 1.0f;  // factual record, not a detection
    event.quality = {bw::DataQuality::Insufficient, bw::DataQuality::Insufficient};
    const bw::Result<void> accepted = publish(event);
    (void)accepted;
}

size_t EventLogger::lowestPriorityResident() const {
    size_t victim = 0;
    EventPriority worst = priorityFor(queue_[0].type);
    for (size_t i = 1; i < count_; ++i) {
        const EventPriority current = priorityFor(queue_[(head_ + i) % kEventQueueDepth].type);
        if (current > worst) {
            worst = current;  // strictly less important → new victim (ties keep the older)
            victim = i;
        }
    }
    return victim;
}

void EventLogger::removeAt(size_t logical_index) {
    for (size_t k = logical_index; k + 1 < count_; ++k) {
        queue_[(head_ + k) % kEventQueueDepth] = queue_[(head_ + k + 1) % kEventQueueDepth];
    }
    if (logical_index == 0) {
        // The element drain() may be peeking right now has just been removed;
        // invalidate that peek (see head_epoch_ in the header).
        ++head_epoch_;
    }
    --count_;  // head_ unchanged; the tail slot freed itself
}

}  // namespace bw::eventlog
