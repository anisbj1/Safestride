// test_ble_contract.cpp - spec 9 row 9 + spec DoD (H9): the UUID scheme
// (D3 base + 16-bit index) and the FULL characteristic table (index, props,
// security) are pinned here exactly as BLE_GATT.md v1 documents them - any
// accidental contract drift fails pio test -e native.

#include <bw/ble/ble_config.h>
#include <bw/ble/codec.h>
#include <bw/ble/gatt_table.h>

#include <cstring>

#include <unity.h>

#include <bw/events.h>
#include <bw/sync_transport.h>  // kMaxSyncFrameBytes, pinned by the CR-019 seam

using bw::ble::CharacteristicSpec;
using bw::ble::CharProp;
using bw::ble::formatGattUuid;
using bw::ble::kGattCharacteristicCount;
using bw::ble::kGattCharacteristics;
using bw::ble::kGattContractVersion;
using bw::ble::kGattServiceCount;
using bw::ble::kGattServices;
using bw::ble::kGattUuidBasePrefix;
using bw::ble::kGattUuidStringBytes;

namespace {

bool has(uint8_t props, CharProp bit) {
    return (props & static_cast<uint8_t>(bit)) != 0;
}

}  // namespace

// D3: base + index, one expected string per published row of the v1 table.
void test_contract_uuid_scheme(void) {
    char uuid[kGattUuidStringBytes] = {};
    struct {
        uint16_t index;
        const char* expected;
    } expected[] = {
        {0x0000, "4bb00319-8811-4387-95a6-f4bb78380000"},  // service
        {0x0001, "4bb00319-8811-4387-95a6-f4bb78380001"},  // Live Status
        {0x0002, "4bb00319-8811-4387-95a6-f4bb78380002"},  // Heart Rate
        {0x0003, "4bb00319-8811-4387-95a6-f4bb78380003"},  // Event Notify
        {0x0004, "4bb00319-8811-4387-95a6-f4bb78380004"},  // Command
        {0x0005, "4bb00319-8811-4387-95a6-f4bb78380005"},  // Config
        {0x0006, "4bb00319-8811-4387-95a6-f4bb78380006"},  // Sync Control
        {0x0007, "4bb00319-8811-4387-95a6-f4bb78380007"},  // Sync Data
    };
    for (const auto& row : expected) {
        TEST_ASSERT_TRUE(formatGattUuid(row.index, uuid, sizeof(uuid)).ok());
        TEST_ASSERT_EQUAL_STRING(row.expected, uuid);
    }
    // The index space is closed: an unallocated index is a BUILD failure,
    // not a silently different UUID.
    TEST_ASSERT_FALSE(formatGattUuid(0x0008, uuid, sizeof(uuid)).ok());
}

void test_contract_uuid_scheme_bounds(void) {
    char uuid[kGattUuidStringBytes] = {};
    TEST_ASSERT_FALSE(formatGattUuid(0, uuid, kGattUuidStringBytes - 1).ok());  // needs 37
    TEST_ASSERT_FALSE(formatGattUuid(0, nullptr, kGattUuidStringBytes).ok());
    // The 32-char base (full UUID = base + 4-char index + NUL = 37 bytes).
    TEST_ASSERT_EQUAL_UINT(32, std::strlen(kGattUuidBasePrefix));
}

// Services: standard DIS + standard Battery + ONE custom service (D4).
void test_contract_service_table(void) {
    TEST_ASSERT_EQUAL_UINT(3, kGattServiceCount);
    TEST_ASSERT_EQUAL_STRING("Device Information", kGattServices[0].name);
    TEST_ASSERT_EQUAL_UINT16(0x180A, kGattServices[0].std_uuid);
    TEST_ASSERT_EQUAL_STRING("Battery", kGattServices[1].name);
    TEST_ASSERT_EQUAL_UINT16(0x180F, kGattServices[1].std_uuid);
    TEST_ASSERT_EQUAL_STRING("Bracelet", kGattServices[2].name);
    TEST_ASSERT_EQUAL_UINT16(0, kGattServices[2].std_uuid);  // custom 128-bit
    TEST_ASSERT_EQUAL_UINT16(bw::ble::kUuidIdxService, kGattServices[2].custom_index);
}

// The 12 rows of BLE_GATT.md v1 6, pinned: uuid, properties, encryption.
void test_contract_characteristic_table(void) {
    TEST_ASSERT_EQUAL_UINT(12, kGattCharacteristicCount);

    // Device Information (standard, unencrypted - [ASSUMED] spec 13).
    TEST_ASSERT_EQUAL_UINT16(0x2A26, kGattCharacteristics[0].std_uuid);
    TEST_ASSERT_TRUE(has(kGattCharacteristics[0].props, CharProp::Read));
    TEST_ASSERT_FALSE(kGattCharacteristics[0].encrypted);
    TEST_ASSERT_EQUAL_UINT16(0x2A27, kGattCharacteristics[1].std_uuid);
    TEST_ASSERT_FALSE(kGattCharacteristics[1].encrypted);
    TEST_ASSERT_EQUAL_UINT16(0x2A28, kGattCharacteristics[2].std_uuid);  // schema versions
    TEST_ASSERT_EQUAL_UINT16(0x2A25, kGattCharacteristics[3].std_uuid);  // device_id
    TEST_ASSERT_FALSE(kGattCharacteristics[3].encrypted);

    // Battery Level: R + N, unencrypted.
    const CharacteristicSpec& battery = kGattCharacteristics[4];
    TEST_ASSERT_EQUAL_UINT16(0x2A19, battery.std_uuid);
    TEST_ASSERT_TRUE(has(battery.props, CharProp::Read));
    TEST_ASSERT_TRUE(has(battery.props, CharProp::Notify));
    TEST_ASSERT_FALSE(has(battery.props, CharProp::Write));
    TEST_ASSERT_FALSE(battery.encrypted);

    // Custom rows, in index order, every one encrypted (external.md 6.2).
    const struct {
        uint8_t row;
        uint16_t index;
        bool read, write, notify, indicate;
    } custom[] = {
        {5, 0x0001, true, false, true, false},    // Live Status R,N
        {6, 0x0002, false, false, true, false},   // Heart Rate N
        {7, 0x0003, false, false, true, false},   // Event Notify N
        {8, 0x0004, false, true, false, false},   // Command W
        {9, 0x0005, true, true, false, false},    // Config R,W
        {10, 0x0006, false, true, false, true},   // Sync Control W,I
        {11, 0x0007, false, false, true, false},  // Sync Data N
    };
    for (const auto& c : custom) {
        const CharacteristicSpec& spec = kGattCharacteristics[c.row];
        TEST_ASSERT_EQUAL_UINT(0, spec.std_uuid);  // all custom are 128-bit
        TEST_ASSERT_EQUAL_UINT16(c.index, spec.uuid_index);
        TEST_ASSERT_EQUAL_UINT(2, spec.service);  // all under "Bracelet"
        TEST_ASSERT_TRUE(spec.encrypted);
        TEST_ASSERT_EQUAL(c.read, has(spec.props, CharProp::Read));
        TEST_ASSERT_EQUAL(c.write, has(spec.props, CharProp::Write));
        TEST_ASSERT_EQUAL(c.notify, has(spec.props, CharProp::Notify));
        TEST_ASSERT_EQUAL(c.indicate, has(spec.props, CharProp::Indicate));
    }
}

// Constants that are contract-pinned (spec 8): a silent change fails here.
void test_contract_pinned_constants(void) {
    TEST_ASSERT_EQUAL_UINT8(1, kGattContractVersion);
    TEST_ASSERT_EQUAL_UINT16(23, bw::ble::kDefaultAttMtu);  // ATT default (HW)
    TEST_ASSERT_EQUAL_UINT16(3, bw::ble::kAttHeaderBytes);  // opcode + handle
    TEST_ASSERT_EQUAL_UINT(48, bw::ble::kMaxEventNotifyBytes);
    TEST_ASSERT_EQUAL_UINT(51, bw::ble::kMinEventNotifyMtu);            // 48 + 3
    TEST_ASSERT_EQUAL_UINT(252, bw::kMaxSyncFrameBytes);                // CR-019
    TEST_ASSERT_EQUAL_UINT(48, sizeof(bw::Event));                      // CR-002 image
    TEST_ASSERT_EQUAL_STRING("Anarolepsy", bw::ble::kDeviceLocalName);  // D5
}

// The Live Status image must stay inside external.md 6.2's 20-byte cap.
void test_contract_live_status_fits_mandate(void) {
    TEST_ASSERT_LESS_OR_EQUAL_UINT(20, bw::ble::kLiveStatusBytes);
    TEST_ASSERT_EQUAL_UINT(14, bw::ble::kLiveStatusBytes);  // v1 table row
}
