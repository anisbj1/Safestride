# 02 — INTERFACES · data contracts: ML layer, state/events, alerts/input/commands

> Part of the shared contracts. **Locked: change only via change request.** Section numbers match the original `02` numbering; index in `docs/02_INTERFACES.md`.

## 4. Data contracts


### 4.4 ML layer
```cpp
constexpr size_t kMaxFeatures = 32;           // [EXPERIMENTAL] upper bound for static storage
struct FeatureVector { uint16_t schema_id; uint16_t schema_version; uint8_t count; float v[kMaxFeatures]; TimeMs t_ms; };
struct ModelOutput   { uint8_t n_classes; float p[8]; float confidence; };  // interpretable probabilities
class IModel {                                  // F05; F06/F09 consume it
 public:
  virtual uint16_t schemaId() const = 0;
  virtual Result<void> predict(const FeatureVector&, ModelOutput&) = 0;     // deterministic, no allocation
  virtual Result<size_t> contributions(const FeatureVector&, Contribution* out, size_t max) = 0;  // optional: Err::Unsupported
};
// CR-034 (developer accepted 2026-10-19), in-memory only. F06 produces current
// motion-window evidence, F07 forwards the original timestamp unchanged.
// valid excludes bad/gapped input and unsafe wear; qualifying is ENMO strictly
// above F06's configurable demo threshold, never quiet contact or held Awake.
struct WakeMotionEvidence { TimeMs t_ms; bool valid; bool qualifying; };
struct AlertnessProbabilities { TimeMs t_ms; bool valid; float awake, drowsy, sleep; float confidence;
     DataQuality input_quality; ReasonMask reasons; WakeMotionEvidence wake_motion; };  // CR-010 + CR-034
```

### 4.5 State and events
```cpp
enum class AlertnessState : uint8_t { Unknown, NotWorn, Awake, Drowsy, HighDrowsinessRisk, PossibleSleep };
enum class OperatingMode : uint8_t { Day, Night, Nap, Calibration, Research };
enum class PowerProfile  : uint8_t { Normal, LowPower };
// CR-007 (accepted 2026-10-11): F08 produces, F11/F12 consume (mailbox: app-owned
// LatestValue<EpisodeStatus>). Append-only enums; not persisted or transmitted.
enum class EpisodePhase : uint8_t { Idle, Candidate, Active };
enum class EpisodeClass : uint8_t { Uncertain, Rest, Nap, MainSleep };  // Uncertain == 0 on purpose
struct EpisodeStatus {
    TimeMs t_ms;                // when this status was produced
    TimeMs t_start_ms;          // monotonic start of the current/last episode; 0 when Idle
    EpisodePhase phase;
    EpisodeClass episode_class;  // provisional while phase != Idle; final value is in the close event
    float confidence;           // running confidence 0..1 (F08 §6 D5)
};
using ReasonMask = uint32_t;                    // bitset of ReasonCode (F22 owns text; codes live in bw_core)
enum class ReasonCode : uint8_t { SustainedLowMotion, ProlongedInactivity, HrBelowBaseline, HrTrendChange,
     AbruptMotionDrop, TimeOfDayTypicalDrowsy, MatchesPastConfirmedEvent, UserMarked, PlannedNap,
     LowSignalQuality, SensorUnavailable, DeviceNotWorn,
     SustainedLowAlertnessProbability,  // CR-008 (accepted 2026-10-11), bit 12
     SustainedWakefulnessProbability,   // CR-008, bit 13
     InsufficientEvidence,              // CR-008, bit 14
     PossibleFall, PostureChange, ShortMotionBurst,  // CR-009 (accepted 2026-10-11), bits 15-17
     /* … append only, never renumber */ };
struct StateSnapshot { TimeMs t_ms; AlertnessState alertness; WearState wear; ActivityContext activity;
     SourceQuality quality; OperatingMode mode; PowerProfile power; uint32_t in_state_ms; float confidence; ReasonMask reasons;
     WakeMotionEvidence wake_motion; };

enum class EventType : uint8_t { SleepEpisode, DrowsinessEvent, SuspectedCataplexy, UserMarked, AlertRaised,
     SensorFault, DeviceNotWorn, BatteryWarning, NapPlanned, NapCompleted, TimeSync, Boot };
enum class UserLabel : uint8_t { Unlabeled, Confirmed, Rejected };
struct Event {                                   // fixed-size record, persisted + transmitted; version it
  static constexpr uint8_t kSchemaVersion = 1;
  // Field order is normative: it packs to 48 bytes on xtensa-esp32 (CR-002, accepted
  // 2026-09-30). CR-011 (2026-10-04) re-aligned this block to CR-002's accepted diff —
  // the previous listing had drifted during application. TimeMs first (8-byte aligned),
  // all 1-byte fields last, so the struct packs with zero padding and no packing pragma.
  // Offsets below are pinned by static_assert in bw_core (contract_checks.h).
  TimeMs t_start_ms;                              // offset 0  monotonic within boot_id
  uint32_t seq;                                   // offset 8  assigned by F17, monotonic per device, never reused
  uint32_t boot_id;                               // offset 12 increments each boot
  uint32_t duration_ms;                           // offset 16
  UnixSeconds t_start_unix;                       // offset 20 0 if unknown at creation time
  float confidence;                               // offset 24 0..1, model/heuristic confidence, NOT clinical certainty
  ReasonMask reasons;                             // offset 28
  uint8_t payload[8];                             // offset 32 type-specific small payload (8-byte aligned)
  SourceQuality quality;                          // offset 40
  EventType type;                                 // offset 42
  UserLabel label;                                // offset 43
  ActivityContext context;                        // offset 44
  AlertnessState alertness_at_start;              // offset 45
  uint8_t detector_id;                            // offset 46
  uint8_t schema_version;                         // offset 47 written as kSchemaVersion
};
```

### 4.6 Alerts, input, commands
```cpp
enum class AlertLevel : uint8_t { Normal, Warning, Active, Persistent };
struct AlertStatus { TimeMs t_ms; AlertLevel level; bool acknowledged; ReasonMask reasons; uint32_t since_ms; };

enum class ButtonGesture : uint8_t { ShortPress, LongPress, DoublePress };
struct ButtonEvent { TimeMs t_ms; ButtonId button; ButtonGesture gesture; };

enum class CommandType : uint8_t { AcknowledgeAlert, CancelAlert, NextScreen, OpenMenu, MarkEvent, ConfirmEvent, RejectEvent,
     StartNap, CancelNap, SetMode, SetSetting, SetTime, StartPairing, EnterResearch, ExitResearch, RequestSync };
struct Command { CommandType type; TimeMs t_ms; uint8_t source; /*Button|Ble|Internal*/ uint32_t arg0; uint32_t arg1; };
// SetSetting (CR-003, accepted 2026-09-30): arg0 = static_cast<uint32_t>(SettingKey);
//   arg1 = the value's raw 32 bits — Bool: 0/1 · Int: static_cast<uint32_t>(int32 bits) · Float: IEEE-754 bit pattern.
//   A receiver resolves the key's SettingType from ISettings::list() and decodes arg1 accordingly;
//   an unrecognised key is dropped (Err::NotFound), never guessed at.
// SetTime (CR-021, accepted 2026-10-17): arg0 = unix_seconds (UnixSeconds, u32 LE on the wire);
//   arg1 = 0, reserved. Set by the companion once per connect; the device
//   stamps TimeSync{boot_id, t_mono_ms, unix_s} (external.md 6.1).
```
