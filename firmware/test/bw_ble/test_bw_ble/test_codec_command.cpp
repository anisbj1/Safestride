// test_codec_command.cpp - spec 9 row 3: 9 B happy path, wrong length ->
// InvalidArg, unknown CommandType -> InvalidArg, t_ms stamped from the passed
// TimeMs and source = Ble.

#include <bw/ble/codec.h>

#include <unity.h>

using bw::Command;
using bw::CommandType;
using bw::ble::decodeCommand;
using bw::ble::kCommandBytes;
using bw::ble::kCommandSourceBle;

void test_command_happy_path(void) {
    const uint8_t wire[kCommandBytes] = {
        static_cast<uint8_t>(CommandType::MarkEvent),
        0x04,
        0x03,
        0x02,
        0x01,  // arg0 = 0x01020304 LE
        0xDD,
        0xCC,
        0xBB,
        0xAA,  // arg1 = 0xAABBCCDD LE
    };
    const bw::Result<Command> cmd = decodeCommand(wire, sizeof(wire), 123'456);
    TEST_ASSERT_TRUE(cmd.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CommandType::MarkEvent),
                            static_cast<uint8_t>(cmd.value.type));
    TEST_ASSERT_EQUAL_HEX32(0x01020304u, cmd.value.arg0);
    TEST_ASSERT_EQUAL_HEX32(0xAABBCCDDu, cmd.value.arg1);
    TEST_ASSERT_EQUAL_UINT64(123456, cmd.value.t_ms);  // device clock, not the phone
    TEST_ASSERT_EQUAL_UINT8(kCommandSourceBle, cmd.value.source);
}

void test_command_wrong_length(void) {
    uint8_t wire[kCommandBytes + 2] = {};
    TEST_ASSERT_FALSE(decodeCommand(wire, kCommandBytes - 1, 0).ok());
    TEST_ASSERT_FALSE(decodeCommand(wire, kCommandBytes + 1, 0).ok());
    TEST_ASSERT_FALSE(decodeCommand(nullptr, kCommandBytes, 0).ok());
}

void test_command_unknown_type_rejected(void) {
    uint8_t wire[kCommandBytes] = {};
    // Past the append-only enum ceiling: rejected, never guessed (D6).
    wire[0] = static_cast<uint8_t>(CommandType::RequestSync) + 1;
    TEST_ASSERT_FALSE(decodeCommand(wire, kCommandBytes, 10).ok());

    // The newest legal value still decodes.
    wire[0] = static_cast<uint8_t>(CommandType::RequestSync);
    TEST_ASSERT_TRUE(decodeCommand(wire, kCommandBytes, 10).ok());

    // Every legal CommandType value decodes (append-only enum coverage).
    for (uint8_t t = 0; t <= static_cast<uint8_t>(CommandType::RequestSync); ++t) {
        wire[0] = t;
        const bw::Result<Command> cmd = decodeCommand(wire, kCommandBytes, 10);
        TEST_ASSERT_TRUE(cmd.ok());
        TEST_ASSERT_EQUAL_UINT8(t, static_cast<uint8_t>(cmd.value.type));
    }
}
