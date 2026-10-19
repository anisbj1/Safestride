#pragma once

// bw/ble/sync_link.h - the sync engine's view of the BLE link (CR-019 seam,
// spec D12): ISyncTransport must answer linkUp()/maxSendBytes() from live
// stack state without knowing anything about NimBLE.
//
// Why this exists (M4 bench bug 1, 2026-10-17): SyncTransport used to take
// the concrete ConnectionMonitor, i.e. it was device-only, so the rule
// "the transport MUST be bound to the monitor" had no host coverage - the
// wiring call was silently missing, linkUp() was false on every device and
// sync was dead on hardware while every host test stayed green. Behind this
// pure interface the device monitor and a host fake are interchangeable
// (bindMonitor), and the wiring binding pass compiles and executes on the
// host, so the same call the device runs is test-pinned (H10).

#include <cstdint>

namespace bw::ble {

// The two live link values CR-019 asks for. Device implementation:
// ConnectionMonitor (NimBLE gap/MTU/security/CCCD callbacks -> atomics);
// host implementation: bw_ble_test::FakeSyncLink. Contract of implementers:
//  linkUsable()    connected + encrypted (bonded link) + the peer enabled
//                  Sync Data notifications (the CR-019 linkUp definition).
//  maxSendBytes()  ATT MTU - kAttHeaderBytes clamped to
//                  [0, kMaxSyncFrameBytes]; 0 while no link.
class ISyncLink {
public:
    virtual bool linkUsable() const = 0;
    virtual uint16_t maxSendBytes() const = 0;
};

}  // namespace bw::ble
