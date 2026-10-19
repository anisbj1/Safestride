#pragma once

// bw/storage/record_codec.h — D4 record framing for the segmented binary log
// (DEC-007/DEC-012, F18 spec §15.3 D4).
//
// On-disk record layout (little-endian, no packing pragmas; every field is
// written and read byte-wise so host/xtensa endianness cannot drift):
//   [ RecordHeader 8 B ][ payload len B ][ CRC32 4 B ]
// The CRC covers header + payload and is written LAST within the single
// write() call that appends the record, so a torn append is detected by a
// missing/mismatched CRC and truncation is safe (spec §15.3 D2/D4).
//
// Record types: Event (payload = bw::Event verbatim, kEventBytes), LabelPatch
// (uint32 seq + uint8 UserLabel), SyncMarker (uint32 seq).

#include <cstddef>
#include <cstdint>

#include <bw/events.h>
#include <bw/result.h>

namespace bw::storage {

enum class RecordType : uint8_t {
    None = 0,  // reserved: a zeroed/erased region parses as None, never as data
    Event = 1,
    LabelPatch = 2,
    SyncMarker = 3,
    // append only within this lib's format version; unknown type = invalid
};

// 'F18R' little-endian — arbitrary but fixed magic (D4). Changing it is a
// format break requiring a version bump, not a silent edit.
constexpr uint32_t kRecordMagic = 0x52314638;

// Framing: header + max payload + CRC.
constexpr size_t kRecordHeaderBytes = 8;
constexpr size_t kRecordCrcBytes = 4;
constexpr size_t kMaxRecordPayloadBytes = 64;  // >= kEventBytes (48); codec checks

// Largest possible record on disk — the codec's scratch bound (header +
// max payload + CRC); callers size buffers with it, never with kEventBytes.
constexpr size_t kMaxRecordTotalBytes =
    kRecordHeaderBytes + kMaxRecordPayloadBytes + kRecordCrcBytes;

// Total on-disk sizes of the three payload kinds (tests use them as sweep
// bounds for the DoD byte-offset fault injection).
constexpr size_t kEventRecordBytes = kRecordHeaderBytes + kEventBytes + kRecordCrcBytes;
constexpr size_t kLabelPatchRecordBytes = kRecordHeaderBytes + 5 + kRecordCrcBytes;
constexpr size_t kSyncMarkerRecordBytes = kRecordHeaderBytes + 4 + kRecordCrcBytes;

// Streaming CRC-32 (IEEE 802.3, reflected, init/final-xor 0xFFFFFFFF),
// table-free: ~60 B per record keeps boot scan cost trivial and no lookup
// table in flash/RAM. Known-answer tested ("123456789" -> 0xCBF43926).
uint32_t crc32(const uint8_t* data, size_t len);
uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len);

// Encodes a full record into `out` (capacity >= kRecordHeaderBytes + len +
// kRecordCrcBytes). Returns total bytes written, or 0 on invalid input
// (unknown type, len > kMaxRecordPayloadBytes).
size_t encodeRecord(RecordType type, const uint8_t* payload, size_t len, uint8_t* out);

// Decodes the record at `buf` (up to `avail` bytes).
// On success: ok, `type`/`payload`/`len` are filled and the record's total
// size is returned via `consumed`. On ANY inconsistency (short buffer, bad
// magic, unknown type, CRC mismatch) returns Err::Corrupt with consumed = 0 —
// the caller decides between tail truncation and forward resync (D4).
struct DecodedRecord {
    RecordType type;
    const uint8_t* payload;  // points into `buf`, not owned
    uint8_t len;
    size_t consumed;
};
Result<DecodedRecord> decodeRecord(const uint8_t* buf, size_t avail);

// Header bytes of a candidate record WITHOUT CRC validation — the resync
// scan's cheap pre-filter (magic + plausible len). Returns false when the
// first 8 bytes cannot start a record.
bool looksLikeRecordHeader(const uint8_t* buf, size_t avail, RecordType& type, uint8_t& len);

}  // namespace bw::storage
