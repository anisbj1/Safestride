#pragma once

// bw/ble/ble_config.h - F19 spec 8: every F19 behavior value in ONE place,
// each named, tagged and justified there. Nothing in bw_ble may hard-code a
// threshold, rate, size or timeout that belongs here (00 7.5 "no magic").

#include <cstddef>
#include <cstdint>

namespace bw::ble {

// ---- advertising / link setup ----------------------------------------------

// [EXPERIMENTAL] Slower advertising saves power; connect latency ~1-2 s.
// Tune with measured connect-time and battery data (F25/F28), spec 8.
constexpr uint32_t kAdvertisingIntervalMs = 1000;

// [EXPERIMENTAL] Fewer ATT payloads per sync record batch. NimBLE clamps to
// the stack maximum (255) if the peer or controller cannot go this high.
constexpr uint16_t kPreferredMtu = 185;

// ---- cadence ---------------------------------------------------------------

// [EXPERIMENTAL] Live-view smoothness vs power (spec D2): notify Live Status
// at most once per second while a peer is subscribed.
constexpr uint32_t kStatusNotifyIntervalMs = 1000;

// [DERIVED] = D2's live-status period: ONE vTaskDelayUntil period drives all
// periodic CommsTask work (battery/event/config are sub-sampled by counters),
// so TaskHealth lateness of CommsTask stays a meaningful single number.
constexpr uint32_t kCommsTickMs = 1000;

// [EXPERIMENTAL] Battery % is quasi-static; 1 Hz would be ~60x useless
// traffic. Tune with measured discharge (F25/F28).
constexpr uint32_t kBatteryNotifyIntervalMs = 60000;

// [EXPERIMENTAL] Heart-rate notify minimum interval. [AUD-6] CommsTask::tick
// may run more often than once per second (BLE writes wake it early), so tick
// count cannot enforce "at most 1 Hz" - the cap sits in PublishGate, at the
// sample, where a fast tick can still emit HR no faster than this interval.
constexpr uint32_t kHrNotifyIntervalMs = 1000;

// [EXPERIMENTAL] Config dump refresh while a peer is connected. The Config
// characteristic is READ without an onRead hook (no provider reads in NimBLE
// callback context, spec D8), so the dump buffer is refreshed here; 5 s
// bounds "just changed a setting" staleness well under human patience.
constexpr uint32_t kConfigRefreshIntervalMs = 5000;

// ---- wire protocol ---------------------------------------------------------

// [DERIVED] Version byte in evolving payloads + the DIS Software Revision
// string; fixed by the BLE_GATT.md v1 table, bump only via CR (02 6.6).
constexpr uint8_t kGattContractVersion = 1;

// [HW] Bluetooth Core Spec Vol 3 Part F 3.2.8: ATT default MTU = 23; the
// floor when a peer negotiates nothing higher.
constexpr uint16_t kDefaultAttMtu = 23;

// [HW] Notification/indication PDU = 1 B opcode + 2 B attribute handle, so
// payload cap = negotiated ATT MTU - 3.
constexpr uint16_t kAttHeaderBytes = 3;

// [DERIVED] sizeof(Event) is pinned to 48 B by CR-002's contract test; the
// Event Notify characteristic must carry the record whole, so it needs
// MTU >= 51 - below that it is suppressed (contract 3 edge case).
constexpr uint16_t kMaxEventNotifyBytes = 48;

// [DERIVED] MTU for Event Notify to be enabled: 48 B record + 3 B ATT
// header. Derived directly from kMaxEventNotifyBytes + kAttHeaderBytes.
// Event-notify floor only; the sync ENGINE needs 52 usable bytes
// (ATT MTU >= 55), see bw/sync_transport.h and CR-019.
constexpr uint16_t kMinEventNotifyMtu = kMaxEventNotifyBytes + kAttHeaderBytes;

// [HW] ATT attribute value cap (BLE Core Spec Vol 3 Part F 3.2.8: an
// attribute value may not exceed 512 B; NimBLE BLE_ATT_ATTR_MAX_LEN = 512
// [MEASURED]). The Config dump therefore holds ~25 settings entries with
// headroom; overflow truncates at an entry boundary with total_count
// reported (snprintf convention, services.md ISettings::list) - the phone
// simply reads again, nothing is fabricated.
constexpr size_t kMaxConfigDumpBytes = 512;

// [EXPERIMENTAL] NimBLE-callback -> CommsTask typed actions (writes +
// subscription changes); sized for fast manual phone use, the push never
// blocks (drops counted), spec 8.
constexpr size_t kGattActionQueueLen = 16;

// [EXPERIMENTAL] F19 -> F20 inbound raw frames; a phone's Request/Ack bursts
// arrive faster than one engine pump only in misuse; drops counted and
// recovery = phone re-request (F20's protocol, CR-019 D9 pull model).
constexpr size_t kSyncFrameQueueLen = 16;

// [EXPERIMENTAL] First restart delay after a BleDown report; x2 growth
// (doubled in BleServer) so a dead radio does not spin the core.
constexpr uint32_t kBleRestartInitialBackoffMs = 1000;

// [EXPERIMENTAL] Backoff cap; F26's policy supersedes when reliability lands.
constexpr uint32_t kBleRestartMaxBackoffMs = 60000;

// ---- wire sizes ------------------------------------------------------------

// [DERIVED] Command wire image is exactly [type u8][arg0 u32][arg1 u32]
// (data_decision.md 4.6, BLE_GATT.md v1): 9 B, pinned by test_codec_command.
constexpr size_t kCommandBytes = 9;

// [DERIVED] Compact Live Status image = 14 B (BLE_GATT.md v1), <= the 20 B
// cap mandated by external.md 6.2.
constexpr size_t kLiveStatusBytes = 14;

// [HW] Standard Heart Rate Measurement (Bluetooth SIG HR Profile): 1 flag
// byte + UINT8 or UINT16 bpm -> at most 3 B for the single field we send.
constexpr size_t kHeartRateMaxBytes = 3;

// [DERIVED] Sync Control WRITE length bound. The v1 control layouts are 8 B
// (Request) and 6 B (Ack); 32 B is 4x headroom for protocol evolution under
// the same version byte. Longer writes (33..kMaxSyncFrameBytes - the
// attribute's max_len) reach onWrite and are COUNTED as writes_rejected, not
// ATT-rejected - F19 is a dumb byte pipe (CR-019 ruling 1) but the
// CHARACTERISTIC length is F19's contract, not F20's (audit AUD-5).
constexpr size_t kMaxSyncControlWriteBytes = 32;

// [EXPERIMENTAL] Config WRITE length bound: [ver][type][value][name_len]
// header is 9 B, names are short identifiers; 64 B allows ~55-char names.
// Longer writes (up to the 512 B attribute cap) are counted as
// writes_rejected; only > 512 B is a real ATT 0x0D from the stack.
constexpr size_t kMaxConfigWriteBytes = 64;

// [DERIVED] Largest bytes blob any GATT write carries into the action queue;
// equals the longest accepted write (Config), so one queue type serves every
// characteristic.
constexpr size_t kMaxActionBytes = kMaxConfigWriteBytes;

// [DERIVED] Battery Level "unknown" value per the standard profile
// (BLE_GATT.md v1): 0xFF until IBatteryMonitor::read().valid.
constexpr uint8_t kBatteryUnknownPercent = 0xFF;

// [EXPERIMENTAL] Upper bound on registered settings for one Config
// dump/write; ISettings::list reports the true total, so a bigger registry
// truncates honestly instead of failing.
constexpr size_t kMaxConfigSettings = 32;

// ---- identity --------------------------------------------------------------

// [DERIVED] Fixed product identity for this build, advertised in the scan
// response (spec D5); recorded in BLE_GATT.md v1 - a rename is a contract
// change once Accepted.
constexpr const char kDeviceLocalName[] = "Anarolepsy";

// ---- UUID scheme (spec D3) -------------------------------------------------

// [DERIVED] One random 128-bit base generated once on 2026-10-15 plus a
// 16-bit index in the last four hex digits (SIG-style). Adding a
// characteristic = allocating the next index in BLE_GATT.md's table; the
// full string is computed by bw::ble::formatGattUuid, so an accidental base
// or index change fails test_ble_contract (host suite).
constexpr char kGattUuidBasePrefix[] = "4bb00319-8811-4387-95a6-f4bb7838";

// Index space, append-only: published values may never be renumbered (CR
// policy once the contract is Accepted).
constexpr uint16_t kUuidIdxService = 0x0000;
constexpr uint16_t kUuidIdxLiveStatus = 0x0001;
constexpr uint16_t kUuidIdxHeartRate = 0x0002;
constexpr uint16_t kUuidIdxEventNotify = 0x0003;
constexpr uint16_t kUuidIdxCommand = 0x0004;
constexpr uint16_t kUuidIdxConfig = 0x0005;
constexpr uint16_t kUuidIdxSyncControl = 0x0006;
constexpr uint16_t kUuidIdxSyncData = 0x0007;
constexpr uint16_t kGattUuidCount = 8;

// [DERIVED] A 128-bit UUID string is 36 chars + NUL.
constexpr size_t kGattUuidStringBytes = 37;

// [DERIVED] device_id = lowercase 12-hex BLE address (D4). DIS strings are
// short build facts; 64 B covers a full "2026-10-15" style revision string
// with headroom, and sets the NimBLE max_len for all four DIS rows.
constexpr size_t kMaxIdentityStringBytes = 64;

// ---- tunable configuration -------------------------------------------------

// One place to tune (spec 7): BleServer is constructed with these defaults;
// a future F24/F25 profile may override without touching library code.
struct Config {
    const char* local_name = kDeviceLocalName;
    uint16_t preferred_mtu = kPreferredMtu;
    uint32_t advertising_interval_ms = kAdvertisingIntervalMs;
    uint32_t status_notify_interval_ms = kStatusNotifyIntervalMs;
    uint32_t battery_notify_interval_ms = kBatteryNotifyIntervalMs;
};

}  // namespace bw::ble
