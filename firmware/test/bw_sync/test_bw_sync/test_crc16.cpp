// bw_sync — CRC-16/CCITT-FALSE (F20 §9 T15 + edge cases).
// Deterministic vectors only: the check value pins the whole algorithm
// (poly, init, reflect, xorout) in one byte string.

#include <unity.h>

#include <cstring>

#include <bw/sync/crc16.h>

void test_crc16_ccitt_false_check_value(void) {
    // SYNC_PROTOCOL.md §6 V1: the canonical CCITT check string.
    const char* s = "123456789";
    TEST_ASSERT_EQUAL_HEX16(
        0x29B1, bw::sync::crc16CcittFalse(reinterpret_cast<const uint8_t*>(s), std::strlen(s)));
}

void test_crc16_empty_input_is_init(void) {
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, bw::sync::crc16CcittFalse(nullptr, 0));
}

void test_crc16_incremental_equals_block(void) {
    const uint8_t data[] = {0x01, 0x00, 0xE8, 0x03, 0xAA, 0x55, 0xFF};
    const uint16_t block = bw::sync::crc16CcittFalse(data, sizeof data);
    // Seed the first half's CRC into the second half's computation.
    const uint16_t part =
        bw::sync::crc16CcittFalse(data + 3, sizeof data - 3, bw::sync::crc16CcittFalse(data, 3));
    TEST_ASSERT_EQUAL_HEX16(block, part);
}

void test_crc16_detects_single_bit_flip(void) {
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    const uint16_t before = bw::sync::crc16CcittFalse(data, sizeof data);
    data[2] ^= 0x01;
    TEST_ASSERT_NOT_EQUAL(before, bw::sync::crc16CcittFalse(data, sizeof data));
}
