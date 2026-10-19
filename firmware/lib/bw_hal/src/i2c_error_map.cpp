#include <bw/hal/i2c_error_map.h>

namespace bw::hal {

bw::Err errFromEndTransmission(uint8_t wire_code) {
    switch (wire_code) {
    case kWireSuccess:
        return bw::Err::Ok;
    case kWireAddrNack:
        // The addressed device did not answer: a device-level fact, not a
        // bus fault (i2c_bus.cpp: NotFound stops the retry/recovery ladder
        // so an unplugged sensor never pulses the shared bus).
        return bw::Err::NotFound;
    case kWireTimeout:
        return bw::Err::Timeout;
    default:
        // 1 (data too long), 3 (data NACK), 4 (other) and any unknown
        // future code: the transfer did not complete and nothing more
        // specific is known.
        return bw::Err::BusError;
    }
}

bw::Err errFromReadFailure(uint8_t probe_code) {
    const bw::Err mapped = errFromEndTransmission(probe_code);
    // Probe ACKed: the device exists but the read failed — the core hid the
    // real cause, so report the bounded, retryable one (Timeout), never Ok.
    return (mapped == bw::Err::Ok) ? bw::Err::Timeout : mapped;
}

}  // namespace bw::hal
