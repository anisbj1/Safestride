#pragma once

// bw/sync/crc16.h — CRC-16/CCITT-FALSE for sync chunks (SYNC_PROTOCOL.md §3.3):
// poly 0x1021, init 0xFFFF, no reflect, xorout 0x0000. Bitwise implementation —
// no lookup table: frames are <= 252 B at a few Hz, so the ~2 ms/MB cost is
// irrelevant while a 512 B table would be pure flash waste (spec §7).

#include <cstddef>
#include <cstdint>

namespace bw::sync {

// Returns the CRC-16/CCITT-FALSE of data[0..len). `seed` allows incremental
// computation; the protocol always starts from the standard init 0xFFFF.
std::uint16_t crc16CcittFalse(const std::uint8_t* data, std::size_t len,
                              std::uint16_t seed = 0xFFFF);

}  // namespace bw::sync
