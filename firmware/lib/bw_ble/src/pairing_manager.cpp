#include <bw/ble/pairing_manager.h>

#if defined(ARDUINO)

#include <NimBLEDevice.h>

namespace bw::ble {

void PairingManager::configure() {
    // Bonding on, MITM off (Just Works), LE Secure Connections on: the only
    // pairing this prototype needs (D1). Passkey entry is a later CR.
    NimBLEDevice::setSecurityAuth(true /*bonding*/, false /*mitm*/, true /*sc*/);
    // BLE_SM_IO_CAP_NO_IO (0x03, ble_sm.h) is NimBLE's own NoInputNoOutput -
    // the 2.5.1 [MEASURED] surface does not pull in Bluedroid's
    // ESP_IO_CAP_NONE; Just Works needs exactly this value.
    NimBLEDevice::setSecurityIOCap(BLE_SM_IO_CAP_NO_IO);
    // Bonds live in NimBLE's NVS store by default - restore-on-boot is what
    // makes "reconnect without re-pairing" work (checklist 1).
}

}  // namespace bw::ble

#endif  // ARDUINO
