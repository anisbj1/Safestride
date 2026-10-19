#pragma once

// Shared scripted-timeline helpers for the bw_input / F11 suite (F11 §9).
// Everything is deterministic: tests own the clock, no wall time, no sleeps.

#include <bw/input/input_engine.h>
#include <bw/input/input_types.h>

namespace bw_input_test {

// Drives InputEngine at an explicit cadence (10 ms = the InputTask period,
// §11) and collects everything it emits. `hold` helpers speak RAW time — the
// debounced edges land kDebounceMs inside each raw edge, exactly like the
// device.
struct EngineFeed {
    bw::input::InputEngine engine{};
    bw::input::InputContext ctx{};
    bw::TimeMs t = 0;
    bool big = false;
    bool small = false;

    static constexpr uint8_t kMaxRecords = 8;
    bw::input::RoutedGesture rec[kMaxRecords]{};
    uint8_t rec_count = 0;
    uint8_t stuck_edges = 0;
    bool stuck_seen[2] = {false, false};

    // One tick per call; dt defaults to the 100 Hz InputTask period.
    void tick(uint32_t n = 1, bw::TimeMs dt = 10) {
        for (uint32_t i = 0; i < n; ++i) {
            const bw::input::StepOutput out = engine.step(t, ctx, big, small);
            for (uint8_t k = 0; k < out.count && rec_count < kMaxRecords; ++k) {
                rec[rec_count++] = out.items[k];
            }
            for (uint8_t b = 0; b < 2; ++b) {
                if (out.stuck[b]) {
                    stuck_seen[b] = true;
                    ++stuck_edges;
                }
            }
            t += dt;
        }
    }

    // Raw down for hold_ms (a multiple of the 10 ms tick), then release and
    // settle so the debounced release edge is observed. Debounced hold time
    // equals hold_ms for hold_ms >= kDebounceMs; a shorter raw pulse never
    // registers at all (bounce rejection).
    void cycleBig(uint32_t hold_ms) {
        big = true;
        tick(hold_ms / 10);
        big = false;
        tick(4);
    }

    void cycleSmall(uint32_t hold_ms) {
        small = true;
        tick(hold_ms / 10);
        small = false;
        tick(4);
    }

    // Both raw levels together — the chord building block.
    void cycleBoth(uint32_t hold_ms) {
        big = true;
        small = true;
        tick(hold_ms / 10);
        big = false;
        small = false;
        tick(4);
    }
};

}  // namespace bw_input_test
