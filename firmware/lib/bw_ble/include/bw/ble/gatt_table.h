#pragma once

// bw/ble/gatt_table.h - the GATT database as DATA (F19 spec 17): one pure
// table that BLE_GATT.md v1 documents, test_ble_contract.cpp pins, and the
// device GattDatabase iterates. Doc <-> code <-> test drift is therefore a
// build failure, not a bench discovery (spec DoD, H9).
//
// The table is host-visible on purpose: only the NimBLE BUILD of the
// database is device-only, the CONTRACT is not.

#include <cstddef>
#include <cstdint>

#include <bw/ble/ble_config.h>

namespace bw::ble {

// Characteristic property bits (subset of the ATT property set F19 uses).
enum class CharProp : uint8_t { Read = 0x01, Write = 0x02, Notify = 0x04, Indicate = 0x08 };

struct ServiceSpec {
    const char* name;
    uint16_t std_uuid;      // 16-bit SIG uuid; 0 = custom 128-bit service
    uint16_t custom_index;  // D3 index when std_uuid == 0
};

struct CharacteristicSpec {
    const char* name;     // exactly the row name in BLE_GATT.md v1 6
    uint8_t service;      // row index into kGattServices
    uint16_t std_uuid;    // 16-bit SIG uuid; 0 = custom 128-bit (D3 index)
    uint16_t uuid_index;  // D3 index when std_uuid == 0
    uint8_t props;        // CharProp bits
    bool encrypted;       // true = bonded + encrypted link required (6.2)
};

// Row indices into kGattCharacteristics - the device Chars struct and the
// host tests refer to rows by these names, never by magic positions.
constexpr uint8_t kRowFwRev = 0;
constexpr uint8_t kRowHwRev = 1;
constexpr uint8_t kRowSwRev = 2;   // schema versions string (D4)
constexpr uint8_t kRowSerial = 3;  // device_id = lowercase 12-hex BLE address (D4)
constexpr uint8_t kRowBatteryLevel = 4;
constexpr uint8_t kRowLiveStatus = 5;
constexpr uint8_t kRowHeartRate = 6;
constexpr uint8_t kRowEventNotify = 7;
constexpr uint8_t kRowCommand = 8;
constexpr uint8_t kRowConfig = 9;
constexpr uint8_t kRowSyncControl = 10;
constexpr uint8_t kRowSyncData = 11;

constexpr size_t kGattServiceCount = 3;
constexpr ServiceSpec kGattServices[kGattServiceCount] = {
    {"Device Information", 0x180A, 0},
    {"Battery", 0x180F, 0},
    {"Bracelet", 0, kUuidIdxService},
};

constexpr size_t kGattCharacteristicCount = 12;
constexpr CharacteristicSpec kGattCharacteristics[kGattCharacteristicCount] = {
    // Device Information (service 0), standard strings, readable unencrypted
    // ([ASSUMED] spec 13 - external.md 6.2 requires encryption for state,
    // events or commands only; device facts are not health records).
    {"Firmware Revision", 0, 0x2A26, 0, static_cast<uint8_t>(CharProp::Read), false},
    {"Hardware Revision", 0, 0x2A27, 0, static_cast<uint8_t>(CharProp::Read), false},
    {"Software Revision", 0, 0x2A28, 0, static_cast<uint8_t>(CharProp::Read), false},
    {"Serial Number", 0, 0x2A25, 0, static_cast<uint8_t>(CharProp::Read), false},
    // Battery (service 1): R + N, unencrypted.
    {"Battery Level", 1, 0x2A19, 0,
     static_cast<uint8_t>(CharProp::Read) | static_cast<uint8_t>(CharProp::Notify), false},
    // Custom service "Bracelet" (service 2; D3 base+index; all encrypted).
    {"Live Status", 2, 0, kUuidIdxLiveStatus,
     static_cast<uint8_t>(CharProp::Read) | static_cast<uint8_t>(CharProp::Notify), true},
    {"Heart Rate", 2, 0, kUuidIdxHeartRate, static_cast<uint8_t>(CharProp::Notify), true},
    {"Event Notify", 2, 0, kUuidIdxEventNotify, static_cast<uint8_t>(CharProp::Notify), true},
    {"Command", 2, 0, kUuidIdxCommand, static_cast<uint8_t>(CharProp::Write), true},
    {"Config", 2, 0, kUuidIdxConfig,
     static_cast<uint8_t>(CharProp::Read) | static_cast<uint8_t>(CharProp::Write), true},
    {"Sync Control", 2, 0, kUuidIdxSyncControl,
     static_cast<uint8_t>(CharProp::Write) | static_cast<uint8_t>(CharProp::Indicate), true},
    {"Sync Data", 2, 0, kUuidIdxSyncData, static_cast<uint8_t>(CharProp::Notify), true},
};

}  // namespace bw::ble
