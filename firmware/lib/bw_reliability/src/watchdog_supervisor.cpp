#include <bw/reliability/watchdog_supervisor.h>

namespace bw::reliability {

namespace {
bool validTaskId(const TaskId id) {
    return static_cast<size_t>(id) < static_cast<size_t>(TaskId::Count);
}
}  // namespace

void WatchdogSupervisor::configure(const TaskConfig& cfg) {
    const size_t index = static_cast<size_t>(cfg.id);
    if (!validTaskId(cfg.id) || cfg.period_ms == 0) {
        return;  // argument error: a 0-period task would stall at once
    }
    Slot& slot = slots_[index];
    if (slot.configured && slot.tier == TaskTier::A) {
        --configured_a_;  // re-configure must not double-count Tier A
    }
    slot.tier = cfg.tier;
    slot.period_ms = cfg.period_ms;
    slot.configured = true;
    if (cfg.tier == TaskTier::A) {
        ++configured_a_;
    }
}

void WatchdogSupervisor::beat(const TaskId id, const TimeMs now_ms) {
    if (!validTaskId(id)) {
        return;  // reject sentinel/invalid ids before touching the slot table
    }
    Slot& slot = slots_[static_cast<size_t>(id)];
    if (!slot.configured) {
        return;  // unconfigured beat: nothing supervises this id (wiring bug)
    }
    slot.last_beat_ms = now_ms;
    slot.ever_beaten = true;
    // stall state is owned by scan(): a beat shortens `elapsed`, and the next
    // scan clears the stall — re-entry is then detected as newly again.
}

ScanResult WatchdogSupervisor::scan(const TimeMs now_ms) {
    ScanResult out{};
    uint8_t a_total = 0;
    uint8_t a_fresh = 0;
    bool a_stalled = false;

    for (size_t i = 0; i < kMaxSupervisedTasks; ++i) {
        Slot& slot = slots_[i];
        if (!slot.configured) {
            continue;
        }
        const uint32_t bit = 1u << i;
        if (slot.ever_beaten) {
            // Monotonic TimeMs; guard a backwards clock jump instead of
            // underflowing into "stalled since the dawn of time".
            const TimeMs elapsed = now_ms >= slot.last_beat_ms ? now_ms - slot.last_beat_ms : 0;
            const uint64_t limit = static_cast<uint64_t>(kStallFactor) * slot.period_ms;
            if (elapsed > limit) {
                if (!slot.stalled) {
                    slot.stalled = true;  // enter edge, once per episode
                    out.newly_stalled_mask |= bit;
                }
                out.stalled_mask |= bit;
            } else {
                slot.stalled = false;  // recovered (beat or still inside 3x period)
            }
        } else {
            out.pending_mask |= bit;  // no history: pending, never stalled (D7)
        }

        if (slot.tier == TaskTier::A) {
            ++a_total;
            if (slot.ever_beaten && !slot.stalled) {
                ++a_fresh;
            }
            if (slot.stalled) {
                a_stalled = true;
            }
        }
    }

    out.tier_a_all_fresh = a_total > 0 && a_fresh == a_total;
    tier_a_stalled_ = a_stalled;
    tier_a_all_fresh_ = out.tier_a_all_fresh;
    return out;
}

bool WatchdogSupervisor::shouldFeed(const TimeMs now_ms) const {
    if (configured_a_ == 0) {
        return false;  // fail-safe: nothing Tier A configured -> never feed
    }
    if (tier_a_stalled_) {
        return false;  // D9: withhold -> TWDT panic -> reset
    }
    if (now_ms < kSupervisionGraceMs) {
        return true;  // boot grace: staggered task creation (§16.3)
    }
    return tier_a_all_fresh_;
}

BeatState WatchdogSupervisor::state(const TaskId id) const {
    if (!validTaskId(id)) {
        return BeatState::Pending;  // invalid ids have no heartbeat evidence
    }
    const Slot& slot = slots_[static_cast<size_t>(id)];
    if (!slot.configured || !slot.ever_beaten) {
        return BeatState::Pending;
    }
    return slot.stalled ? BeatState::Stalled : BeatState::Fresh;
}

}  // namespace bw::reliability
