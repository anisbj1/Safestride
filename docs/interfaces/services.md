# 02 — INTERFACES · service/provider interfaces

> Part of the shared contracts. **Locked: change only via change request.** Section numbers match the original `02` numbering; index in `docs/02_INTERFACES.md`.

## 5. Service/provider interfaces
```cpp
class IStateProvider  { public: virtual bool latest(StateSnapshot&) const = 0; };
class IFeatureProvider{ public: virtual bool latestMotion(MotionFeatures&) const = 0; virtual bool latestPpg(PpgFeatures&) const = 0; };
class IEventSink      { public: virtual Result<void> publish(const Event&) = 0; };                 // → F17
class IEventStore {                                                                                 // F18 implements, F17/F20/F16 use
 public:
  virtual Result<uint32_t> append(const Event&) = 0;                       // returns seq
  virtual Result<size_t>   readFrom(uint32_t from_seq, Event* out, size_t max) = 0;
  virtual Result<void>     setLabel(uint32_t seq, UserLabel) = 0;
  virtual Result<void>     markSyncedThrough(uint32_t seq) = 0;
  virtual uint32_t         lastSyncedSeq() const = 0;
};
class IKeyValueStore  { public: virtual Result<size_t> get(const char* key, void* buf, size_t cap) = 0;
                                virtual Result<void> put(const char* key, const void* data, size_t len) = 0; };
enum class SettingKey : uint16_t { /* registered settings; append only, never renumber */ };
enum class SettingType : uint8_t { Bool, Int, Float };
struct SettingValue {                             // trivially copyable; only the member matching `type` is meaningful
  SettingType type;
  union { bool b; int32_t i; float f; };
};
struct SettingSpec {                              // the registration table: one entry per key
  SettingKey  key;
  SettingType type;
  SettingValue min;                               // bounds expressed in the value's own type
  SettingValue max;
  SettingValue default_v;
  const char* name;                               // stable identifier for IKeyValueStore and the BLE Config characteristic
};
class ISettings {                                 // registration table is fixed at wiring time; lookups are O(1)
 public:
  virtual Result<SettingValue> get(SettingKey) const = 0;             // Err::NotFound for an unregistered key
  virtual Result<void>         set(SettingKey, const SettingValue&) = 0;  // type mismatch -> Err::InvalidArg;
                                                                         // outside min..max -> clamped, not rejected
  // Returns the TOTAL number of registered keys (never less than what is written) and fills at
  // most `max` entries, first `max` in registration order. `returned > max` therefore means
  // "truncated — call again with a bigger buffer". Same convention as snprintf()/va_list.
  virtual size_t               list(SettingSpec* out, size_t max) const = 0;  // fills the range/default table (UI + reset)
};
class ICommandSink    { public: virtual bool push(const Command&) = 0; };                         // BLE, buttons, UI push here
class IFaultReporter  { public: virtual void report(FaultCode, FaultSeverity, TimeMs) = 0; };     // → F26; all features may call
class IBaselineProvider { public: virtual bool profile(BaselineProfile&) const = 0; };            // F13; consumers must accept "not available"
struct BaselineProfile { bool valid; float resting_hr_bpm; float resting_hr_mad; float typical_enmo_g; uint8_t confidence_pct; uint16_t version; };
struct HealthSnapshot { TimeMs t_ms; SourceQuality quality; uint8_t battery_percent; bool battery_valid;
     uint32_t min_free_heap; uint16_t worst_task_lateness_ms; bool ble_connected; uint32_t fault_bits; bool degraded_mode; };
// CR-015 (accepted 2026-10-12): storage health, F18 produces / F23 consumes. In-memory only, no kSchemaVersion.
struct StorageHealth { TimeMs t_ms; uint32_t total_bytes; uint32_t free_bytes; uint32_t corrupt_records;
     uint32_t torn_tails_repaired; uint32_t segments_skipped; uint32_t evicted_synced_segments;
     uint32_t evicted_windows; uint32_t kv_discards; };
class IStorageHealthProvider { public: virtual bool latest(StorageHealth&) const = 0; };  // false = no snapshot yet
// CR-016 (accepted 2026-10-12): confirm prompt state, F14 produces / F12 consumes.
enum class ConfirmPromptPhase : uint8_t { Idle, Deferred, Offered };  // append-only, 0 = Idle
struct ConfirmPromptState { static constexpr uint8_t kSchemaVersion = 1; TimeMs t_ms; uint32_t seq;
     ConfirmPromptPhase phase; TimeMs offer_deadline_ms; uint8_t queue_depth; };
class IConfirmPromptProvider { public: virtual bool latest(ConfirmPromptState&) const = 0; };  // false = no state yet
// CR-018 (accepted 2026-10-15): BLE link state, F19 produces / F12+F23 consume. In-memory only, no kSchemaVersion.
struct BleLinkState { TimeMs t_ms; bool connected; bool encrypted; uint16_t mtu; };            // mtu = 23 before negotiation
class IBleLinkProvider { public: virtual bool latest(BleLinkState&) const = 0; };              // false = no snapshot yet
// CR-019 (accepted 2026-10-15): sync transport seam, full text in bw/sync_transport.h (F19 adapter implements, F20 engine consumes).
enum class SyncChannel : uint8_t { Control = 0, Data = 1 };                                    // append-only, 0 = Control
constexpr uint16_t kMaxSyncFrameBytes = 252;                                                   // [DERIVED] ATT MTU 255 - 3 B header
class ISyncTransport {                                                                         // frames opaque; protocol semantics = F20 (SYNC_PROTOCOL.md)
 public: virtual Result<void> send(SyncChannel, const uint8_t*, uint16_t) = 0;                 // NotReady / Busy / InvalidArg
         virtual Result<size_t> receive(SyncChannel*, uint8_t*, size_t) = 0;                   // Ok(0) = none pending; Overflow = dropped
         virtual bool linkUp() const = 0;                                                      // connected + encrypted + subscribed
         virtual uint16_t maxSendBytes() const = 0; };                                         // usable >= 52 B (MTU >= 55) to stream
// CR-019 part B: sync status for UI/BLE, F20 produces / F12+F21 consume later.
struct SyncStatus { static constexpr uint8_t kSchemaVersion = 1; TimeMs t_ms; uint32_t last_seq;
     uint32_t last_synced_seq; uint32_t events_sent; uint32_t chunks_resent; uint16_t protocol_version;
     uint8_t flags; };                                                                         // bit0 linkUp..bit3 transfer_aborted
class ISyncStatusProvider { public: virtual bool latest(SyncStatus&) const = 0; };             // false = no snapshot yet
// CR-027 (accepted 2026-10-18, W6 G1): operating-mode profile, F24 produces / F01+F10+F12 consume later. Full text in bw/mode_profile.h.
struct ModeProfile { static constexpr uint8_t kSchemaVersion = 1; TimeMs t_ms; OperatingMode mode; PowerProfile power;
     uint8_t ui_brightness_pct; AlertLevel alert_max_level; bool wall_time_known; };           // hints, never commands
class IModeProvider { public: virtual bool latest(ModeProfile&) const = 0; };                 // false = no profile yet
// CR-029 + CR-030 (accepted 2026-10-18, W6 G1): degradation facilities + fault-registry read path,
// F26 produces / F23 consumes (F25/F12/F24 may consume the flags later). In-memory only, no kSchemaVersion.
enum class DegradedFacility : uint8_t { PpgLost = 0, ImuLost, StorageDegraded, BleDown, SafeMode, Count };  // append-only
using DegradedMode = uint32_t;                                                               // bit i <=> DegradedFacility i
struct ReliabilityState { TimeMs t_ms; uint32_t fault_bits; DegradedMode degraded; bool safe_mode; };  // sticky bits, no clearing
class IReliabilityStateProvider { public: virtual bool latest(ReliabilityState&) const = 0; };  // false = no snapshot yet
```
Ownership of implementations: F17 `EventLogger : IEventSink`; F18 `LittleFsEventStore : IEventStore` and `KvStore : IKeyValueStore`; F26 `FaultRegistry : IFaultReporter`; F13 `BaselineTracker : IBaselineProvider`; F23 produces `HealthSnapshot` and `SourceQuality`; F07 produces `StateSnapshot`.
