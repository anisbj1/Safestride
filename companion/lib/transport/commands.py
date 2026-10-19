"""Command frame builders for the Command characteristic (BLE_GATT.md §4.3).

F21 only ever builds frames the spec's scope needs (W5: SetTime for time sync).
Arg layouts follow the accepted CR-021 clarification of data_decision.md §4.6.
"""

from __future__ import annotations

import struct

from companion.transport.codec import COMMAND_BYTES

# [HW] `CommandType` index of SetTime (data_decision.md §4.6, append-only enum;
# index 11 per the enum order — CR-021 pinned the argument layout, not the index).
COMMAND_TYPE_SET_TIME = 11

# [HW] Command image: type u8 + arg0 u32 LE + arg1 u32 LE (BLE_GATT §4.3).
_COMMAND_V0 = struct.Struct("<BII")
assert _COMMAND_V0.size == COMMAND_BYTES

# [HW] UnixSeconds is u32 (core.md §1: 0 = unknown; fits until 2106).
_UNIX_SECONDS_MAX = 0xFFFFFFFF


def build_set_time(unix_seconds: int) -> bytes:
    """Build the 9 B SetTime command: type 0x0B + arg0 = unix_seconds u32 LE,
    arg1 = 0 reserved (CR-021 accepted, data_decision.md §4.6).

    The device stamps t_ms/source itself from its own clock (BLE_GATT §4.3):
    a phone cannot spoof either.
    """
    if not 0 <= unix_seconds <= _UNIX_SECONDS_MAX:
        raise ValueError(f"SetTime: unix_seconds {unix_seconds} outside u32 range")
    return _COMMAND_V0.pack(COMMAND_TYPE_SET_TIME, unix_seconds, 0)
