#include <bw/sync/crc16.h>

namespace bw::sync {

std::uint16_t crc16CcittFalse(const std::uint8_t* data, std::size_t len, std::uint16_t seed) {
    std::uint16_t crc = seed;
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x8000U) != 0U) {
                crc = static_cast<std::uint16_t>((crc << 1) ^ 0x1021U);
            } else {
                crc = static_cast<std::uint16_t>(crc << 1);
            }
        }
    }
    return crc;
}

}  // namespace bw::sync
