#pragma once

// bw/ble/sync_transport.h - F19's implementation of bw::ISyncTransport
// (CR-019, accepted 2026-10-15; G1 ruling 1: a DUMB BYTE PIPE - no sync
// control parsing, no protocol semantics, those live in F20's engine and
// SYNC_PROTOCOL.md). Shape (spec D12): the gate - argument bounds, link
// state, inbound queue - is pure and host-tested over the ISyncLink seam;
// only deliver() touches the radio (device: NimBLE indication/notification).

#include <bw/ble/sync_frame_queue.h>
#include <bw/ble/sync_link.h>
#include <bw/result.h>
#include <bw/sync_transport.h>

#if defined(ARDUINO)
#include <bw/ble/gatt_database.h>
#endif

namespace bw::ble {

class SyncTransport : public ISyncTransport {
public:
    // The transport MUST observe the link (M4 bench bug 1: an unbound
    // monitor leaves linkUp() false forever and F20 can never send).
    // wire_ble's binding pass calls this on every re-bind - a call the host
    // wiring test executes and pins (H10 revert guard, spec D12).
    void bindMonitor(ISyncLink* link) { link_ = link; }

#if defined(ARDUINO)
    // Characteristic pointers, set once BleServer::start() has built the
    // GATT database (device only).
    void setChars(const GattDatabase::Chars* chars) { chars_ = chars; }
#endif

    // ISyncTransport ------------------------------------------------------------

    //  Ok            frame accepted (Control: queued for indication;
    //                               Data: queued for notification).
    //  Err::NotReady link not usable (CR-019 linkUp() false).
    //  Err::Busy     the stack refused (previous indication unconfirmed / no
    //                room for the notification): nothing consumed, retry.
    //  Err::InvalidArg len == 0 or len > kMaxSyncFrameBytes (caller bug).
    Result<void> send(SyncChannel ch, const uint8_t* data, uint16_t len) override;

    // Oldest pending inbound frame (Sync Control writes only - Sync Data is
    // device->phone). Ok(0) when empty or right after a disconnect (frames
    // of a dropped link are discarded in passing, SyncFrameQueue).
    Result<size_t> receive(SyncChannel* ch_out, uint8_t* out, size_t cap) override;

    bool linkUp() const override;  // connected + encrypted + subscribed (CR-019)
    // ATT MTU - 3, 0 while no link. [MEASURED] CR-019: the sync engine needs
    // 52 usable bytes => ATT MTU >= 55. (The 51 B figure in kMinEventNotifyMtu
    // is the separate Event-notify floor, NOT the engine floor - audit AUD-5.)
    uint16_t maxSendBytes() const override;

    // F19-side seams ------------------------------------------------------------
    SyncFrameQueue& inbound() { return inbound_; }

protected:
    // Platform boundary (D12): send() has already checked the arguments and
    // the link - this hands ONE accepted frame to the radio. Device: Control
    // -> indication with real Busy backpressure, Data -> notification (M4
    // bench fix 2). Host base: no radio exists there; the suite subclasses
    // SyncTransport and overrides deliver() to record send attempts, and an
    // un-overridden base answers Unsupported - never a fabricated success.
    virtual Result<void> deliver(SyncChannel ch, const uint8_t* data, uint16_t len);

private:
    ISyncLink* link_ = nullptr;
#if defined(ARDUINO)
    const GattDatabase::Chars* chars_ = nullptr;
#endif
    SyncFrameQueue inbound_;
};

}  // namespace bw::ble
