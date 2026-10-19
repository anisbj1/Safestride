#pragma once

// bw/research/raw_tee.h — F28 D3: the non-consuming tap for raw sample streams.
//
// WHY THIS EXISTS: ctx.imu_samples / ctx.ppg_samples are SPSC queues whose only
// legal consumer is F02's ProcessingTask — the streamer must never pop them
// (single-consumer hazard, D5). Instead the producer-side call site (one-line
// integration action, spec §15 A1) pushes a COPY into these F28-owned queues,
// which the research task drains. That is a legal SPSC pairing: producer =
// sensor-side task, consumer = research task.
//
// Gate: pushes are refused while the tee is disabled, so the default-off path
// (§5) costs the caller one branch and stores nothing.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <bw/containers/spsc_queue.h>
#include <bw/hardware.h>
#include <bw/research/research_config.h>
#include <bw/result.h>

namespace bw::research {

class RawTee {
public:
    // Producer side (sensor-side task).
    //  - disabled  → Err::Unsupported, nothing stored and NOT counted: switching
    //    capture off on purpose is not data loss;
    //  - full      → Err::Full refused, SpscQueue::drops() counts it (CR-004)
    //    and the count is surfaced in the next #HDR (D10);
    //  - otherwise → Err::Ok, ordered at the tail.
    bw::Err onImu(const bw::ImuSample& sample);
    bw::Err onPpg(const bw::PpgSample& sample);

    // Set by the research task from ResearchStreamer::active() each tick.
    void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

    // Consumer side (research task via ResearchStreamer).
    bool popImu(bw::ImuSample& out) { return imu_.pop(out); }
    bool popPpg(bw::PpgSample& out) { return ppg_.pop(out); }

    uint32_t imuDrops() const { return imu_.drops(); }
    uint32_t ppgDrops() const { return ppg_.drops(); }

private:
    bw::SpscQueue<bw::ImuSample, kImuTeeDepth> imu_;
    bw::SpscQueue<bw::PpgSample, kPpgTeeDepth> ppg_;
    std::atomic<bool> enabled_{false};
};

}  // namespace bw::research
