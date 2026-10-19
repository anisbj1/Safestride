#pragma once
// bw/sync_transport.h — CR-019 (accepted 2026-10-15). Transport seam between the
// F20 sync engine and the F19 BLE adapter; see docs/contracts/SYNC_PROTOCOL.md.

#include <cstddef>
#include <cstdint>

#include <bw/result.h>
#include <bw/types.h>

namespace bw {

// Which logical pipe a sync frame belongs to (02 §6.2 characteristics).
// Append-only: new values may be appended, never renumbered.
enum class SyncChannel : uint8_t {
    Control = 0,  // Sync Control: Write phone->device, Indicate device->phone
    Data = 1,     // Sync Data: Notify device->phone only
};

// [DERIVED] Largest frame the transport ever passes: ATT MTU is capped at
// 255 B and the 3 B ATT notification header is stripped by the stack,
// leaving 252 usable bytes (kMaxSyncMtuBytes - 3, SYNC_PROTOCOL.md §3.3).
constexpr uint16_t kMaxSyncFrameBytes = 252;

// Duplex byte pipe for the sync protocol. Call context: exactly one caller —
// the F20 sync pump (a single task) — invokes send()/receive()/maxSendBytes();
// implementations must serialize internally if their own callbacks run in
// another context (e.g. the NimBLE host task). Frames are opaque to the
// transport; all protocol semantics live in the F20 engine.
class ISyncTransport {
public:
    // device -> phone. Requires 0 < len <= kMaxSyncFrameBytes.
    //  Ok            frame accepted (Data: queued for notification;
    //                               Control: queued for indication).
    //  Err::NotReady  link is down; nothing sent; retry later.
    //  Err::Busy      link congested (e.g. previous indication unconfirmed);
    //                 nothing consumed; retry on a later pump.
    //  Err::InvalidArg len == 0 or len > kMaxSyncFrameBytes (caller bug;
    //                 nothing sent).
    virtual Result<void> send(SyncChannel ch, const uint8_t* data, uint16_t len) = 0;

    // phone -> device. Copies the OLDEST pending inbound frame into `out`
    // (capacity `cap`) and reports its channel in `ch_out`.
    //  Ok(0)    no frame pending (also right after a disconnect: inbound
    //           queues are discarded when the link drops).
    //  Ok(n>0)  one frame of n bytes delivered; n <= cap.
    //  Err::Overflow  a pending frame was larger than `cap` and was dropped
    //                 (correct callers pass cap >= kMaxSyncFrameBytes).
    virtual Result<size_t> receive(SyncChannel* ch_out, uint8_t* out, size_t cap) = 0;

    // True when the sync link is usable: connected, encrypted per bonding
    // policy, and the peer has enabled Sync Data notifications.
    // Polled every pump; false means "abort any transfer immediately".
    virtual bool linkUp() const = 0;

    // Usable bytes per Data frame (negotiated ATT MTU minus the 3 B ATT
    // header), in [0, kMaxSyncFrameBytes]; 0 when no MTU has been negotiated.
    // Meaningful only while linkUp() is true. The engine needs >= 52 B
    // (one 48 B record + 4 B framing => ATT MTU >= 55 B) before it streams
    // (SYNC_PROTOCOL.md §3.3).
    virtual uint16_t maxSendBytes() const = 0;
};

// CR-019 part B — sync status for UI/BLE (F20 spec §3 "Sync status").
struct SyncStatus {
    static constexpr uint8_t kSchemaVersion = 1;
    TimeMs t_ms;                // snapshot time
    uint32_t last_seq;          // newest seq known to the engine (store head seen)
    uint32_t last_synced_seq;   // last cumulative ack committed (= IEventStore.lastSyncedSeq)
    uint32_t events_sent;       // cumulative this boot (diagnostics/UI)
    uint32_t chunks_resent;     // cumulative this boot (diagnostics)
    uint16_t protocol_version;  // kSyncProtocolVersion the engine speaks
    uint8_t flags;              // bit0 linkUp, bit1 streaming, bit2 mtu_too_low,
                                // bit3 transfer_aborted (last attempt gave up);
                                // bits4..7 reserved (0)
};
class ISyncStatusProvider {
public:
    virtual bool latest(SyncStatus&) const = 0;  // false = no snapshot yet
};

}  // namespace bw
