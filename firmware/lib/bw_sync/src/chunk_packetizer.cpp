#include <bw/sync/chunk_packetizer.h>

#include <cstring>

#include <bw/sync/crc16.h>

namespace bw::sync {
namespace {

// Little-endian helpers: both ends are LE (Xtensa/x86/phone ARM, D8), and the
// wire format is offset-defined — never a struct cast.
void putU16(std::uint8_t* out, std::uint16_t v) {
    out[0] = static_cast<std::uint8_t>(v & 0xFFU);
    out[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
}

void putU32(std::uint8_t* out, std::uint32_t v) {
    out[0] = static_cast<std::uint8_t>(v & 0xFFU);
    out[1] = static_cast<std::uint8_t>((v >> 8) & 0xFFU);
    out[2] = static_cast<std::uint8_t>((v >> 16) & 0xFFU);
    out[3] = static_cast<std::uint8_t>((v >> 24) & 0xFFU);
}

std::uint16_t getU16(const std::uint8_t* in) {
    return static_cast<std::uint16_t>(in[0]) | static_cast<std::uint16_t>(in[1] << 8);
}

std::uint32_t getU32(const std::uint8_t* in) {
    return static_cast<std::uint32_t>(in[0]) | (static_cast<std::uint32_t>(in[1]) << 8) |
           (static_cast<std::uint32_t>(in[2]) << 16) | (static_cast<std::uint32_t>(in[3]) << 24);
}

}  // namespace

std::size_t recordsPerChunk(std::uint16_t usable_bytes) {
    if (usable_bytes < kMinSyncUsableBytes) {
        return 0;  // below floor: one whole record does not fit (mtu_too_low)
    }
    const std::size_t payload = usable_bytes - kChunkFramingBytes;
    std::size_t records = payload / kEventRecordBytes;
    if (records > kMaxEventsPerChunk) {
        records = kMaxEventsPerChunk;  // capped MTU: larger frames buy nothing (§8)
    }
    return records;
}

Result<std::size_t> encodeChunkFrame(std::uint16_t chunk_no, const bw::Event* events,
                                     std::size_t count, std::uint8_t* out, std::size_t cap) {
    if (count == 0 || count > kMaxEventsPerChunk || events == nullptr || out == nullptr) {
        return {Err::InvalidArg, 0};
    }
    const std::size_t frame_len = kChunkHeaderBytes + count * kEventRecordBytes + kChunkCrcBytes;
    if (cap < frame_len) {
        return {Err::InvalidArg, 0};  // caller bug: nothing written
    }
    putU16(out, chunk_no);
    // Records are the byte image of bw::Event (48 B, zero padding, CR-002):
    // copy verbatim; contract_checks.h pins the offsets this relies on.
    std::memcpy(out + kChunkHeaderBytes, events, count * kEventRecordBytes);
    const std::uint16_t crc = crc16CcittFalse(out, kChunkHeaderBytes + count * kEventRecordBytes);
    putU16(out + kChunkHeaderBytes + count * kEventRecordBytes, crc);
    return {Err::Ok, frame_len};
}

void encodeStatusFrame(std::uint32_t last_seq, std::uint32_t last_synced, std::uint8_t flags,
                       std::uint8_t out[kStatusFrameBytes]) {
    out[0] = static_cast<std::uint8_t>(kSyncProtocolVersion);
    out[1] = kStatusFrameType;
    putU32(out + 2, last_seq);
    putU32(out + 6, last_synced);
    out[10] = flags;
}

void encodeCursorFrame(const CursorRequest& request, std::uint8_t out[kCursorFrameBytes]) {
    out[0] = static_cast<std::uint8_t>(kSyncProtocolVersion);
    out[1] = kCursorFrameType;
    putU32(out + 2, request.from_seq);
    putU16(out + 6, request.max);
    putU32(out + 8, request.ack_seq);
}

Result<void> parseCursorFrame(const std::uint8_t* data, std::size_t len, CursorRequest* out) {
    if (data == nullptr || out == nullptr || len != kCursorFrameBytes) {
        return {Err::InvalidArg};  // wrong length: dropped per D7, never a NACK
    }
    if (data[0] != static_cast<std::uint8_t>(kSyncProtocolVersion) || data[1] != kCursorFrameType) {
        return {Err::InvalidArg};  // unknown version/type: dropped per D7
    }
    out->from_seq = getU32(data + 2);
    out->max = getU16(data + 6);
    out->ack_seq = getU32(data + 8);
    return {Err::Ok};
}

Result<std::size_t> decodeChunkFrame(const std::uint8_t* data, std::size_t len,
                                     std::uint16_t* chunk_no_out, bw::Event* out, std::size_t max) {
    if (data == nullptr || chunk_no_out == nullptr || out == nullptr) {
        return {Err::InvalidArg, 0};
    }
    if (len < kChunkFramingBytes + kEventRecordBytes ||
        (len - kChunkFramingBytes) % kEventRecordBytes != 0) {
        return {Err::InvalidArg, 0};  // record never split: len % 48 == 4 must hold
    }
    const std::size_t records = (len - kChunkFramingBytes) / kEventRecordBytes;
    if (records > max) {
        return {Err::Overflow, 0};  // caller buffer too small; nothing written
    }
    const std::uint16_t want = getU16(data + len - kChunkCrcBytes);
    const std::uint16_t got = crc16CcittFalse(data, len - kChunkCrcBytes);
    if (want != got) {
        return {Err::Corrupt, 0};
    }
    *chunk_no_out = getU16(data);
    std::memcpy(out, data + kChunkHeaderBytes, records * kEventRecordBytes);
    return {Err::Ok, records};
}

}  // namespace bw::sync
