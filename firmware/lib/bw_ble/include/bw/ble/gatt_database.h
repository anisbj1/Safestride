#pragma once

// bw/ble/gatt_database.h - F19 spec 7 (GattDatabase row): the NimBLE build
// of the pinned table in bw/ble/gatt_table.h (BLE_GATT.md v1, spec 17).
// Device-only: builds the services and characteristics on a NimBLEServer,
// assigns the access permissions (encrypted rows need a bonded+encrypted
// link, external.md 6.2) and hands the characteristic pointers back to
// BleServer / ConnectionMonitor / SyncTransport.

#if defined(ARDUINO)

#include <NimBLEDevice.h>

#include <bw/ble/gatt_table.h>

namespace bw::ble {

// Static device facts served through the standard Device Information
// service (D4). App-supplied build strings (spec 18): the app layer may
// include any feature's constants, F19 may not (01 1 rule 1).
struct DeviceIdentity {
    const char* firmware_rev = "unknown";     // -> 0x2A26
    const char* hw_rev = "unknown";           // -> 0x2A27
    const char* schema_versions = "unknown";  // -> 0x2A28, format ev<N>;g<N> (D4)
    // device_id (-> 0x2A25, lowercase 12-hex BLE address) is filled from the
    // stack at start(): it is the device's own address, not a build string.
};

class GattDatabase {
public:
    // Characteristic pointers, indexed by the kRow* constants of
    // gatt_table.h - one source of row identity for device code and tests.
    struct Chars {
        NimBLECharacteristic* by_row[kGattCharacteristicCount] = {};

        NimBLECharacteristic* liveStatus() const { return by_row[kRowLiveStatus]; }
        NimBLECharacteristic* heartRate() const { return by_row[kRowHeartRate]; }
        NimBLECharacteristic* eventNotify() const { return by_row[kRowEventNotify]; }
        NimBLECharacteristic* command() const { return by_row[kRowCommand]; }
        NimBLECharacteristic* config() const { return by_row[kRowConfig]; }
        NimBLECharacteristic* syncControl() const { return by_row[kRowSyncControl]; }
        NimBLECharacteristic* syncData() const { return by_row[kRowSyncData]; }
        NimBLECharacteristic* batteryLevel() const { return by_row[kRowBatteryLevel]; }
    };

    // Creates every service/characteristic of BLE_GATT.md v1 on `server`
    // (UUIDs from formatGattUuid / the SIG 16-bit values in the table) and
    // returns the pointers. Call once, after PairingManager::configure().
    Chars create(NimBLEServer* server);

    // The custom service UUID string (index 0), for the advertising payload
    // (D5: companion scans filter by service).
    const char* serviceUuidString() const { return service_uuid_; }

private:
    char service_uuid_[kGattUuidStringBytes] = {};
};

}  // namespace bw::ble

#endif  // ARDUINO
