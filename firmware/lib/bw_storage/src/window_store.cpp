#include <bw/storage/window_store.h>

#include <cstdio>
#include <cstring>

#include <bw/storage/record_codec.h>

namespace bw::storage {

namespace {

void putU16LE(uint8_t* out, uint16_t v) {
    out[0] = static_cast<uint8_t>(v & 0xFF);
    out[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

uint16_t getU16LE(const uint8_t* in) {
    return static_cast<uint16_t>(in[0]) | (static_cast<uint16_t>(in[1]) << 8);
}

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

}  // namespace

WindowStore::WindowStore(IFlashFs& fs, StorageCounters& counters) : fs_(fs), counters_(counters) {
    std::memset(slots_, 0, sizeof(slots_));
}

void WindowStore::slotPath(uint8_t slot, char* out, size_t cap) const {
    std::snprintf(out, cap, "w_%02u.bin", static_cast<unsigned>(slot));
}

int WindowStore::findSeq(uint32_t seq) const {
    for (size_t i = 0; i < count_; ++i) {
        if (slots_[i].seq == seq) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int WindowStore::findOldest() const {
    int oldest = -1;
    for (size_t i = 0; i < count_; ++i) {
        if (oldest < 0 || slots_[i].seq < slots_[static_cast<size_t>(oldest)].seq) {
            oldest = static_cast<int>(i);
        }
    }
    return oldest;
}

Result<void> WindowStore::mount() {
    count_ = 0;
    for (uint8_t slot = 0; slot < kMaxStoredWindows; ++slot) {
        char path[kMaxPathBytes];
        slotPath(slot, path, sizeof(path));
        if (!fs_.exists(path)) {
            continue;
        }
        uint8_t hdr[kWindowHeaderBytes];
        const Result<size_t> rd = fs_.read(path, 0, hdr, sizeof(hdr));
        const Result<uint32_t> sz = fs_.size(path);
        if (!rd.ok() || !sz.ok()) {
            // Transient IO on the probe (F-04, same root cause as the segment
            // scan): a failed READ is not proof of corruption — keep the blob
            // for the next boot, never delete on read failure. Counted in
            // corrupt_records because CR-015 has no transient-IO field: an
            // unreadable blob must stay visible, and nothing is destroyed.
            counters_.corrupt_records++;
            continue;
        }
        const bool header_ok = rd.value == sizeof(hdr) && getU32LE(hdr) == kWindowMagic &&
                               hdr[4] == kWindowSchemaVersion && hdr[5] == 0;
        if (!header_ok) {
            counters_.corrupt_records++;  // torn window blob discarded (§10)
            (void)fs_.remove(path);
            continue;
        }
        const uint16_t samples = getU16LE(hdr + 6);
        const uint32_t seq = getU32LE(hdr + 8);
        const uint32_t expected =
            kWindowHeaderBytes + static_cast<uint32_t>(samples) * 2 + kWindowCrcBytes;
        if (samples == 0 || static_cast<uint32_t>(samples) * 2 > kRawWindowBytes ||
            sz.value != expected) {
            // Size mismatch = the CRC-last write never landed (torn): discard.
            counters_.corrupt_records++;
            (void)fs_.remove(path);
            continue;
        }
        if (count_ < kMaxStoredWindows) {
            slots_[count_].slot = slot;
            slots_[count_].seq = seq;
            slots_[count_].count = samples;
            ++count_;
        }
    }
    return {bw::Err::Ok};
}

Result<void> WindowStore::writeBlob(uint8_t slot, uint32_t seq, const int16_t* samples,
                                    size_t count) {
    char path[kMaxPathBytes];
    slotPath(slot, path, sizeof(path));
    uint8_t hdr[kWindowHeaderBytes];
    putU32LE(hdr, kWindowMagic);
    hdr[4] = kWindowSchemaVersion;
    hdr[5] = 0;
    putU16LE(hdr + 6, static_cast<uint16_t>(count));
    putU32LE(hdr + 8, seq);

    // Header + payload first, CRC last (D4 rule applied per blob): any tear
    // leaves a short file or a bad CRC, both rejected by mount()/getWindow().
    const Result<size_t> wr_hdr = fs_.write(path, 0, hdr, sizeof(hdr));
    if (!wr_hdr.ok() || wr_hdr.value != sizeof(hdr)) {
        return {wr_hdr.ok() ? bw::Err::HwFault : wr_hdr.err};
    }
    uint32_t crc = crc32Update(0, hdr, sizeof(hdr));
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(samples);
    const size_t payload_bytes = count * 2;
    size_t offset = 0;
    while (offset < payload_bytes) {
        const size_t want =
            (payload_bytes - offset) < kIoChunkBytes ? (payload_bytes - offset) : kIoChunkBytes;
        const Result<size_t> wr =
            fs_.write(path, kWindowHeaderBytes + offset, bytes + offset, want);
        if (!wr.ok() || wr.value != want) {
            return {wr.ok() ? bw::Err::HwFault : wr.err};
        }
        crc = crc32Update(crc, bytes + offset, want);
        offset += want;
    }
    uint8_t crc_buf[kWindowCrcBytes];
    putU32LE(crc_buf, crc);
    const Result<size_t> wr_crc =
        fs_.write(path, kWindowHeaderBytes + payload_bytes, crc_buf, sizeof(crc_buf));
    if (!wr_crc.ok() || wr_crc.value != sizeof(crc_buf)) {
        return {wr_crc.ok() ? bw::Err::HwFault : wr_crc.err};
    }
    return {bw::Err::Ok};
}

Result<void> WindowStore::putWindow(uint32_t seq, const int16_t* samples, size_t count) {
    if (seq == 0 || samples == nullptr || count == 0 || count * 2 > kRawWindowBytes ||
        count > 0xFFFF) {
        return {bw::Err::InvalidArg};
    }
    auto remove_entry = [this](size_t idx) {
        for (size_t i = idx; i + 1 < count_; ++i) {
            slots_[i] = slots_[i + 1];
        }
        --count_;
    };

    int table_idx = findSeq(seq);
    uint8_t slot;
    if (table_idx >= 0) {
        slot = slots_[static_cast<size_t>(table_idx)].slot;  // idempotent re-put
    } else if (count_ < kMaxStoredWindows) {
        // First slot not in the table (mount removed any stale file there).
        slot = kMaxStoredWindows;
        for (uint8_t s = 0; s < kMaxStoredWindows; ++s) {
            bool used = false;
            for (size_t i = 0; i < count_; ++i) {
                if (slots_[i].slot == s) {
                    used = true;
                    break;
                }
            }
            if (!used) {
                slot = s;
                break;
            }
        }
    } else {
        // Count limit (D6 step 2): evict the oldest, reuse its slot.
        const int oldest = findOldest();
        char old_path[kMaxPathBytes];
        slotPath(slots_[static_cast<size_t>(oldest)].slot, old_path, sizeof(old_path));
        (void)fs_.remove(old_path);
        counters_.evicted_windows++;
        slot = slots_[static_cast<size_t>(oldest)].slot;
        remove_entry(static_cast<size_t>(oldest));
    }

    Result<void> r = writeBlob(slot, seq, samples, count);
    if (!r.ok() && r.err == bw::Err::Full) {
        // Partition pressure beyond the count limit: drop the oldest OTHER
        // window once, retry; still Full -> the caller faults (§10).
        char path[kMaxPathBytes];
        slotPath(slot, path, sizeof(path));
        (void)fs_.remove(path);
        const int oldest = findOldest();
        if (oldest >= 0 && slots_[static_cast<size_t>(oldest)].seq != seq) {
            char old_path[kMaxPathBytes];
            slotPath(slots_[static_cast<size_t>(oldest)].slot, old_path, sizeof(old_path));
            (void)fs_.remove(old_path);
            counters_.evicted_windows++;
            remove_entry(static_cast<size_t>(oldest));
            r = writeBlob(slot, seq, samples, count);
        }
    }
    if (!r.ok()) {
        char path[kMaxPathBytes];
        slotPath(slot, path, sizeof(path));
        (void)fs_.remove(path);
        if (r.err == bw::Err::HwFault) {
            // The partial blob was removed here (runtime repair) — the same
            // accounting a boot-scan truncation would produce (D4).
            counters_.torn_tails_repaired++;
        }
        // F-06: the eviction ladder is exhausted and the store refuses the
        // window (§10's "the caller faults" made real): surface it to F23 —
        // without this the refusal was visible only to a caller that may
        // ignore the Result. Warning, not Error: event storage still works,
        // this source is downgraded (file convention, same as the reader).
        if (faults_ != nullptr) {
            faults_->report(bw::FaultCode::StorageDegraded, bw::FaultSeverity::Warning, 0);
        }
        return r;
    }
    table_idx = findSeq(seq);
    if (table_idx < 0) {
        table_idx = static_cast<int>(count_);
        slots_[count_].slot = slot;
        ++count_;
    }
    slots_[static_cast<size_t>(table_idx)].seq = seq;
    slots_[static_cast<size_t>(table_idx)].count = static_cast<uint16_t>(count);
    return {bw::Err::Ok};
}

Result<size_t> WindowStore::getWindow(uint32_t seq, int16_t* out, size_t max_samples) {
    const int idx = findSeq(seq);
    if (idx < 0) {
        return {bw::Err::NotFound, 0};  // evicted or never written: honest unknown
    }
    const SlotInfo& info = slots_[static_cast<size_t>(idx)];
    char path[kMaxPathBytes];
    slotPath(info.slot, path, sizeof(path));
    const size_t payload_bytes = static_cast<size_t>(info.count) * 2;
    if (info.count > max_samples || out == nullptr) {
        return {bw::Err::Full, 0};  // caller buffer too small; nothing consumed
    }
    // Stream + CRC-validate on the way out: a blob that rotted since mount is
    // Corrupt (counted), never silently served.
    uint8_t hdr[kWindowHeaderBytes];
    const Result<size_t> rd_hdr = fs_.read(path, 0, hdr, sizeof(hdr));
    if (!rd_hdr.ok() || rd_hdr.value != sizeof(hdr)) {
        counters_.corrupt_records++;
        return {bw::Err::Corrupt, 0};
    }
    uint32_t crc = crc32Update(0, hdr, sizeof(hdr));
    uint8_t chunk[kIoChunkBytes];
    uint8_t* bytes = reinterpret_cast<uint8_t*>(out);
    size_t offset = 0;
    while (offset < payload_bytes) {
        const size_t want =
            (payload_bytes - offset) < sizeof(chunk) ? (payload_bytes - offset) : sizeof(chunk);
        const Result<size_t> rd = fs_.read(path, kWindowHeaderBytes + offset, chunk, want);
        if (!rd.ok() || rd.value != want) {
            counters_.corrupt_records++;
            return {bw::Err::Corrupt, 0};
        }
        crc = crc32Update(crc, chunk, want);
        std::memcpy(bytes + offset, chunk, want);
        offset += want;
    }
    uint8_t stored_crc[kWindowCrcBytes];
    const Result<size_t> rd_crc =
        fs_.read(path, kWindowHeaderBytes + payload_bytes, stored_crc, sizeof(stored_crc));
    if (!rd_crc.ok() || rd_crc.value != sizeof(stored_crc) || getU32LE(stored_crc) != crc) {
        counters_.corrupt_records++;
        if (faults_ != nullptr) {
            faults_->report(bw::FaultCode::StorageDegraded, bw::FaultSeverity::Warning, 0);
        }
        return {bw::Err::Corrupt, 0};
    }
    return {bw::Err::Ok, info.count};
}

}  // namespace bw::storage
