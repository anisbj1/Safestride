#include <bw/storage/kv_store.h>

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

KvStore::KvStore(IFlashFs& fs, StorageCounters& counters) : fs_(fs), counters_(counters) {}

bool KvStore::validKey(const char* key) {
    if (key == nullptr || key[0] == '\0') {
        return false;
    }
    size_t len = 0;
    for (const char* p = key; *p != '\0'; ++p, ++len) {
        if (len >= kKvKeyMaxLen) {
            return false;
        }
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) {
            return false;
        }
    }
    return true;
}

void KvStore::keyPath(const char* key, char* out, size_t cap) const {
    std::snprintf(out, cap, "kv_%s.bin", key);
}

void KvStore::tmpPath(const char* key, char* out, size_t cap) const {
    std::snprintf(out, cap, "kv_%s.tmp", key);
}

Result<size_t> KvStore::get(const char* key, void* buf, size_t cap) {
    if (!validKey(key)) {
        return {bw::Err::InvalidArg, 0};
    }
    char path[kMaxPathBytes];
    keyPath(key, path, sizeof(path));
    if (!fs_.exists(path)) {
        return {bw::Err::NotFound, 0};
    }
    uint8_t hdr[kKvHeaderBytes];
    const Result<size_t> rd_hdr = fs_.read(path, 0, hdr, sizeof(hdr));
    if (!rd_hdr.ok() || rd_hdr.value != sizeof(hdr) || getU32LE(hdr) != kKvMagic ||
        hdr[4] != kKvSchemaVersion || hdr[5] != 0) {
        counters_.kv_discards++;
        if (faults_ != nullptr) {
            faults_->report(bw::FaultCode::StorageDegraded, bw::FaultSeverity::Warning, 0);
        }
        return {bw::Err::Corrupt, 0};
    }
    const uint16_t len = getU16LE(hdr + 6);
    if (len > kKvMaxValueBytes) {
        counters_.kv_discards++;
        return {bw::Err::Corrupt, 0};
    }
    if (len > cap) {
        // Caller's buffer too small: refused, nothing copied (CR-004 Full —
        // retry with a bigger buffer succeeds without anything consumed).
        return {bw::Err::Full, 0};
    }
    // Stream payload while CRC-ing (bounded stack: kIoChunkBytes, 00 §7.2).
    uint8_t chunk[kIoChunkBytes];
    uint32_t crc = crc32Update(0, hdr, sizeof(hdr));
    size_t offset = 0;
    uint8_t* out = static_cast<uint8_t*>(buf);
    while (offset < len) {
        const size_t want = (len - offset) < sizeof(chunk) ? (len - offset) : sizeof(chunk);
        const Result<size_t> rd = fs_.read(path, kKvHeaderBytes + offset, chunk, want);
        if (!rd.ok() || rd.value != want) {
            counters_.kv_discards++;
            return {bw::Err::Corrupt, 0};
        }
        crc = crc32Update(crc, chunk, want);
        if (out != nullptr) {
            std::memcpy(out + offset, chunk, want);
        }
        offset += want;
    }
    uint8_t stored_crc[kKvCrcBytes];
    const Result<size_t> rd_crc =
        fs_.read(path, kKvHeaderBytes + len, stored_crc, sizeof(stored_crc));
    if (!rd_crc.ok() || rd_crc.value != sizeof(stored_crc) || getU32LE(stored_crc) != crc) {
        // Torn/corrupt blob: counted and reported; the caller restarts from
        // defaults (storage design: "corrupt blob -> discard, log a fault").
        counters_.kv_discards++;
        if (faults_ != nullptr) {
            faults_->report(bw::FaultCode::StorageDegraded, bw::FaultSeverity::Warning, 0);
        }
        return {bw::Err::Corrupt, 0};
    }
    return {bw::Err::Ok, len};
}

Result<void> KvStore::put(const char* key, const void* data, size_t len) {
    if (!validKey(key) || (data == nullptr && len > 0) || len > kKvMaxValueBytes) {
        return {bw::Err::InvalidArg};
    }
    char tmp[kMaxPathBytes];
    char dst[kMaxPathBytes];
    tmpPath(key, tmp, sizeof(tmp));
    keyPath(key, dst, sizeof(dst));

    // Stale tmp from a previous power cut: removed first (we never read tmp).
    (void)fs_.remove(tmp);

    uint8_t hdr[kKvHeaderBytes];
    putU32LE(hdr, kKvMagic);
    hdr[4] = kKvSchemaVersion;
    hdr[5] = 0;
    putU16LE(hdr + 6, static_cast<uint16_t>(len));

    // Header + payload in one write, CRC last in a second one (D7): any tear
    // leaves an invalid tmp; the rename below never runs, old value intact.
    const Result<size_t> wr_hdr = fs_.write(tmp, 0, hdr, sizeof(hdr));
    if (!wr_hdr.ok() || wr_hdr.value != sizeof(hdr)) {
        (void)fs_.remove(tmp);
        if (wr_hdr.err != bw::Err::Full) {
            counters_.torn_tails_repaired++;  // partial tmp removed (D7 repair)
        }
        return {wr_hdr.ok() ? bw::Err::HwFault : wr_hdr.err};
    }
    uint32_t crc = crc32Update(0, hdr, sizeof(hdr));
    const uint8_t* in = static_cast<const uint8_t*>(data);
    size_t offset = 0;
    while (offset < len) {
        const size_t want = (len - offset) < kIoChunkBytes ? (len - offset) : kIoChunkBytes;
        const Result<size_t> wr = fs_.write(tmp, kKvHeaderBytes + offset, in + offset, want);
        if (!wr.ok() || wr.value != want) {
            (void)fs_.remove(tmp);
            if (wr.err != bw::Err::Full) {
                counters_.torn_tails_repaired++;
            }
            return {wr.ok() ? bw::Err::HwFault : wr.err};
        }
        crc = crc32Update(crc, in + offset, want);
        offset += want;
    }
    uint8_t crc_buf[kKvCrcBytes];
    putU32LE(crc_buf, crc);
    const Result<size_t> wr_crc = fs_.write(tmp, kKvHeaderBytes + len, crc_buf, sizeof(crc_buf));
    if (!wr_crc.ok() || wr_crc.value != sizeof(crc_buf)) {
        (void)fs_.remove(tmp);
        if (wr_crc.err != bw::Err::Full) {
            counters_.torn_tails_repaired++;
        }
        return {wr_crc.ok() ? bw::Err::HwFault : wr_crc.err};
    }

    // Atomic overwrite (littlefs rename semantics): old-or-new, never torn.
    const Result<void> renamed = fs_.rename(tmp, dst);
    if (!renamed.ok()) {
        (void)fs_.remove(tmp);
        return renamed;
    }
    return {bw::Err::Ok};
}

}  // namespace bw::storage
