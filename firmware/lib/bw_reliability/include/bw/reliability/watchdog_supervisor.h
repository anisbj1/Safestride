#pragma once

// bw/reliability/watchdog_supervisor.h — heartbeat aggregation (spec §6 D1,
// §16.1 D7/D9): the supervisor feeds the hardware WDT only when every Tier A
// heartbeat is fresh; no loop that can survive a dead pipeline ever feeds.
//
// Pure core: slot table + stall scan + feed decision, no platform calls
// (esp_task_wdt_* lives in wire_reliability.cpp). Beats arrive from other
// tasks, so the wiring guards beat()/scan() with bw::CriticalSection (D8).

#include <cstddef>
#include <cstdint>

#include <bw/reliability/reliability_config.h>
#include <bw/types.h>

namespace bw::reliability {

// D7 tiers: Tier A gates the hardware-WDT feed (stall => reset — the
// sensing->decision->alert path); Tier B is reported and degraded but NEVER
// resets (DisplayFailed must never block sensing, BLE down must never stop
// local alerting — fault.h, 00 non-negotiable).
enum class TaskTier : uint8_t { A, B };

// Supervised runtime.md §3.2 tasks (W6 P4: 10 Tier A + 8 Tier B).
// Each independently scheduled pump has its own key. Append-only:
// the bit masks in ScanResult/PolicyInput are 1 << TaskId.
enum class TaskId : uint8_t {
    Sensor = 0,    // F01        Tier A
    Processing,    // F02        Tier A
    Intelligence,  // F03 ONLY   Tier A (legacy key retained)
    State,         // F07 ONLY   Tier A
    Alert,         // F10        Tier A
    Input,         // F11        Tier A
    Diag,          // F23        Tier B
    Ui,            // F12        Tier B
    Storage,       // F18        Tier B
    Comms,         // F19        Tier B
    ServicePump,   // F20/F16    Tier B
    Wear,          // F04        Tier A
    Wake,          // F06        Tier A
    Sleep,         // F08        Tier A
    Cataplexy,     // F09        Tier A
    EventLog,      // F17        Tier B
    Baseline,      // F13        Tier B
    Research,      // F28 optional Tier B (pending when disabled)
    Count,
};
static_assert(static_cast<uint8_t>(TaskId::Count) <= kMaxSupervisedTasks,
              "slot table sized by kMaxSupervisedTasks");

// D7 stall rule states: a task that has never beaten is "pending", never
// stalled — before its first beat there is no history to judge (the boot grace
// window covers staggered creation).
enum class BeatState : uint8_t { Pending, Fresh, Stalled };

struct TaskConfig {
    TaskId id;
    TaskTier tier;
    uint32_t period_ms;  // expected heartbeat period; stall = kStallFactor x period
};

struct ScanResult {
    uint32_t stalled_mask = 0;        // currently stalled (bit i <=> TaskId i)
    uint32_t newly_stalled_mask = 0;  // Fresh -> Stalled transitions this scan
    uint32_t pending_mask = 0;        // configured, never beaten
    bool tier_a_all_fresh = false;    // every configured Tier A task Fresh right now
};

class WatchdogSupervisor {
public:
    // Registers one supervised task; called from the wiring's fixed table at
    // wire() time (configuration is init-only, no lock needed). Count and
    // out-of-range ids are ignored by configure()/beat(); state() returns Pending.
    void configure(const TaskConfig& cfg);

    // Records a heartbeat (wiring guards this — beats come from other tasks).
    void beat(TaskId id, TimeMs now_ms);

    // Recomputes every slot against `now_ms` and returns the stall masks.
    // Recovery clears a stall on the next scan (a new beat makes elapsed <=
    // kStallFactor x period again), so `newly_stalled_mask` fires once per
    // stall episode — that is where TaskStalled is reported (rows 5/6).
    ScanResult scan(TimeMs now_ms);

    // D9: feed the WDT only when (a) at least one Tier A task is configured —
    // supervising nothing is a wiring fault, so never feed (fail-safe);
    // (b) no Tier A task is stalled (a stall withholds the feed -> TWDT panic
    // -> reset); (c) the boot grace has passed AND every configured Tier A
    // heartbeat is fresh. The grace excuses PENDING tasks only, never a stall.
    bool shouldFeed(TimeMs now_ms) const;

    // Diagnostic view for tests: state of one slot after the last scan().
    BeatState state(TaskId id) const;

private:
    struct Slot {
        TimeMs last_beat_ms = 0;
        uint32_t period_ms = 0;
        TaskTier tier = TaskTier::B;
        bool configured = false;
        bool ever_beaten = false;
        bool stalled = false;
    };

    Slot slots_[kMaxSupervisedTasks]{};
    uint8_t configured_a_ = 0;
    bool tier_a_stalled_ = false;
    bool tier_a_all_fresh_ = false;
};

}  // namespace bw::reliability
