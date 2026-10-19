#pragma once

// bw/ble/pairing_manager.h - F19 spec 7 (PairingManager row) + spec D1: the
// NimBLE security configuration. Device-only: nothing here compiles on the
// host (the whole file is behind ARDUINO, bw_storage/littlefs_adapter.cpp
// precedent), the host suites cover pure logic only (spec 9).

#if defined(ARDUINO)

namespace bw::ble {

// D1 - simplest option that satisfies external.md 6.2: standard NimBLE
// bonding (Just Works), LE Secure Connections on, IO capability
// NoInputNoOutput, bonds persisted in NimBLE's NVS store and restored on
// boot, so a bonded phone reconnects without re-pairing (device checklist
// 1). The future passkey/pairing-window CR replaces THIS module only - keep
// it swappable, never inline security policy elsewhere (spec D1).
//
// The [ASSUMED] NimBLE 2.5.1 surface lives here (spec 13): setSecurityAuth /
// setSecurityIOCap / NVS bond store defaults.
class PairingManager {
public:
    // Call once after NimBLEDevice::init(), before any characteristic with
    // encrypted access permissions is created.
    static void configure();
};

}  // namespace bw::ble

#endif  // ARDUINO
