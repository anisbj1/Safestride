#pragma once

// bw/storage/event_store.h — LittleFsEventStore: F18's bw::IEventStore
// implementation plus the CR-015 bw::IStorageHealthProvider (F18 spec §15.2,
// §15.3 D3/D5/D6/D8).
//
// Write-path boundary (D3): append()/setLabel() validate and enqueue into a
// fixed queue and markSyncedThrough() advances the RAM cursor — none of the
// three touches flash, so real-time producers cannot stall on a flash write.
// pump(), driven solely by wire_storage's StorageTask (runtime.md §3.2, core
// 0), applies the queue to flash and flushes the due sync checkpoint.
//
// Multi-producer safety (F-07): the queue is SPSC, but the three entry points
// above are called from different tasks (F17 append, input setLabel, F20
// markSyncedThrough). Attaching an IQueueGuard (wire_storage attaches a
// FreeRTOS critical section on device) makes each entry point atomic across
// producers: push + seq reservation + cache update can never interleave.
// pump() — the single consumer — never takes the guard, and no guard is ever
// held across a flash write (D3).
//
// Seq authority: the store owns the seq watermark (recovered at mount from
// the log, D5). append() accepts Event.seq == 0 (store assigns) or ==
// nextSeq (F17 pre-assigned, verified); anything else is refused with
// Err::InvalidArg — the monotonic "never reused" rule is enforced here.

#include <cstdint>

#include <bw/containers/spsc_queue.h>
#include <bw/fault.h>
#include <bw/providers.h>

#include <bw/storage/constants.h>
#include <bw/storage/counters.h>
#include <bw/storage/flash_fs.h>
#include <bw/storage/segment_manager.h>

namespace bw::storage {

// Producer-side mutual exclusion for the store's entry points (F-07). The
// queue itself stays SPSC: the guard serializes the PRODUCER side only, so
// from the queue's point of view there is still exactly one producer. The
// consumer (pump) stays unguarded. nullptr = no guard (host single-thread
// tests; wire_storage attaches a critical section on the device).
struct IQueueGuard {
    virtual void enter() = 0;
    virtual void leave() = 0;

protected:
    ~IQueueGuard() = default;
};

class LittleFsEventStore final : public bw::IEventStore, public bw::IStorageHealthProvider {
public:
    // Queue element (trivially copyable, 02 §1). AppendEvent carries the full
    // event; LabelPatch carries seq + label only.
    struct Op {
        enum class Kind : uint8_t { AppendEvent, LabelPatch };
        Kind kind;
        bw::Event event;
        uint32_t seq;
        uint8_t label;
    };

    LittleFsEventStore(IFlashFs& fs, StorageCounters& counters);

    // F26 sink; nullptr (until wired) = faults are counted only, never a crash.
    void attachFaultReporter(bw::IFaultReporter* reporter) { faults_ = reporter; }

    // F-07: producer guard for append/setLabel/markSyncedThrough. Attach
    // BEFORE tasks start (wire_storage); nullptr = unguarded.
    void attachQueueGuard(IQueueGuard* guard) { guard_ = guard; }

    // Mounts the fs with the F-03 two-step contract: a read-only mount first;
    // ONLY a failed read-only mount falls back to format-on-fail, and that
    // fallback reports StorageDegraded + counts format_recoveries BEFORE the
    // wipe — §10 "restarts empty" is never silent. Attach the fault reporter
    // before calling this. Runs the recovery scan (D4); call once from wire()
    // before the task starts.
    Result<void> mount();

    // --- bw::IEventStore ------------------------------------------------------
    Result<uint32_t> append(const bw::Event& event) override;
    Result<size_t> readFrom(uint32_t from_seq, bw::Event* out, size_t max) override;
    Result<void> setLabel(uint32_t seq, bw::UserLabel label) override;
    Result<void> markSyncedThrough(uint32_t seq) override;
    uint32_t lastSyncedSeq() const override { return last_synced_seq_; }

    // --- bw::IStorageHealthProvider (CR-015) ---------------------------------
    // Snapshot refreshed by every productive pump() (diagnostics tolerate the
    // staleness between pumps). false until the first pump.
    bool latest(bw::StorageHealth& out) const override;

    // --- StorageTask boundary (D3) -------------------------------------------
    // Applies queued ops and flushes a due sync marker. Returns ops applied
    // (0 = idle; the task then sleeps kStorageIdlePollMs). now_ms == 0 means
    // "no clock yet": a dirty marker is flushed immediately.
    size_t pump(bw::TimeMs now_ms);

    size_t pendingOps() const { return queue_.size(); }
    uint32_t queueDrops() const { return queue_.drops(); }
    SegmentManager& segments() { return segments_; }

private:
    Result<void> applyAppend(const bw::Event& event);
    Result<void> applyPatch(uint32_t seq, bw::UserLabel label);
    void reportFault(bw::TimeMs now_ms);
    void refreshHealth(bw::TimeMs now_ms);

    IFlashFs& fs_;
    StorageCounters& counters_;
    SegmentManager segments_;
    bw::IFaultReporter* faults_ = nullptr;
    IQueueGuard* guard_ = nullptr;  // F-07; nullptr = unguarded (host tests)

    bw::SpscQueue<Op, kStorageQueueDepth> queue_;

    uint32_t last_synced_seq_ = 0;
    // Shared with the producer side (audit NEW-L1): markSyncedThrough() sets
    // it under the F-07 guard, pump() reads and clears it. A single flag that
    // pairs with nothing else: the worst case of a lost or repeated toggle is
    // one skipped or repeated checkpoint flush — the marker write itself is
    // idempotent (D5), never data loss.
    bool marker_dirty_ = false;
    bw::TimeMs last_marker_ms_ = 0;
    bool has_checkpoint_ = false;  // at least one marker flushed this boot

    bool has_snapshot_ = false;
    bw::StorageHealth health_{};
};

}  // namespace bw::storage
