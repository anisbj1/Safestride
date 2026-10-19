#pragma once

// bw/providers.h — docs/02_INTERFACES.md §5 (service/provider interfaces).
// Implementations: F17 EventLogger (IEventSink), F18 KvStore/LittleFsEventStore,
// F13 BaselineTracker, F07 StateSnapshot (IStateProvider), F23 HealthSnapshot.
// F00 only declares them (plus the CR-003 settings types below).
//
// CR-003 (accepted 2026-09-30) supplies the ISettings signature that 02 §5 left as a
// prose comment; 02's diff has not been applied yet (integrator's step, §15 of F00).

#include <cstddef>
#include <cstdint>

#include <bw/commands.h>
#include <bw/events.h>
#include <bw/features.h>
#include <bw/quality.h>
#include <bw/result.h>
#include <bw/state.h>
#include <bw/types.h>

namespace bw {

// ---- settings (CR-003) ------------------------------------------------------

// Registered settings; append only, never renumber (02 §6.6). Deliberately empty at
// M0: each key is registered by the feature that owns it, when it lands (CR-003) —
// F00 does not pre-commit anyone's settings list.
enum class SettingKey : uint16_t {};

enum class SettingType : uint8_t { Bool, Int, Float };

// Trivially copyable; only the member matching `type` is meaningful (02 §1's
// validity-flag rule applied to a union: read the WRONG member and you get garbage —
// always switch on `type` first).
struct SettingValue {
    SettingType type;
    union {
        bool b;
        int32_t i;
        float f;
    };
};

// The registration table: one entry per key (CR-003). Bounds are expressed in the
// value's own type; `name` points at a static literal (IKeyValueStore and the BLE
// Config characteristic identify settings by it) — no owned memory.
struct SettingSpec {
    SettingKey key;
    SettingType type;
    SettingValue min;
    SettingValue max;
    SettingValue default_v;
    const char* name;
};

// Registration table is fixed at wiring time; lookups are O(1) (CR-003).
class ISettings {
public:
    virtual Result<SettingValue> get(SettingKey) const = 0;  // unregistered key -> Err::NotFound
    // Type mismatch -> Err::InvalidArg; outside min..max -> clamped, not rejected.
    virtual Result<void> set(SettingKey, const SettingValue&) = 0;
    // Fills the range/default table for UI (F12) and reset paths; returns count.
    virtual size_t list(SettingSpec* out, size_t max) const = 0;
};

// ---- providers --------------------------------------------------------------

class IStateProvider {
public:
    virtual bool latest(StateSnapshot&) const = 0;  // false = no snapshot yet
};

class IFeatureProvider {
public:
    virtual bool latestMotion(MotionFeatures&) const = 0;  // false = none yet
    virtual bool latestPpg(PpgFeatures&) const = 0;
};

class IEventSink {
public:
    virtual Result<void> publish(const Event&) = 0;  // → F17
};

// F18 implements, F17/F20/F16 use (02 §5).
class IEventStore {
public:
    virtual Result<uint32_t> append(const Event&) = 0;  // returns seq
    virtual Result<size_t> readFrom(uint32_t from_seq, Event* out, size_t max) = 0;
    virtual Result<void> setLabel(uint32_t seq, UserLabel) = 0;
    virtual Result<void> markSyncedThrough(uint32_t seq) = 0;
    virtual uint32_t lastSyncedSeq() const = 0;
};

class IKeyValueStore {
public:
    virtual Result<size_t> get(const char* key, void* buf, size_t cap) = 0;
    virtual Result<void> put(const char* key, const void* data, size_t len) = 0;
};

class ICommandSink {
public:
    virtual bool push(const Command&) = 0;  // BLE, buttons, UI push here
};

// F13; consumers must accept "not available" (02 §5) — check `valid`, never guess.
struct BaselineProfile {
    bool valid;
    float resting_hr_bpm;
    float resting_hr_mad;
    float typical_enmo_g;
    uint8_t confidence_pct;
    uint16_t version;
};

class IBaselineProvider {
public:
    virtual bool profile(BaselineProfile&) const = 0;
};

// F23 produces this; the whole snapshot must degrade to "unknown" fields, not to
// fabricated values, when a source is missing (00 §1).
struct HealthSnapshot {
    TimeMs t_ms;
    SourceQuality quality;
    uint8_t battery_percent;
    bool battery_valid;  // false = battery unknown (A-04: no divider seen)
    uint32_t min_free_heap;
    uint16_t worst_task_lateness_ms;
    bool ble_connected;
    uint32_t fault_bits;  // aggregated from FaultCode reports (F26/F23)
    bool degraded_mode;
};

// CR-015 (accepted 2026-10-12) — F18 produces, F23 (and diagnostics) consume.
// Aggregation of F18's store counters. In-memory only, never persisted or
// transmitted → deliberately no kSchemaVersion (core.md §1).
struct StorageHealth {
    TimeMs t_ms;  // when produced
    uint32_t total_bytes;
    uint32_t free_bytes;
    uint32_t corrupt_records;
    uint32_t torn_tails_repaired;
    uint32_t segments_skipped;
    uint32_t evicted_synced_segments;
    uint32_t evicted_windows;
    uint32_t kv_discards;
};

class IStorageHealthProvider {
public:
    virtual bool latest(StorageHealth&) const = 0;  // false = no snapshot yet
};

// CR-016 (accepted 2026-10-12) — F14 produces, F12 consumes.
enum class ConfirmPromptPhase : uint8_t { Idle, Deferred, Offered };  // append-only, 0 = Idle

// Trivially copyable; safe to publish through a LatestValue mailbox.
struct ConfirmPromptState {
    static constexpr uint8_t kSchemaVersion = 1;
    TimeMs t_ms;   // when produced
    uint32_t seq;  // event currently offered; 0 = none
    ConfirmPromptPhase phase;
    TimeMs offer_deadline_ms;  // prompt auto-resolves at this monotonic time; 0 unless Offered
    uint8_t queue_depth;       // pending events behind the offered one
};

class IConfirmPromptProvider {
public:
    virtual bool latest(ConfirmPromptState&) const = 0;  // false = no state yet
};

// CR-018 (accepted 2026-10-15) - BLE link state, F19 produces / F12+F23 consume.
// In-memory only, never persisted or transmitted -> no kSchemaVersion (core.md §1).
struct BleLinkState {
    TimeMs t_ms;     // when produced
    bool connected;  // >= 1 GAP link up
    bool encrypted;  // link encrypted & bonded (F19 D1: Just Works)
    uint16_t mtu;    // negotiated ATT MTU; 23 (kDefaultAttMtu) before negotiation
};

class IBleLinkProvider {
public:
    virtual bool latest(BleLinkState&) const = 0;  // false = no snapshot yet
};

// CR-029 (accepted 2026-10-18, W6 G1) — which capability is reduced. F26 produces,
// F23 consumes as its `degraded_mode` source; F25/F12/F24 may consume later.
// Append-only; in-memory only, never persisted or transmitted -> no kSchemaVersion
// (core.md §1). Distinct from FaultCode: a facility flag names the CAPABILITY that
// is down, a fault code names the SYMPTOM that was reported.
enum class DegradedFacility : uint8_t {
    PpgLost = 0,      // PPG pipeline unusable (F01/F23 signal)
    ImuLost,          // IMU pipeline unusable
    StorageDegraded,  // F18 store refusing/losing data
    BleDown,          // sync capability unavailable (F19/F20)
    SafeMode,         // BootGuard safe mode active (F26)
    Count,            // sentinel; never stored as a flag
};
using DegradedMode = uint32_t;  // bit i set <=> DegradedFacility i active

// CR-030 (accepted 2026-10-18, W6 G1) — the read path back from F26's fault
// registry into F23's HealthSnapshot.fault_bits (gap recorded at bw/fault.h and in
// F23 spec §15.8). F26 produces, F23 consumes. In-memory only, no kSchemaVersion.
struct ReliabilityState {
    TimeMs t_ms;            // when produced
    uint32_t fault_bits;    // bit N <=> FaultCode value N reported at least once
                            // this boot; sticky, never cleared — same mapping as
                            // F23 D8; the registry is a superset of F23's own bits
    DegradedMode degraded;  // CR-029 facility mask
    bool safe_mode;         // BootGuard safe mode active
};

class IReliabilityStateProvider {
public:
    virtual bool latest(ReliabilityState&) const = 0;  // false = no snapshot yet
};

}  // namespace bw
