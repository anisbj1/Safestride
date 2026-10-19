#pragma once

// bw/sync/sync_config.h — F20 §8 (named constants, every value tagged and
// mirrored in the spec table). Frame sizes, flow-control bounds and the MTU
// floor for the cursor+ack sync protocol (docs/contracts/SYNC_PROTOCOL.md).

#include <cstddef>
#include <cstdint>

#include <bw/events.h>
#include <bw/sync_transport.h>

namespace bw::sync {

// ---- protocol identity (spec §8) -------------------------------------------

// [DERIVED] First byte of every frame; any layout change bumps it and needs a
// CR on SYNC_PROTOCOL.md (§5 versioning). Why 1: first published layout.
constexpr uint16_t kSyncProtocolVersion = 1;

// ---- frame geometry (SYNC_PROTOCOL.md §3) ----------------------------------

// [DERIVED] CURSOR frame (phone -> device, §3.1):
// 1 version + 1 type + 4 from_seq + 2 max + 4 ack_seq = 12; static_asserted.
constexpr size_t kCursorFrameBytes = 12;

// [DERIVED] STATUS frame (device -> phone, §3.2):
// 1 version + 1 type + 4 last_seq + 4 last_synced + 1 flags = 11; static_asserted.
constexpr size_t kStatusFrameBytes = 11;

// [DERIVED] Byte image of the canonical bw::Event record: schema v1 is exactly
// kEventBytes (48 B, CR-002 / 02 §4.5), pinned by bw/contract_checks.h. The
// chunk payload is whole records of this size; nothing is ever split.
constexpr size_t kEventRecordBytes = kEventBytes;

// [DERIVED] CHUNK header: the 2 B per-transfer chunk counter (§3.3).
constexpr size_t kChunkHeaderBytes = 2;

// [DERIVED] CHUNK trailer: the 2 B CRC-16/CCITT-FALSE over bytes [0, len-2) (§3.3).
constexpr size_t kChunkCrcBytes = 2;

// [DERIVED] Bytes a Data frame spends on framing (counter + CRC) before any
// payload: 2 + 2 = 4 (SYNC_PROTOCOL.md §3.3).
constexpr size_t kChunkFramingBytes = kChunkHeaderBytes + kChunkCrcBytes;

// ---- MTU floor and caps (spec §8, CR-019 accepted header comment) ----------

// [DERIVED] Smallest USABLE Data-frame byte count that carries one whole
// record: 48 record + 4 framing = 52. The CR-019 transport reports usable
// bytes as negotiated ATT MTU minus the 3 B ATT header, so the ATT MTU floor
// is 52 + 3 = kMinSyncMtuBytes. Below the floor sync is off (status flag),
// never partial.
constexpr uint16_t kMinSyncUsableBytes =
    static_cast<uint16_t>(kEventRecordBytes + kChunkFramingBytes);

// [DERIVED] Smallest negotiated ATT MTU the engine streams on: 3 ATT header +
// 52 usable = 55 (SYNC_PROTOCOL.md §2, D3). Below it: mtu_too_low flag, zero
// data sends.
constexpr uint16_t kMinSyncMtuBytes = static_cast<uint16_t>(kMinSyncUsableBytes + 3);

// [DERIVED] Cap on the negotiated ATT MTU the engine uses: 255 B is the ATT
// spec cap for a single-byte length field; usable = 255 - 3 = kMaxSyncFrameBytes
// (bw/sync_transport.h). Larger MTUs buy nothing at 5 events/chunk.
constexpr uint16_t kMaxSyncMtuBytes = 255;

// [DERIVED] Whole records in the largest chunk at the MTU cap:
// (kMaxSyncFrameBytes - 4) / 48 = (252 - 4) / 48 = 5 (240 of 248 payload
// bytes used, 8 B slack). Bounds the per-chunk event loop.
constexpr size_t kMaxEventsPerChunk = (kMaxSyncFrameBytes - kChunkFramingBytes) / kEventRecordBytes;

// [DERIVED] Store batch per pump (kMaxEventsPerRead * 48 = 768 B read buffer);
// bounds one step() while still filling >= 3 chunks at MTU 255 (spec §11).
constexpr size_t kMaxEventsPerRead = 16;

// ---- flow control and timers (spec §8) -------------------------------------

// [EXPERIMENTAL] Resend window: this long after the oldest unacked chunk the
// unacked suffix is resent (same chunk numbers). Tune with bench data (F28).
constexpr bw::TimeMs kAckTimeoutMs = 5000;

// [EXPERIMENTAL] Simple flow control: at most this many chunks in flight
// ahead of the last ack (4 chunks ~= 20 events at MTU 255; spec §8). Bounds
// RAM (window entries) and the wasted resend on a lost ack.
constexpr size_t kMaxUnackedChunks = 4;

// [EXPERIMENTAL] After this many ack timeouts (or consecutive send failures)
// with no ack progress, the attempt is aborted (status flag) and the phone's
// next request restarts it; bounds wasted radio on a wedged peer (spec §10).
constexpr uint8_t kMaxAckTimeoutsPerTransfer = 3;

// [DERIVED] Upper bound on inbound Control frames processed per pump: keeps
// step() bounded even against a phone flooding CURSOR writes; extra frames
// stay queued for the next pump (SYNC_PROTOCOL.md §4.2).
constexpr size_t kMaxControlFramesPerPump = 8;

// ---- frame type bytes and STATUS flags (SYNC_PROTOCOL.md §3) ----------------

// [DERIVED] CURSOR frame type byte (§3.1). Distinguished from STATUS by
// direction: Control frames the phone WRITES are CURSOR; the device only ever
// parses inbound Control frames as CURSOR (0x01 = "control request").
constexpr uint8_t kCursorFrameType = 0x01;

// [DERIVED] STATUS frame type byte (§3.2). Same value as kCursorFrameType by
// design: direction (Write vs Indicate) disambiguates, and each end parses
// only the frames it receives.
constexpr uint8_t kStatusFrameType = 0x01;

// [DERIVED] STATUS flags bits (§3.2): the four booleans the phone needs to
// decide whether to request now, later, or never (mtu_too_low). Append-only.
constexpr uint8_t kStatusFlagLinkUp = 0x01;
constexpr uint8_t kStatusFlagStreaming = 0x02;
constexpr uint8_t kStatusFlagMtuTooLow = 0x04;
constexpr uint8_t kStatusFlagTransferAborted = 0x08;

// ---- layout pins (drift here is a build failure, not a runtime surprise) ----
static_assert(kSyncProtocolVersion == 1, "bump needs a CR on SYNC_PROTOCOL.md");
static_assert(kCursorFrameBytes == 12, "CURSOR layout drifted (SYNC_PROTOCOL.md §3.1)");
static_assert(kStatusFrameBytes == 11, "STATUS layout drifted (SYNC_PROTOCOL.md §3.2)");
static_assert(kEventRecordBytes == 48, "record size drifted from bw::Event (CR-002)");
static_assert(kMinSyncMtuBytes == kMinSyncUsableBytes + 3,
              "ATT MTU floor must be usable floor + 3 B ATT header");
static_assert(kMaxSyncMtuBytes == kMaxSyncFrameBytes + 3,
              "kMaxSyncMtuBytes must match the CR-019 transport cap");
static_assert(kMaxEventsPerChunk >= 1 &&
                  kMaxEventsPerChunk * kEventRecordBytes + kChunkFramingBytes <= kMaxSyncFrameBytes,
              "one whole record must fit the largest frame");
static_assert(kMaxEventsPerRead >= kMaxEventsPerChunk,
              "one pump must be able to fill the flow-control window");

}  // namespace bw::sync
