#include <bw/storage/record_codec.h>

#include <cstring>

namespace bw::storage {

namespace {

void putU32(uint8_t* out, uint32_t v) {
    out[0] = static_cast<uint8_t>(v & 0xFF);
    out[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    out[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

uint32_t getU32(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

bool isKnownType(uint8_t raw) {
    return raw == static_cast<uint8_t>(RecordType::Event) ||
           raw == static_cast<uint8_t>(RecordType::LabelPatch) ||
           raw == static_cast<uint8_t>(RecordType::SyncMarker);
}

}  // namespace

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
    // Reflected bitwise CRC-32/IEEE: polynomial 0xEDB88320. Chosen over a
    // nibble/table variant for obvious correctness; record sizes (<= 72 B)
    // make the 8-iterations-per-byte cost negligible for the pump.
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

uint32_t crc32(const uint8_t* data, size_t len) {
    return crc32Update(0, data, len);
}

size_t encodeRecord(RecordType type, const uint8_t* payload, size_t len, uint8_t* out) {
    if (len > kMaxRecordPayloadBytes || len > 0xFF || payload == nullptr || out == nullptr) {
        return 0;
    }
    if (!isKnownType(static_cast<uint8_t>(type))) {
        return 0;
    }
    putU32(out, kRecordMagic);
    out[4] = static_cast<uint8_t>(type);
    out[5] = static_cast<uint8_t>(len);
    out[6] = 0;  // reserved, must be zero (kept for future header extension)
    out[7] = 0;
    std::memcpy(out + kRecordHeaderBytes, payload, len);
    // CRC last over header + payload (D4): the bytes appended to the file
    // carry their own validity stamp in the same write() call.
    const uint32_t crc = crc32(out, kRecordHeaderBytes + len);
    putU32(out + kRecordHeaderBytes + len, crc);
    return kRecordHeaderBytes + len + kRecordCrcBytes;
}

Result<DecodedRecord> decodeRecord(const uint8_t* buf, size_t avail) {
    DecodedRecord rec{};
    rec.type = RecordType::None;
    rec.payload = nullptr;
    rec.len = 0;
    rec.consumed = 0;
    if (buf == nullptr || avail < kRecordHeaderBytes) {
        return {bw::Err::Corrupt, rec};
    }
    if (getU32(buf) != kRecordMagic) {
        return {bw::Err::Corrupt, rec};
    }
    const uint8_t raw_type = buf[4];
    const uint8_t len = buf[5];
    if (!isKnownType(raw_type) || buf[6] != 0 || buf[7] != 0 || len > kMaxRecordPayloadBytes) {
        return {bw::Err::Corrupt, rec};
    }
    const size_t total = kRecordHeaderBytes + len + kRecordCrcBytes;
    if (avail < total) {
        return {bw::Err::Corrupt, rec};  // torn tail candidate: record does not fit
    }
    const uint32_t want = getU32(buf + kRecordHeaderBytes + len);
    const uint32_t got = crc32(buf, kRecordHeaderBytes + len);
    if (want != got) {
        return {bw::Err::Corrupt, rec};
    }
    rec.type = static_cast<RecordType>(raw_type);
    rec.payload = buf + kRecordHeaderBytes;
    rec.len = len;
    rec.consumed = total;
    return {bw::Err::Ok, rec};
}

bool looksLikeRecordHeader(const uint8_t* buf, size_t avail, RecordType& type, uint8_t& len) {
    if (buf == nullptr || avail < kRecordHeaderBytes || getU32(buf) != kRecordMagic) {
        return false;
    }
    if (!isKnownType(buf[4]) || buf[6] != 0 || buf[7] != 0 || buf[5] > kMaxRecordPayloadBytes) {
        return false;
    }
    type = static_cast<RecordType>(buf[4]);
    len = buf[5];
    return true;
}

}  // namespace bw::storage
