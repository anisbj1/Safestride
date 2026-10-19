#include <bw/storage/event_store.h>

#include <cstring>

namespace bw::storage {

namespace {

static_assert(sizeof(LittleFsEventStore::Op) == 64,
              "F-09: Op width pinned — spec §15 states Op = 64 B, so the op "
              "queue (kStorageQueueDepth x sizeof(Op)) = 16 x 64 B = 1024 B");

// F-07 RAII scope: holds the attached producer guard across one store entry
// point so validation, capacity check, queue push and seq reservation are one
// atomic unit across producer tasks. nullptr guard (host single-thread tests)
// = no-op. Never spans flash IO — only the RAM-only entry points take it.
struct GuardScope {
    explicit GuardScope(IQueueGuard* g) : g_(g) {
        if (g_ != nullptr) {
            g_->enter();
        }
    }
    ~GuardScope() {
        if (g_ != nullptr) {
            g_->leave();
        }
    }
    GuardScope(const GuardScope&) = delete;
    GuardScope& operator=(const GuardScope&) = delete;

private:
    IQueueGuard* g_;
};

void putU32LE(uint8_t* out, uint32_t v) {
    out[0] = static_cast<uint8_t>(v & 0xFF);
    out[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

// readFrom visitor (D8): serves events with seq >= from_seq in ascending
// order, applying the manager's label-patch cache (labels are tail patches,
// D4). Stops the walk as soon as `max` events are collected.
struct ReadVisitor final : ScanVisitor {
    const SegmentManager* segments;
    bw::Event* out;
    size_t max;
    size_t count = 0;

    bool onEvent(const bw::Event& event) override {
        bw::Event copy = event;
        bw::UserLabel patched{};
        if (segments->patchFor(event.seq, patched)) {
            copy.label = patched;  // latest tail patch wins (D4)
        }
        // Cache miss: keep the label as persisted ON THE EVENT RECORD — a
        // miss must never clobber an in-band label with Unlabeled (F-10).
        if (out != nullptr && count < max) {
            out[count] = copy;
        }
        ++count;
        return count < max;
    }

    // F-10: a patch whose cache entry was evicted (16-entry ring) still wins —
    // the walk sees its disk record and applies it to copies already emitted.
    void onPatch(uint32_t seq, bw::UserLabel label) override {
        if (out == nullptr) {
            return;
        }
        for (size_t i = 0; i < count && i < max; ++i) {
            if (out[i].seq == seq) {
                out[i].label = label;
            }
        }
    }
};

}  // namespace

LittleFsEventStore::LittleFsEventStore(IFlashFs& fs, StorageCounters& counters)
    : fs_(fs), counters_(counters), segments_(fs, counters) {}

Result<void> LittleFsEventStore::mount() {
    // F-03: two-step mount — NEVER a silent format. Step 1 mounts READ-ONLY;
    // only if that fails do we fall back to format-on-fail (§10: empty rather
    // than a brick), and the fallback is reported + counted BEFORE the wipe so
    // "restarts empty" reaches F23 instead of passing silently (wire_storage
    // attaches the fault reporter before mount() for exactly this). Warning,
    // not Error: the medium still works after provisioning — the signal is
    // "prior data, if any, is gone", a quality downgrade, not an outage.
    Result<void> mounted = fs_.mount(/*format_on_fail=*/false);
    if (!mounted.ok()) {
        if (faults_ != nullptr) {
            faults_->report(bw::FaultCode::StorageDegraded, bw::FaultSeverity::Warning, 0);
        }
        mounted = fs_.mount(/*format_on_fail=*/true);  // §10 never brick
        if (!mounted.ok()) {
            return mounted;
        }
        // Counted only when the fallback format actually ran (a recovery):
        // first-boot provisioning counts too — the medium needed formatting.
        ++counters_.format_recoveries;
    }
    const Result<void> scanned = segments_.mount();
    if (!scanned.ok()) {
        return scanned;
    }
    last_synced_seq_ = segments_.lastMarkerSeq();  // D5: conservative after power cut
    marker_dirty_ = false;
    return {bw::Err::Ok};
}

Result<uint32_t> LittleFsEventStore::append(const bw::Event& event) {
    // F-07: one producer-critical section across validate → capacity → push →
    // reserve, so two producer tasks can never double-assign a seq or race
    // the SPSC push side. RAM-only; no flash under the guard (D3).
    const GuardScope guard{guard_};
    const uint32_t next = segments_.nextSeq();
    // F17 leaves seq 0 (store assigns) or pre-assigns the expected value;
    // anything else would break monotonicity — refused, never silently fixed.
    if (event.seq != 0 && event.seq != next) {
        return {bw::Err::InvalidArg, 0};
    }
    // Synchronous capacity refusal (CR-004/§5): the log is full and nothing
    // is evictable because every event is unsynced (D6). Nothing is consumed.
    if (!segments_.canPersistEvent(last_synced_seq_)) {
        reportFault(0);  // F-06: refusal is visible to F23, not just the caller
        return {bw::Err::Full, 0};
    }
    Op op{};
    op.kind = Op::Kind::AppendEvent;
    op.event = event;
    op.event.seq = next;
    op.seq = 0;
    op.label = 0;
    const bw::Err pushed = queue_.push(op);
    if (pushed != bw::Err::Ok) {
        return {pushed, 0};  // queue full: refused, caller retries (CR-004)
    }
    segments_.reserveSeq();  // seq bound to this op at enqueue (D3), never reused
    return {bw::Err::Ok, next};
}

Result<size_t> LittleFsEventStore::readFrom(uint32_t from_seq, bw::Event* out, size_t max) {
    if (max == 0 || out == nullptr) {
        return {bw::Err::InvalidArg, 0};
    }
    ReadVisitor visitor{};
    visitor.segments = &segments_;
    visitor.out = out;
    visitor.max = max;
    const Result<void> walked = segments_.scanFrom(from_seq, visitor);
    if (!walked.ok()) {
        return {bw::Err::Corrupt, 0};  // IO failure only; gaps are not errors (D8)
    }
    return {bw::Err::Ok, visitor.count};
}

Result<void> LittleFsEventStore::setLabel(uint32_t seq, bw::UserLabel label) {
    // F-07: push + cache update as one unit across producer tasks.
    const GuardScope guard{guard_};
    if (label != bw::UserLabel::Unlabeled && label != bw::UserLabel::Confirmed &&
        label != bw::UserLabel::Rejected) {
        return {bw::Err::InvalidArg};
    }
    if (seq == 0 || seq > segments_.lastEventSeq()) {
        return {bw::Err::NotFound};  // never guessed at (02 §1)
    }
    Op op{};
    op.kind = Op::Kind::LabelPatch;
    op.event = bw::Event{};
    op.seq = seq;
    op.label = static_cast<uint8_t>(label);
    const bw::Err pushed = queue_.push(op);
    if (pushed != bw::Err::Ok) {
        return {pushed};
    }
    // Visible to reads immediately; the patch record is persisted by pump().
    segments_.notePatch(seq, label);
    return {bw::Err::Ok};
}

Result<void> LittleFsEventStore::markSyncedThrough(uint32_t seq) {
    // F-07: check-then-set of the sync cursor as one unit (it does not push —
    // the finding's "three push sites" claim is wrong here — but two producer
    // tasks interleaving check/set could still regress the cursor).
    const GuardScope guard{guard_};
    if (seq == last_synced_seq_) {
        return {bw::Err::Ok};  // idempotent re-mark (host-testing property rule)
    }
    // Monotonic cursor: never backwards, never past persisted data (F20 reads
    // only what readFrom returned, so seq > lastEventSeq is a caller bug).
    if (seq < last_synced_seq_ || seq > segments_.lastEventSeq()) {
        return {bw::Err::InvalidArg};
    }
    last_synced_seq_ = seq;
    marker_dirty_ = true;  // flushed by pump() under kSyncCheckpointIntervalMs (D5)
    return {bw::Err::Ok};
}

size_t LittleFsEventStore::pump(bw::TimeMs now_ms) {
    size_t applied = 0;
    Op op{};
    while (queue_.pop(op)) {
        const Result<void> r = (op.kind == Op::Kind::AppendEvent)
                                   ? applyAppend(op.event)
                                   : applyPatch(op.seq, static_cast<bw::UserLabel>(op.label));
        if (!r.ok()) {
            // D3: the op is consumed either way — a post-enqueue write
            // failure is reported as a fault, never silently dropped.
            reportFault(now_ms);
        }
        ++applied;
    }
    if (marker_dirty_) {
        const bool due = (now_ms == 0) || !has_checkpoint_ ||
                         (now_ms - last_marker_ms_ >= kSyncCheckpointIntervalMs);
        if (due) {
            uint8_t payload[4];
            putU32LE(payload, last_synced_seq_);
            const Result<void> r = segments_.appendMeta(RecordType::SyncMarker, payload,
                                                        sizeof(payload), last_synced_seq_);
            if (r.ok()) {
                marker_dirty_ = false;
                has_checkpoint_ = true;
                last_marker_ms_ = now_ms;
                ++applied;
            } else {
                reportFault(now_ms);  // retried on a later pump (marker stays dirty)
            }
        }
    }
    if (applied > 0) {
        has_snapshot_ = true;
        refreshHealth(now_ms);
    }
    return applied;
}

Result<void> LittleFsEventStore::applyAppend(const bw::Event& event) {
    return segments_.appendEvent(event, last_synced_seq_);
}

Result<void> LittleFsEventStore::applyPatch(uint32_t seq, bw::UserLabel label) {
    uint8_t payload[5];
    putU32LE(payload, seq);
    payload[4] = static_cast<uint8_t>(label);
    // Harmless if the target event was evicted since enqueue (patch for an
    // absent seq is a no-op on reads; it only costs 17 B of log).
    return segments_.appendMeta(RecordType::LabelPatch, payload, sizeof(payload), last_synced_seq_);
}

void LittleFsEventStore::reportFault(bw::TimeMs now_ms) {
    if (faults_ != nullptr) {
        faults_->report(bw::FaultCode::StorageDegraded, bw::FaultSeverity::Error, now_ms);
    }
}

void LittleFsEventStore::refreshHealth(bw::TimeMs now_ms) {
    health_.t_ms = now_ms;
    const Result<uint32_t> total = fs_.totalBytes();
    const Result<uint32_t> free = fs_.freeBytes();
    health_.total_bytes = total.ok() ? total.value : 0;
    health_.free_bytes = free.ok() ? free.value : 0;
    health_.corrupt_records = counters_.corrupt_records;
    health_.torn_tails_repaired = counters_.torn_tails_repaired;
    health_.segments_skipped = counters_.segments_skipped;
    health_.evicted_synced_segments = counters_.evicted_synced_segments;
    health_.evicted_windows = counters_.evicted_windows;
    health_.kv_discards = counters_.kv_discards;
}

bool LittleFsEventStore::latest(bw::StorageHealth& out) const {
    if (!has_snapshot_) {
        return false;
    }
    out = health_;
    return true;
}

}  // namespace bw::storage
