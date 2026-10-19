#include <bw/storage/segment_manager.h>

#include <cstdio>
#include <cstring>

namespace bw::storage {

static_assert(sizeof(bw::Event) == kEventBytes, "Event layout pinned by CR-002/contract_checks.h");

namespace {

// Segment header layout (kSegmentHeaderBytes): [0..3] magic, [4] format
// version, [5..7] reserved zero, [8..11] serial (creation order — the scan
// and eviction ordering key; first_seq is deliberately NOT persisted, D4),
// [12..15] reserved zero. Written exactly once at segment creation.
constexpr size_t kSerialOffsetBytes = 8;

// Audit NEW-M1: sentinel written into PatchEntry::seq while an entry is
// being republished by cachePatch. Real event seqs start at 1 (next_seq_
// watermark), so 0 means "slot mid-update, do not read" — a concurrent
// patchFor can never match it and instead keeps the event's own label (F-10).
constexpr uint32_t kInvalidPatchSeq = 0;

void putU32LE(uint8_t* out, uint32_t v) {
    out[0] = static_cast<uint8_t>(v & 0xFF);
    out[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

uint32_t getU32LE(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

bool headerBytesZero(const uint8_t* buf) {
    return buf[5] == 0 && buf[6] == 0 && buf[7] == 0 && buf[12] == 0 && buf[13] == 0 &&
           buf[14] == 0 && buf[15] == 0;
}

bool validLabel(uint8_t raw) {
    return raw <= static_cast<uint8_t>(bw::UserLabel::Rejected);
}

}  // namespace

void SegmentManager::MountCollector::onPatch(uint32_t seq, bw::UserLabel label) {
    self->cachePatch(seq, label);
}

void SegmentManager::MountCollector::onMarker(uint32_t seq) {
    self->last_marker_seq_ = seq;
}

SegmentManager::SegmentManager(IFlashFs& fs, StorageCounters& counters)
    : fs_(fs), counters_(counters) {
    std::memset(index_, 0, sizeof(index_));
    std::memset(patch_cache_, 0, sizeof(patch_cache_));
    // F-02: no scratch members — every walk/append uses a stack buffer.
}

void SegmentManager::segmentPath(uint8_t slot, char* out, size_t cap) const {
    std::snprintf(out, cap, "evt_%03u.bin", static_cast<unsigned>(slot));
}

int SegmentManager::findSlotIndex(uint8_t slot) const {
    for (size_t i = 0; i < count_; ++i) {
        if (index_[i].slot == slot) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void SegmentManager::insertIndex(const SegmentInfo& info) {
    // Insertion-sorted by serial: at mount, segments are inserted in serial
    // order, so this appends; the rare unsorted insert stays correct.
    size_t pos = count_;
    while (pos > 0 && index_[pos - 1].serial > info.serial) {
        index_[pos] = index_[pos - 1];
        --pos;
    }
    index_[pos] = info;
    if (count_ < kMaxSegments) {
        ++count_;
    }
}

bool SegmentManager::patchFor(uint32_t seq, bw::UserLabel& out) const {
    for (size_t i = 0; i < patch_count_; ++i) {
        const uint32_t entry_seq = patch_cache_[i].seq;
        // kInvalidPatchSeq = entry mid-publish (NEW-M1): it can never match a
        // real event seq, so such a slot is simply skipped.
        if (entry_seq != seq) {
            continue;
        }
        if (patch_read_hook_ != nullptr) {
            patch_read_hook_();  // test seam: a writer may run right here (NEW-M1)
        }
        const bw::UserLabel entry_label = patch_cache_[i].label;
        // NEW-M1 seqlock validate: cachePatch republishes entries with the
        // seq invalidated first and committed LAST, so if the seq changed
        // between our two loads, the label just read may belong to a
        // different seq — return a miss and let the caller keep the event's
        // own label instead of emitting a wrong pairing. On the real targets
        // both loads and the writer's same-address stores execute in program
        // order (x86-64 TSO; Xtensa LX6 in-order) — see the status log for
        // the acknowledged C++-abstract-machine caveat.
        if (patch_cache_[i].seq != entry_seq) {
            return false;
        }
        out = entry_label;
        return true;
    }
    return false;  // no cached patch: caller keeps the event's own label (F-10)
}

void SegmentManager::cachePatch(uint32_t seq, bw::UserLabel label) {
    for (size_t i = 0; i < patch_count_; ++i) {
        if (patch_cache_[i].seq == seq) {
            // Single label store, seq untouched (latest patch wins, D4): the
            // pair can never mix seqs, so no republish protocol is needed.
            patch_cache_[i].label = label;
            return;
        }
    }
    if (patch_count_ < kLabelPatchCacheEntries) {
        // Publication order (NEW-M1): label first, seq LAST as the commit
        // word, patch_count_ last of all — a slot only becomes visible to
        // patchFor once its pair is complete.
        patch_cache_[patch_count_].label = label;
        patch_cache_[patch_count_].seq = seq;
        ++patch_count_;
        return;
    }
    // Full ring: the OLDEST cached patch is dropped from RAM (it stays on
    // disk; documented limitation — see spec §15 status log). Republish it
    // with the NEW-M1 invalidate/commit protocol: the old entry is killed
    // with kInvalidPatchSeq BEFORE the new label lands, and the new seq is
    // written LAST — so a concurrent patchFor can observe only the old pair
    // or an invalidated slot, never {new seq, old label}, the wrong-label
    // pairing audit NEW-M1 identified. Packing the pair into one machine
    // word was rejected: {seq, label} needs 40 bits and Xtensa has no
    // 64-bit store (a uint64_t write compiles to two 32-bit stores — the
    // same cross-core hazard); snapshotting the ring under a lock was
    // rejected because correctness would then depend on every reader
    // remembering the guard, while this protocol validates at the read
    // site itself.
    patch_cache_[patch_head_].seq = kInvalidPatchSeq;
    if (patch_write_hook_ != nullptr) {
        patch_write_hook_();  // test seam: readers may run in this window (NEW-M1)
    }
    patch_cache_[patch_head_].label = label;
    patch_cache_[patch_head_].seq = seq;  // commit word
    patch_head_ = (patch_head_ + 1) % kLabelPatchCacheEntries;
}

Result<void> SegmentManager::mount() {
    count_ = 0;
    has_current_ = false;
    current_used_ = 0;
    serial_next_ = 1;
    next_seq_ = 1;
    last_event_seq_ = 0;
    last_marker_seq_ = 0;
    resync_budget_used_ = 0;
    patch_count_ = 0;
    patch_head_ = 0;

    // Pass 1: validate segment headers and order slots by serial.
    for (uint8_t slot = 0; slot < kMaxSegments; ++slot) {
        char path[kMaxPathBytes];
        segmentPath(slot, path, sizeof(path));
        // Stale copy-rename temp from an interrupted truncate repair (see
        // littlefs_adapter kCutSuffix): the original was left intact, so the
        // temp is safe to drop — this bounds the orphan-space leak to one
        // segment per slot, at most until the next mount.
        char cut[kMaxPathBytes];
        std::snprintf(cut, sizeof(cut), "evt_%03u.cut", static_cast<unsigned>(slot));
        (void)fs_.remove(cut);
        if (!fs_.exists(path)) {
            continue;
        }
        uint8_t hdr[kSegmentHeaderBytes];
        const Result<size_t> rd = fs_.read(path, 0, hdr, sizeof(hdr));
        const Result<uint32_t> sz = fs_.size(path);
        if (!rd.ok() || !sz.ok()) {
            // Transient IO on the probe (F-04): a failed READ is not proof of
            // corruption — keep the file and let the next boot retry. Skip in
            // place, counted; deleting here would destroy a healthy segment
            // over one bad read.
            counters_.segments_skipped++;
            continue;
        }
        if (rd.value != sizeof(hdr) || sz.value < kSegmentHeaderBytes ||
            getU32LE(hdr) != kSegmentMagic || hdr[4] != kSegmentFormatVersion ||
            !headerBytesZero(hdr)) {
            // Bad/erased header, verified by READING it (F-04): skip the
            // whole segment, never guess (D4/§10) — removing a header that
            // was actually read and found invalid cannot lose good data.
            counters_.segments_skipped++;
            (void)fs_.remove(path);
            continue;
        }
        SegmentInfo info{};
        info.slot = slot;
        info.serial = getU32LE(hdr + kSerialOffsetBytes);
        info.first_seq = 0;
        info.last_seq = 0;
        info.used_bytes = sz.value;
        insertIndex(info);
        if (info.serial >= serial_next_) {
            serial_next_ = info.serial + 1;
        }
    }

    // Pass 2: record scan in serial order (strictly-increasing seq validation
    // relies on this order), repairing torn tails as we go. Per-segment
    // failures are counted inside and skipped — mount never fails (§10).
    for (size_t i = 0; i < count_; ++i) {
        (void)scanSegment(index_[i].slot, /*allow_repair=*/true);
    }

    if (count_ > 0) {
        current_slot_ = index_[count_ - 1].slot;
        has_current_ = true;
        current_used_ = index_[count_ - 1].used_bytes;
        next_seq_ = last_event_seq_ + 1;
    }
    if (last_marker_seq_ > last_event_seq_) {
        last_marker_seq_ = last_event_seq_;  // clamp: cursor never exceeds data (D5)
    }
    return {bw::Err::Ok};
}

Result<void> SegmentManager::scanSegment(uint8_t slot, bool allow_repair) {
    const int idx = findSlotIndex(slot);
    if (idx < 0) {
        return {bw::Err::NotFound};
    }
    char path[kMaxPathBytes];
    segmentPath(slot, path, sizeof(path));
    WalkOptions opt{};
    opt.allow_repair = allow_repair;
    opt.update_watermark = true;
    opt.min_seq_exclusive = last_event_seq_;
    MountCollector collector{};
    collector.self = this;
    const Result<bool> walked =
        walkSegment(index_[static_cast<size_t>(idx)], kSegmentHeaderBytes, opt, collector);
    if (!walked.ok()) {
        // Resync budget exhausted or IO error: the remainder of this segment
        // is skipped — counted once, boot continues (§10: never brick).
        counters_.segments_skipped++;
    }
    return {bw::Err::Ok};
}

Result<bool> SegmentManager::walkSegment(const SegmentInfo& info, size_t start,
                                         const WalkOptions& opt, ScanVisitor& visitor) {
    char path[kMaxPathBytes];
    segmentPath(info.slot, path, sizeof(path));
    // F-02: per-call scratch — a member buffer would be raced by a
    // concurrent readFrom/pump pair; a stack buffer is per-walk private.
    uint8_t rec_scratch[kMaxRecordTotalBytes];
    const int live_idx = findSlotIndex(info.slot);
    if (live_idx < 0) {
        return {bw::Err::InvalidArg, true};
    }
    SegmentInfo& live = index_[static_cast<size_t>(live_idx)];  // repair may shrink used_bytes
    size_t offset = start;
    uint32_t prev_seq = opt.min_seq_exclusive;

    while (offset < live.used_bytes) {
        const size_t avail = live.used_bytes - offset;
        const size_t to_read = avail < kMaxRecordTotalBytes ? avail : kMaxRecordTotalBytes;
        const Result<size_t> rd = fs_.read(path, offset, rec_scratch, to_read);
        Result<DecodedRecord> dec = {bw::Err::Corrupt, DecodedRecord{}};
        if (rd.ok() && rd.value == to_read) {
            dec = decodeRecord(rec_scratch, to_read);
        }

        if (!dec.ok()) {
            if (!opt.allow_repair) {
                // Read-only walk (readFrom): resync to keep serving the rest.
                const size_t next = searchNextValidRecord(path, offset + 1, live.used_bytes);
                if (next == 0) {
                    return {bw::Err::Ok, true};  // tail garbage: just stop
                }
                offset = next;
                continue;
            }
            if (resync_budget_used_ >= kMaxCorruptResyncsPerBoot) {
                return {bw::Err::Unsupported, true};  // skip rest of segment
            }
            const size_t next = searchNextValidRecord(path, offset + 1, live.used_bytes);
            if (next == 0) {
                // Nothing valid after this point = torn tail (D2): truncate
                // back to the last good record boundary.
                if (offset < live.used_bytes) {
                    const Result<void> tr = fs_.truncate(path, offset);
                    if (!tr.ok()) {
                        return {bw::Err::HwFault, true};
                    }
                    counters_.torn_tails_repaired++;
                    live.used_bytes = offset;
                    if (live.slot == current_slot_) {
                        current_used_ = offset;
                    }
                }
                return {bw::Err::Ok, true};
            }
            // Valid data after a hole = mid-segment corruption (D4): skip the
            // hole, count it, keep the records after it.
            counters_.corrupt_records++;
            offset = next;
            continue;
        }

        const DecodedRecord& rec = dec.value;
        bool hole = false;
        switch (rec.type) {
        case RecordType::Event: {
            if (rec.len != kEventBytes) {
                hole = true;
                break;
            }
            bw::Event event{};
            std::memcpy(&event, rec.payload, kEventBytes);
            if (event.seq <= opt.min_seq_exclusive) {
                break;  // below the requested cursor (scanFrom): skip silently
            }
            if (event.seq <= prev_seq) {
                hole = true;  // duplicate/regressed seq: not data we trust
                break;
            }
            prev_seq = event.seq;
            if (opt.update_watermark) {
                if (live.first_seq == 0) {
                    live.first_seq = event.seq;
                }
                live.last_seq = event.seq;
                last_event_seq_ = event.seq;
            }
            if (!visitor.onEvent(event)) {
                return {bw::Err::Ok, false};
            }
            break;
        }
        case RecordType::LabelPatch: {
            if (rec.len != 5) {
                hole = true;
                break;
            }
            const uint32_t seq = getU32LE(rec.payload);
            if (!validLabel(rec.payload[4])) {
                hole = true;
                break;
            }
            visitor.onPatch(seq, static_cast<bw::UserLabel>(rec.payload[4]));
            break;
        }
        case RecordType::SyncMarker: {
            if (rec.len != 4) {
                hole = true;
                break;
            }
            visitor.onMarker(getU32LE(rec.payload));
            break;
        }
        default:
            hole = true;  // decodeRecord already rejects unknown types
            break;
        }

        if (hole) {
            if (!opt.allow_repair) {
                const size_t next = searchNextValidRecord(path, offset + 1, live.used_bytes);
                if (next == 0) {
                    return {bw::Err::Ok, true};
                }
                offset = next;
                continue;
            }
            if (resync_budget_used_ >= kMaxCorruptResyncsPerBoot) {
                return {bw::Err::Unsupported, true};
            }
            const size_t next = searchNextValidRecord(path, offset + 1, live.used_bytes);
            if (next == 0) {
                const Result<void> tr = fs_.truncate(path, offset);
                if (!tr.ok()) {
                    return {bw::Err::HwFault, true};
                }
                counters_.torn_tails_repaired++;
                live.used_bytes = offset;
                if (live.slot == current_slot_) {
                    current_used_ = offset;
                }
                return {bw::Err::Ok, true};
            }
            counters_.corrupt_records++;
            offset = next;
            continue;
        }

        offset += rec.consumed;
    }
    return {bw::Err::Ok, true};
}

size_t SegmentManager::searchNextValidRecord(const char* path, size_t from, size_t end) {
    if (resync_budget_used_ >= kMaxCorruptResyncsPerBoot || from >= end) {
        return 0;
    }
    // Window scan with overlap: a record starting anywhere in
    // [pos, pos + nread - kOverlap) is fully inside the window, so no
    // candidate is missed at an edge; candidates in the overlap tail are
    // re-examined in the next window, and a candidate that crosses `end`
    // cannot be a record at all (records never straddle used_bytes, D4).
    constexpr size_t kOverlap = kMaxRecordTotalBytes - 1;
    // F-02: per-call scratch (window + straddling record), never a member.
    uint8_t search_scratch[kIoChunkBytes + kMaxRecordTotalBytes];
    uint8_t rec_scratch[kMaxRecordTotalBytes];
    size_t pos = from;
    while (pos < end) {
        const size_t want =
            (end - pos) < sizeof(search_scratch) ? (end - pos) : sizeof(search_scratch);
        const Result<size_t> rd = fs_.read(path, pos, search_scratch, want);
        if (!rd.ok() || rd.value < kRecordHeaderBytes) {
            return 0;
        }
        const size_t nread = rd.value;
        for (size_t i = 0; i + kRecordHeaderBytes <= nread; ++i) {
            RecordType type = RecordType::None;
            uint8_t len = 0;
            if (!looksLikeRecordHeader(search_scratch + i, nread - i, type, len)) {
                continue;
            }
            if (resync_budget_used_ >= kMaxCorruptResyncsPerBoot) {
                return 0;  // candidate budget exhausted: bounded boot work (D4)
            }
            resync_budget_used_++;  // one candidate examined
            const size_t total = kRecordHeaderBytes + len + kRecordCrcBytes;
            if (i + total <= nread) {
                if (decodeRecord(search_scratch + i, nread - i).ok()) {
                    return pos + i;
                }
            } else if (pos + i + total <= end) {
                // Straddles the window edge but fits before `end`: pull the
                // full record in and validate it there.
                const Result<size_t> full = fs_.read(path, pos + i, rec_scratch, total);
                if (full.ok() && full.value == total && decodeRecord(rec_scratch, total).ok()) {
                    return pos + i;
                }
            }
        }
        if (pos + nread >= end) {
            return 0;  // final window: the whole range has been examined
        }
        pos += nread > kOverlap ? nread - kOverlap : nread;
    }
    return 0;
}

bool SegmentManager::evictOldestSynced(uint32_t last_synced_seq) {
    // D6 order step 1: only fully-synced segments (last_seq <= cursor, a
    // marker-only segment trivially qualifies) — unsynced data is never
    // dropped silently. Pick the eligible segment with the oldest serial.
    int best = -1;
    for (size_t i = 0; i < count_; ++i) {
        if (index_[i].last_seq > last_synced_seq) {
            continue;
        }
        if (best < 0 || index_[i].serial < index_[best].serial) {
            best = static_cast<int>(i);
        }
    }
    if (best < 0) {
        return false;
    }
    char path[kMaxPathBytes];
    segmentPath(index_[static_cast<size_t>(best)].slot, path, sizeof(path));
    if (!fs_.remove(path).ok()) {
        // Unremovable segment: dropping it from the index would leak the
        // file; treat as not evictable so the caller faults (§10).
        return false;
    }
    counters_.evicted_synced_segments++;
    for (size_t i = static_cast<size_t>(best); i + 1 < count_; ++i) {
        index_[i] = index_[i + 1];
    }
    --count_;
    return true;
}

Result<void> SegmentManager::rotate(uint32_t last_synced_seq) {
    has_current_ = false;
    int chosen = -1;
    for (uint8_t slot = 0; slot < kMaxSegments; ++slot) {
        if (findSlotIndex(slot) < 0) {
            chosen = slot;
            break;
        }
    }
    if (chosen < 0) {
        if (!evictOldestSynced(last_synced_seq)) {
            return {bw::Err::Full};  // everything is unsynced: refuse (§5/§10)
        }
        for (uint8_t slot = 0; slot < kMaxSegments; ++slot) {
            if (findSlotIndex(slot) < 0) {
                chosen = slot;
                break;
            }
        }
        if (chosen < 0) {
            return {bw::Err::Full};
        }
    }

    char path[kMaxPathBytes];
    segmentPath(static_cast<uint8_t>(chosen), path, sizeof(path));
    uint8_t hdr[kSegmentHeaderBytes] = {0};
    putU32LE(hdr, kSegmentMagic);
    hdr[4] = kSegmentFormatVersion;
    putU32LE(hdr + kSerialOffsetBytes, serial_next_++);
    const Result<size_t> wr = fs_.write(path, 0, hdr, sizeof(hdr));
    if (!wr.ok() || wr.value != sizeof(hdr)) {
        return {bw::Err::HwFault};
    }
    SegmentInfo info{};
    info.slot = static_cast<uint8_t>(chosen);
    info.serial = serial_next_ - 1;
    info.first_seq = 0;
    info.last_seq = 0;
    info.used_bytes = kSegmentHeaderBytes;
    insertIndex(info);
    current_slot_ = info.slot;
    current_used_ = info.used_bytes;
    has_current_ = true;
    return {bw::Err::Ok};
}

Result<void> SegmentManager::ensureCurrent(uint32_t last_synced_seq) {
    if (has_current_) {
        return {bw::Err::Ok};
    }
    return rotate(last_synced_seq);
}

bool SegmentManager::canPersistEvent(uint32_t last_synced_seq) const {
    const size_t record = kEventRecordBytes;
    if (has_current_ && current_used_ + record <= kSegmentSizeBytes) {
        return true;
    }
    if (count_ < kMaxSegments) {
        return true;  // an unused slot exists (or the log is empty)
    }
    for (size_t i = 0; i < count_; ++i) {
        if (index_[i].last_seq <= last_synced_seq) {
            return true;  // rotation would evict this synced segment
        }
    }
    return false;
}

Result<void> SegmentManager::appendEvent(const bw::Event& event, uint32_t last_synced_seq) {
    bw::Result<void> ensure = ensureCurrent(last_synced_seq);
    if (!ensure.ok()) {
        return ensure;
    }
    if (current_used_ + kEventRecordBytes > kSegmentSizeBytes) {
        // Records never straddle segments (D4): rotate first.
        ensure = rotate(last_synced_seq);
        if (!ensure.ok()) {
            return ensure;
        }
    }
    // F-02: per-call encode buffer (pump's task stack, ~76 B).
    uint8_t rec_scratch[kMaxRecordTotalBytes];
    const size_t total = encodeRecord(RecordType::Event, reinterpret_cast<const uint8_t*>(&event),
                                      kEventBytes, rec_scratch);
    if (total != kEventRecordBytes) {
        return {bw::Err::InvalidArg};
    }
    char path[kMaxPathBytes];
    segmentPath(current_slot_, path, sizeof(path));
    // ONE write call per record (D4/T3): the byte-offset fault-injection
    // sweep is well-defined and a torn append leaves an invalid CRC tail.
    const Result<size_t> wr = fs_.write(path, current_used_, rec_scratch, total);
    if (!wr.ok() || wr.value != total) {
        // Repair the (possibly partial) tail immediately so the next append
        // never lands after garbage; if the repair itself fails, the segment
        // is abandoned in place and the boot scan truncates it (§10). The
        // counter counts REAL repairs only: a write that committed nothing
        // (fault before any byte) has no garbage to remove.
        const Result<uint32_t> sz = fs_.size(path);
        if (sz.ok() && sz.value > current_used_ && fs_.truncate(path, current_used_).ok()) {
            counters_.torn_tails_repaired++;
        }
        return {wr.ok() ? bw::Err::HwFault : wr.err};
    }
    current_used_ += total;
    SegmentInfo& live = index_[findSlotIndex(current_slot_)];
    live.used_bytes = current_used_;
    if (live.first_seq == 0) {
        live.first_seq = event.seq;
    }
    live.last_seq = event.seq;
    last_event_seq_ = event.seq;
    // F-01: next_seq_ is deliberately NOT assigned here. reserveSeq() at
    // ENQUEUE (D3) is the only runtime authority — the pump applies an op
    // after later ops may already have reserved their seqs, so writing
    // event.seq + 1 back would REWIND the watermark below reserved values
    // and cause seq reuse whenever a queued op failed (audit F-01).
    return {bw::Err::Ok};
}

Result<void> SegmentManager::appendMeta(RecordType type, const uint8_t* payload, size_t len,
                                        uint32_t last_synced_seq) {
    if (type != RecordType::LabelPatch && type != RecordType::SyncMarker) {
        return {bw::Err::InvalidArg};
    }
    // F-02: per-call encode buffer (pump's task stack, ~76 B).
    uint8_t rec_scratch[kMaxRecordTotalBytes];
    const size_t total = encodeRecord(type, payload, len, rec_scratch);
    if (total == 0) {
        return {bw::Err::InvalidArg};  // payload too large for the framing
    }
    bw::Result<void> ensure = ensureCurrent(last_synced_seq);
    if (!ensure.ok()) {
        return ensure;
    }
    if (current_used_ + total > kSegmentSizeBytes) {
        ensure = rotate(last_synced_seq);
        if (!ensure.ok()) {
            return ensure;
        }
    }
    char path[kMaxPathBytes];
    segmentPath(current_slot_, path, sizeof(path));
    const Result<size_t> wr = fs_.write(path, current_used_, rec_scratch, total);
    if (!wr.ok() || wr.value != total) {
        const Result<uint32_t> sz = fs_.size(path);
        if (sz.ok() && sz.value > current_used_ && fs_.truncate(path, current_used_).ok()) {
            counters_.torn_tails_repaired++;  // real garbage removed (see appendEvent)
        }
        return {wr.ok() ? bw::Err::HwFault : wr.err};
    }
    current_used_ += total;
    index_[findSlotIndex(current_slot_)].used_bytes = current_used_;
    if (type == RecordType::SyncMarker && len == 4) {
        last_marker_seq_ = getU32LE(payload);
    }
    return {bw::Err::Ok};
}

Result<void> SegmentManager::scanFrom(uint32_t from_seq, ScanVisitor& visitor) {
    const uint32_t min_seq = from_seq > 0 ? from_seq - 1 : 0;
    for (size_t i = 0; i < count_; ++i) {
        const SegmentInfo& info = index_[i];
        if (info.last_seq != 0 && info.last_seq < min_seq) {
            continue;  // fully below the requested cursor; no patches target later events
        }
        WalkOptions opt{};
        opt.allow_repair = false;
        opt.update_watermark = false;
        opt.min_seq_exclusive = min_seq;
        const Result<bool> walked = walkSegment(info, kSegmentHeaderBytes, opt, visitor);
        if (!walked.ok()) {
            return {walked.err};  // IO error only (read-only walk never repairs)
        }
        if (!walked.value) {
            break;  // visitor asked to stop
        }
    }
    return {bw::Err::Ok};
}

}  // namespace bw::storage
