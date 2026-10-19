#pragma once

// bw/sync/chunk_packetizer.h — pure frame encode/decode for the sync protocol
// (docs/contracts/SYNC_PROTOCOL.md §3). No transport, no store, no time: the
// SyncSession owns the state machine, this module owns bytes.
//
// Event records travel as the BYTE IMAGE of the canonical bw::Event record
// (02 §4.5 / CR-002; layout pinned by bw/contract_checks.h). This is not an
// independent encoding: on the firmware side sizeof(bw::Event) == 48 with zero
// padding, so a record is copied verbatim; consumers decode by that contract's
// offsets (D8).

#include <cstddef>
#include <cstdint>

#include <bw/events.h>
#include <bw/result.h>
#include <bw/sync/sync_config.h>

namespace bw::sync {

// Parsed CURSOR frame (phone -> device, SYNC_PROTOCOL.md §3.1). Fields are
// independent: one frame may carry a new request AND an ack.
struct CursorRequest {
    std::uint32_t from_seq = 0;  // >= 1: stream from this seq; 0 = no request
    std::uint16_t max = 0;       // cap on events for this round; 0 = no request cap
    std::uint32_t ack_seq = 0;   // highest seq durably stored by the phone; 0 = no ack

    // All-zero CURSOR is a status query (§3.1) -> device replies with STATUS.
    bool isStatusQuery() const { return from_seq == 0 && max == 0 && ack_seq == 0; }
};

// Whole records that fit one Data frame of `usable_bytes` (transport-reported:
// ATT MTU - 3). 0 means "below the MTU floor" — the caller must not stream.
std::size_t recordsPerChunk(std::uint16_t usable_bytes);

// -- encode (device side) -----------------------------------------------------

// CHUNK (§3.3): [0..2) chunk_no LE, then `count` whole 48 B records, then
// CRC-16/CCITT-FALSE LE over [0, len-2). Returns the frame length.
//  Err::InvalidArg  count == 0, count > kMaxEventsPerChunk, or cap too small.
Result<std::size_t> encodeChunkFrame(std::uint16_t chunk_no, const bw::Event* events,
                                     std::size_t count, std::uint8_t* out, std::size_t cap);

// STATUS (§3.2): version, type, last_seq LE, last_synced LE, flags.
void encodeStatusFrame(std::uint32_t last_seq, std::uint32_t last_synced, std::uint8_t flags,
                       std::uint8_t out[kStatusFrameBytes]);

// -- encode (companion/test-harness mirror; firmware never calls these) -------

// CURSOR (§3.1): version, type, from_seq LE, max LE, ack_seq LE.
void encodeCursorFrame(const CursorRequest& request, std::uint8_t out[kCursorFrameBytes]);

// -- parse / decode -----------------------------------------------------------

// Validates version, type and exact length (D7: unknown version, unknown type
// or wrong length => drop, no crash); fills `out` on success.
Result<void> parseCursorFrame(const std::uint8_t* data, std::size_t len, CursorRequest* out);

// CHUNK (§3.3) -> whole records. Validates length (>= framing, `frame_len % 48 == 4`)
// and the trailing CRC; copies the records' byte images into out[0..returned).
//  Err::Corrupt      CRC mismatch.
//  Err::InvalidArg   malformed length or out too small for the decoded count.
//  Err::Overflow     more records in the frame than `max` fit into `out`
//                    (nothing written beyond the check).
Result<std::size_t> decodeChunkFrame(const std::uint8_t* data, std::size_t len,
                                     std::uint16_t* chunk_no_out, bw::Event* out, std::size_t max);

}  // namespace bw::sync
