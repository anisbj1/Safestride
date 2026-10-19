#include <bw/research/raw_tee.h>

namespace bw::research {

bw::Err RawTee::onImu(const bw::ImuSample& sample) {
    if (!enabled()) {
        return bw::Err::Unsupported;  // capture off: refused, no drop recorded
    }
    return imu_.push(sample);  // Err::Full on a full queue, drops() counts it
}

bw::Err RawTee::onPpg(const bw::PpgSample& sample) {
    if (!enabled()) {
        return bw::Err::Unsupported;
    }
    return ppg_.push(sample);
}

}  // namespace bw::research
