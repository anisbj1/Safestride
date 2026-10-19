#pragma once

// bw/storage/window_store.h — WindowStore: int16-packed event-window blobs
// tied to seq (F18 spec §15.2, §8, §15.3 D6 step 2).
//
// Fixed slot files w_00.bin .. w_07.bin (kMaxStoredWindows): boot probes the
// slots, no directory listing needed anywhere in F18. File layout:
// {'F18W', seq, count, reserved} header + int16 payload + CRC32-last — the
// CRC arrives in a second write, so any tear (smaller-than-expected size or
// CRC mismatch) is detected and the blob is discarded, never served.
//
// Write context: pump/StorageTask context like every flash writer (§5). The
// read API exists for tests and for F20's future contract (spec §15.3 note);
// this wave ships write/evict only as a public seam.

#include <cstddef>
#include <cstdint>

#include <bw/fault.h>

#include <bw/storage/constants.h>
#include <bw/storage/counters.h>
#include <bw/storage/flash_fs.h>

namespace bw::storage {

class WindowStore {
public:
    static constexpr uint32_t kWindowMagic = 0x57383146;  // 'F18W'
    static constexpr uint8_t kWindowSchemaVersion = 1;
    static constexpr size_t kWindowHeaderBytes = 12;
    static constexpr size_t kWindowCrcBytes = 4;

    WindowStore(IFlashFs& fs, StorageCounters& counters);

    void attachFaultReporter(bw::IFaultReporter* reporter) { faults_ = reporter; }

    // Validates the fixed slots (header magic + exact expected size — a torn
    // write is detected cheaply without re-reading payloads) and builds the
    // RAM index; invalid slots are removed and counted (§10: never brick).
    Result<void> mount();

    // Stores one window; oldest-first eviction when the slot table is full
    // (counters_.evicted_windows) and one evict-and-retry when the fs is full
    // (D6 order step 2). count is in SAMPLES (not bytes).
    Result<void> putWindow(uint32_t seq, const int16_t* samples, size_t count);

    // Streams a window out, validating the CRC on the way. Err::NotFound =
    // seq not stored (evicted or never written) — an honest "unknown".
    Result<size_t> getWindow(uint32_t seq, int16_t* out, size_t max_samples);

    size_t storedCount() const { return count_; }
    bool contains(uint32_t seq) const { return findSeq(seq) >= 0; }

private:
    struct SlotInfo {
        uint8_t slot;
        uint32_t seq;
        uint16_t count;  // samples
    };

    void slotPath(uint8_t slot, char* out, size_t cap) const;
    int findSeq(uint32_t seq) const;
    int findOldest() const;  // slot with the smallest seq, or -1
    Result<void> writeBlob(uint8_t slot, uint32_t seq, const int16_t* samples, size_t count);

    IFlashFs& fs_;
    StorageCounters& counters_;
    bw::IFaultReporter* faults_ = nullptr;
    SlotInfo slots_[kMaxStoredWindows];
    size_t count_ = 0;
};

}  // namespace bw::storage
