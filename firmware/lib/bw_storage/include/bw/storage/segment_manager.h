#pragma once

// bw/storage/segment_manager.h — the segmented binary event log engine
// (F18 spec §15.2/§15.3 D4/D5/D6; DEC-007 ring-overwrite of oldest synced
// segments first).
//
// Responsibilities: fixed segment slot table, mount-time scan with torn-tail
// truncation and forward resync, append with rotation + synced-first
// eviction, seq watermark, label-patch cache, read-only record iteration for
// readFrom(). Pure logic over IFlashFs — no FreeRTOS/Arduino, host-tested.
//
// Thread model (F-02 audit): the StorageTask pump is the single CONSUMER of
// the op queue and the single writer of segment state; readFrom()/scanFrom()
// may run in another non-RT task CONCURRENTLY with a pump. Consequences:
//   * All decode/encode scratch is per-call STACK (76 B record buffer, 204 B
//     resync window — never members), so the two sides share no mutable
//     decode buffer and a read can never observe a half-written record.
//   * Shared words stay safe by being word-sized: append only extends the
//     current segment's used_bytes (a walker bounds its reads by the value
//     it loaded and every record is CRC-validated — a racing walker may see
//     slightly more or fewer records, never fabricated bytes).
//   * Residual: rotation/eviction may shift index_ entries during a walk, so
//     a concurrent reader can miss or repeat one segment at a rotation
//     boundary. Records stay CRC-clean and readFrom's cursor makes a
//     duplicate harmless (it cannot advance F20's sync twice, D8). Full
//     index snapshotting under a lock was rejected: it would put locking
//     near flash IO (D3: producers never stall) for a rare, bounded,
//     non-fabricating race — decision recorded in the spec status log.
//   * Shared state, listed in full (audit NEW-L1): index_ (residual above),
//     used_bytes (append-only extension, word-sized), patch_cache_ pairs
//     (published via the invalidate/commit protocol, NEW-M1) and
//     resync_budget_used_ — a READ-ONLY scanFrom walk bumps it via
//     searchNextValidRecord while the pump may bump it too; over-counting
//     only makes the resync stop earlier (conservative, no data effect).
//     marker_dirty_ on the event store is the producer/pump flag (its note
//     lives with the member in event_store.h).

#include <cstddef>
#include <cstdint>

#include <bw/events.h>
#include <bw/result.h>

#include <bw/storage/constants.h>
#include <bw/storage/counters.h>
#include <bw/storage/flash_fs.h>
#include <bw/storage/record_codec.h>

namespace bw::storage {

// [DERIVED] Structural budget for sizeof(SegmentManager) (F-02 audit): index
// (16 x 20 B = 320) + patch cache (16 x 8 B = 128) + watermarks/refs + the
// two NEW-M1 test-seam pointers (16 B on host) ≈ 540 B, so 600 B leaves ~60 B
// of headroom while staying far below the ~800 B the class would occupy if
// the 76 B + 204 B decode scratch lived here again.
// The scratch MUST stay on the caller's stack (thread model above); a test
// enforces this budget at runtime. Raise only with a stated reason for what
// state was added and why sharing it is safe.
constexpr size_t kSegmentManagerStateBudgetBytes = 600;

// Record-dispatch visitor for scanFrom() (D8): events in ascending seq order,
// patches and sync markers as found. Return false from onEvent to stop early.
struct ScanVisitor {
    virtual bool onEvent(const bw::Event& event) = 0;
    virtual void onPatch(uint32_t seq, bw::UserLabel label) {
        (void)seq;
        (void)label;
    }
    virtual void onMarker(uint32_t seq) { (void)seq; }

protected:
    ~ScanVisitor() = default;
};

class SegmentManager {
public:
    SegmentManager(IFlashFs& fs, StorageCounters& counters);

    // Scans every segment slot, repairs torn tails, builds the in-RAM index,
    // fills the label cache and the seq/marker watermarks. Idempotent:
    // calling it again over an already-mounted fs yields the same state.
    Result<void> mount();

    size_t segmentCount() const { return count_; }
    uint32_t nextSeq() const { return next_seq_; }
    uint32_t lastEventSeq() const { return last_event_seq_; }
    uint32_t lastMarkerSeq() const { return last_marker_seq_; }
    uint32_t currentUsedBytes() const { return current_used_; }
    uint32_t evictedSyncedCount() const { return counters_.evicted_synced_segments; }

    // Latest CACHED tail patch for seq (D4: labels are tail patches). Returns
    // false when no patch is cached for seq — the caller then keeps the label
    // as persisted on the event record itself (F-10: a cache miss must not
    // fabricate "Unlabeled" over an in-band label).
    bool patchFor(uint32_t seq, bw::UserLabel& out) const;

    // Appends an Event record (assigns nothing: seq is validated by the
    // caller, see LittleFsEventStore::append). Rotates/evicts as needed.
    // Err::Full = log full and nothing evictable (D6: unsynced never dropped).
    Result<void> appendEvent(const bw::Event& event, uint32_t last_synced_seq);

    // Appends a LabelPatch/SyncMarker record (meta records never change the
    // event seq watermark). Creates the first segment when the log is empty.
    Result<void> appendMeta(RecordType type, const uint8_t* payload, size_t len,
                            uint32_t last_synced_seq);

    // RAM-side pre-check for IEventStore::append's synchronous Err::Full
    // (CR-004: refuse, nothing consumed): true when the next event record
    // could be persisted right now (space in current segment, or an unused
    // slot, or an evictable fully-synced segment).
    bool canPersistEvent(uint32_t last_synced_seq) const;

    // Reserves the next event seq at ENQUEUE time (D3): the pump persists the
    // reserved value later, so a failed write leaves a counted gap instead of
    // a duplicate (gaps are legal — D8 readFrom skips missing seqs).
    void reserveSeq() { ++next_seq_; }

    // Applies a label patch to the RAM cache without scanning (runtime path;
    // the patch record itself is persisted by appendMeta).
    void notePatch(uint32_t seq, bw::UserLabel label) { cachePatch(seq, label); }

    // Test-only seams (audit NEW-M1): the write hook fires inside cachePatch's
    // ring-republish window (after the entry is invalidated, before its new
    // label/seq land) and the read hook fires inside patchFor between its two
    // seq/label loads — the exact interleavings a concurrent task could take.
    // The host suite uses them to pin the publish/validate protocol
    // deterministically; production always leaves them nullptr.
    using PatchHook = void (*)();
    void setPatchWriteHookForTest(PatchHook hook) { patch_write_hook_ = hook; }
    void setPatchReadHookForTest(PatchHook hook) { patch_read_hook_ = hook; }

    // Iterates event records with seq >= from_seq in ascending order,
    // dispatching to the visitor; patches/markers in the walked range are
    // dispatched too. Read-only (no repair) — safe from the CommsTask.
    Result<void> scanFrom(uint32_t from_seq, ScanVisitor& visitor);

private:
    struct SegmentInfo {
        uint8_t slot;
        uint32_t serial;      // creation order; the scan/eviction ordering key
        uint32_t first_seq;   // first event seq, 0 when the segment holds none
        uint32_t last_seq;    // last event seq, 0 when the segment holds none
        uint32_t used_bytes;  // header + valid records
    };

    // Mount-time visitor: fills the label cache and marker watermark.
    struct MountCollector final : ScanVisitor {
        SegmentManager* self;
        bool onEvent(const bw::Event&) override { return true; }
        void onPatch(uint32_t seq, bw::UserLabel label) override;
        void onMarker(uint32_t seq) override;
    };

    void segmentPath(uint8_t slot, char* out, size_t cap) const;

    Result<void> scanSegment(uint8_t slot, bool allow_repair);
    // Walks the record stream of one segment from byte `start`; used by both
    // scanSegment (repairing, watermark-updating) and scanFrom (read-only).
    // Returns false when the visitor asked to stop early.
    struct WalkOptions {
        bool allow_repair;           // truncate torn tails / bump counters
        bool update_watermark;       // advance last_event_seq_/first/last_seq
        uint32_t min_seq_exclusive;  // event seqs must be strictly greater
    };
    Result<bool> walkSegment(const SegmentInfo& info, size_t start, const WalkOptions& opt,
                             ScanVisitor& visitor);
    // Byte-wise forward search for the next valid record in [from, info_end).
    // Returns absolute offset or 0 when none (within the resync budget).
    size_t searchNextValidRecord(const char* path, size_t from, size_t end);

    void insertIndex(const SegmentInfo& info);  // keeps index_ sorted by serial
    int findSlotIndex(uint8_t slot) const;

    // Rotation (D6): picks an unused slot or evicts the oldest fully-synced
    // segment; writes the new segment header; Err::Full when nothing evictable.
    Result<void> rotate(uint32_t last_synced_seq);
    bool evictOldestSynced(uint32_t last_synced_seq);
    Result<void> ensureCurrent(uint32_t last_synced_seq);  // current segment exists

    void cachePatch(uint32_t seq, bw::UserLabel label);

    IFlashFs& fs_;
    StorageCounters& counters_;

    SegmentInfo index_[kMaxSegments];
    size_t count_ = 0;  // segments in index_
    uint8_t current_slot_ = 0;
    bool has_current_ = false;
    uint32_t current_used_ = 0;
    uint32_t serial_next_ = 1;

    uint32_t next_seq_ = 1;
    uint32_t last_event_seq_ = 0;
    uint32_t last_marker_seq_ = 0;
    uint32_t resync_budget_used_ = 0;  // reset per mount()

    struct PatchEntry {
        uint32_t seq;
        bw::UserLabel label;
    };
    PatchEntry patch_cache_[kLabelPatchCacheEntries];
    size_t patch_count_ = 0;
    size_t patch_head_ = 0;                 // ring: next slot to overwrite when full
    PatchHook patch_write_hook_ = nullptr;  // NEW-M1 test seams; the public
    PatchHook patch_read_hook_ = nullptr;   // setters explain them

    // No scratch buffers here (F-02): walkSegment/searchNextValidRecord/
    // appendEvent/appendMeta each use a per-call stack buffer, sized by
    // kSegmentManagerStateBudgetBytes — see the thread-model note on top.
};

}  // namespace bw::storage
