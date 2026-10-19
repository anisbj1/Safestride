#include <bw/ble/gatt_database.h>

#if defined(ARDUINO)

#include <cstring>

#include <bw/ble/ble_config.h>
#include <bw/ble/codec.h>
#include <bw/sync_transport.h>  // kMaxSyncFrameBytes (Sync Data/Control max_len)

// NimBLE-Arduino 2.5.1 internal declaration [VERIFIED in .pio/libdeps]:
// src/nimble/nimble/host/src/ble_gatt_priv.h:227 `void
// ble_gatts_set_clt_cfg_perm_flags(uint8_t flags);` - the header resolves
// because the NimBLE src root is on the library include path (same root that
// makes <NimBLEDevice.h> work). It pulls only NimBLE's own public host
// headers plus syscfg/stats, all under that root.
#include <nimble/nimble/host/src/ble_gatt_priv.h>

namespace bw::ble {

namespace {

uint32_t nimbleProps(const CharacteristicSpec& spec) {
    uint32_t out = 0;
    if ((spec.props & static_cast<uint8_t>(CharProp::Read)) != 0) {
        out |= NIMBLE_PROPERTY::READ;
        // Encryption enforcement rides on the property set in NimBLE 2.5.1
        // [MEASURED] (the 1.x setAccessPermissions() is gone): READ_ENC /
        // WRITE_ENC make the stack refuse an unbonded/unencrypted access with
        // ATT 0x05 / 0x0F before any callback runs (spec 6.2, D6).
        if (spec.encrypted) {
            out |= NIMBLE_PROPERTY::READ_ENC;
        }
    }
    if ((spec.props & static_cast<uint8_t>(CharProp::Write)) != 0) {
        out |= NIMBLE_PROPERTY::WRITE;
        if (spec.encrypted) {
            out |= NIMBLE_PROPERTY::WRITE_ENC;
        }
    }
    if ((spec.props & static_cast<uint8_t>(CharProp::Notify)) != 0) {
        out |= NIMBLE_PROPERTY::NOTIFY;
    }
    if ((spec.props & static_cast<uint8_t>(CharProp::Indicate)) != 0) {
        out |= NIMBLE_PROPERTY::INDICATE;
    }
    return out;
}

// Per-characteristic max_len (NimBLE 2.5.1 [MEASURED] createCharacteristic
// 3rd argument): the stack refuses any write LONGER than this with ATT 0x0D
// (BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN) before onWrite - the only genuine
// length ATT error the 2.x callback surface can still produce (its write
// callback can no longer reject). It also caps setValue(), so a row that
// carries device->phone payloads must be as large as its biggest payload,
// never as small as its write bound (BLE_GATT.md v1 2).
uint16_t maxValueLen(uint8_t row) {
    switch (row) {
    case kRowFwRev:
    case kRowHwRev:
    case kRowSwRev:
    case kRowSerial:
        return kMaxIdentityStringBytes;
    case kRowBatteryLevel:
        return 1;
    case kRowLiveStatus:
        return kLiveStatusBytes;
    case kRowHeartRate:
        return kHeartRateMaxBytes;
    case kRowEventNotify:
        return kMaxEventNotifyBytes;
    case kRowCommand:
        // Write-only, exactly kCommandBytes: max_len == the contract
        // length, so an over-long Command write IS a real ATT 0x0D.
        return kCommandBytes;
    case kRowConfig:
        // Read carries the whole dump; the 64 B WRITE bound is a
        // counted semantic reject (one attribute, one max_len).
        return kMaxConfigDumpBytes;
    case kRowSyncControl:
    case kRowSyncData:
        // Indicate/Notify payloads are F20 frames up to kMaxSyncFrameBytes;
        // the 32 B Sync Control WRITE bound is counted, not ATT (dumb
        // pipe, CR-019: F19 cannot parse to justify 0x0D).
        return kMaxSyncFrameBytes;
    default:
        return kMaxIdentityStringBytes;
    }
}

}  // namespace

GattDatabase::Chars GattDatabase::create(NimBLEServer* server) {
    Chars chars{};

    // AUD-1 layer 1: every CCCD this server registers must refuse writes from
    // an unencrypted link. NimBLE 2.5.1 [VERIFIED] defaults the client-config
    // descriptor permissions to BLE_ATT_F_READ|BLE_ATT_F_WRITE (ble_gatts.c
    // line 32) and registers them with those flags in ble_gatts_start
    // (ble_gatts.c line 1018, ble_att_svr_register). The setter must run
    // BEFORE server->start() (BleServer::start calls create() first), then a
    // CCCD write over an unencrypted link is refused by the stack with ATT
    // 0x05 Insufficient Authentication (no bond) / 0x0F Insufficient
    // Encryption (bonded, link not encrypted) - ble_att_svr.c access check,
    // values [VERIFIED] in host/ble_att.h (0x05 / 0x0F, same pair the row
    // properties produce).
    // BLE_ATT_F_* values [VERIFIED] in host/ble_att.h (READ 0x01, WRITE 0x02,
    // WRITE_ENC 0x20). This is global to the stack: ALL CCCDs (Live Status,
    // HR, Event, Sync Data, Battery) need an encrypted link to SUBSCRIBE -
    // documented in BLE_GATT.md v1 3. Battery VALUE reads stay unencrypted.
    ble_gatts_set_clt_cfg_perm_flags(BLE_ATT_F_READ | BLE_ATT_F_WRITE | BLE_ATT_F_WRITE_ENC);

    (void)formatGattUuid(kUuidIdxService, service_uuid_, sizeof(service_uuid_));

    NimBLEService* services[kGattServiceCount] = {};
    for (size_t i = 0; i < kGattServiceCount; ++i) {
        const ServiceSpec& spec = kGattServices[i];
        if (spec.std_uuid != 0) {
            services[i] = server->createService(NimBLEUUID(spec.std_uuid));
        } else {
            char uuid[kGattUuidStringBytes] = {};
            (void)formatGattUuid(spec.custom_index, uuid, sizeof(uuid));
            services[i] = server->createService(NimBLEUUID(uuid));
        }
    }

    for (size_t i = 0; i < kGattCharacteristicCount; ++i) {
        const CharacteristicSpec& spec = kGattCharacteristics[i];
        char custom_uuid[kGattUuidStringBytes] = {};
        NimBLEUUID uuid(spec.std_uuid);
        if (spec.std_uuid == 0) {
            (void)formatGattUuid(spec.uuid_index, custom_uuid, sizeof(custom_uuid));
            uuid = NimBLEUUID(custom_uuid);
        }
        NimBLECharacteristic* chr = services[spec.service]->createCharacteristic(
            uuid, nimbleProps(spec), maxValueLen(static_cast<uint8_t>(i)));
        // Empty value until the owner publishes real data: "no data" is
        // explicit, never a fabricated snapshot (spec D6, 00 1).
        chr->setValue("");
        chars.by_row[i] = chr;
    }

    return chars;
}

}  // namespace bw::ble

#endif  // ARDUINO
