#pragma once

// bw/ble/codec.h - F19 spec 7 (Codec row): pure encode/decode for every
// custom wire image in docs/contracts/BLE_GATT.md v1. No FreeRTOS/Arduino,
// no providers owned, no globals - host-tested on its own (spec 9).
//
// Rules baked in here (spec D6): little-endian multi-byte integers; the
// version byte kGattContractVersion in evolving payloads (Live Status,
// Config, Sync Control writes); "no data" is never encoded, it is refused.
// Err returns are SEMANTIC rejects: they travel up the task side where the
// caller COUNTS them (writes_rejected / config_rejected / command_rejected).
// NimBLE 2.5.1's write callback cannot reject, so these Errs are never
// translated into ATT errors by the write path (audit AUD-5; the two real
// ATT errors are produced by the stack itself, see BLE_GATT.md v1 3).
//
// CR-019 ruling 1: F19 does NOT parse Sync Control protocol semantics -
// Request/Ack framing, opcodes and CRCs belong to F20 (SYNC_PROTOCOL.md).
// The only Sync Control rule F19 enforces is the characteristic write
// LENGTH bound (checkSyncControlWrite).

#include <cstddef>
#include <cstdint>

#include <bw/ble/ble_config.h>
#include <bw/commands.h>
#include <bw/events.h>
#include <bw/features.h>
#include <bw/hardware.h>  // BatteryReading for encodeBatteryLevel
#include <bw/providers.h>
#include <bw/result.h>
#include <bw/state.h>
#include <bw/types.h>

namespace bw::ble {

// Full 128-bit UUID string for one index of the D3 base+index scheme,
// e.g. index 0 -> "4bb00319-8811-4387-95a6-f4bb78380000" (service).
//  Err::InvalidArg  index >= kGattUuidCount, or cap < kGattUuidStringBytes.
Result<void> formatGattUuid(uint16_t index, char* out, size_t cap);

// ---- Live Status -----------------------------------------------------------
// Wire image (BLE_GATT.md v1, 14 B LE):
//   [0]      ver (kGattContractVersion)
//   [1]      alertness   [2] wear   [3] activity
//   [4]      quality     [5] mode   [6] power
//   [7..8]   in_state_s  u16, seconds, saturated at 0xFFFF
//   [9]      confidence_pct  u8, clamped from the float in [0,1]
//   [10..13] reasons     u32 (full ReasonMask)
struct LiveStatus {
    uint8_t alertness;
    uint8_t wear;
    uint8_t activity;
    uint8_t quality;
    uint8_t mode;
    uint8_t power;
    uint16_t in_state_s;
    uint8_t confidence_pct;
    uint32_t reasons;
};

//  Err::InvalidArg  cap < kLiveStatusBytes (nothing written).
Result<size_t> encodeLiveStatus(const StateSnapshot& snap, uint8_t* out, size_t cap);

//  Err::InvalidArg  len != kLiveStatusBytes or version byte unknown.
Result<void> decodeLiveStatus(const uint8_t* data, size_t len, LiveStatus& out);

// ---- Heart Rate ------------------------------------------------------------
// Standard 0x2A37 image: [flags u8][bpm u8 or u16 LE], flags bit0 = UINT16.
// Only the heart-rate field is sent; no RR, no sensor contact bits.

//  Err::InvalidArg  PpgFeatures says no reliable rate (hr_valid == false),
//                   bpm <= 0, non-finite, or cap < needed bytes.
//                   Unknown HR is never encoded (spec D6).
Result<size_t> encodeHeartRate(const PpgFeatures& ppg, uint8_t* out, size_t cap);

struct HeartRateValue {
    bool wide;     // true = UINT16 format flag was set
    uint16_t bpm;  // rounded beats per minute
};

//  Err::InvalidArg  bad length, or a flag bit we never send (reject, never
//                   guess at another layout).
Result<HeartRateValue> decodeHeartRate(const uint8_t* data, size_t len);

// ---- Command ---------------------------------------------------------------
// Wire image (9 B): [type u8][arg0 u32 LE][arg1 u32 LE]. t_ms and source are
// stamped by the DEVICE from the passed clock reading - the phone cannot
// set either (spec D6, data_decision.md 4.6).

//  Err::InvalidArg  len != kCommandBytes or unknown CommandType.
Result<Command> decodeCommand(const uint8_t* data, size_t len, TimeMs t_ms);

// The BLE-origin command source byte (data_decision.md 4.6 comment order
// Button|Ble|Internal; [ASSUMED] value 1, spec 13).
constexpr uint8_t kCommandSourceBle = 1;

// ---- Config ----------------------------------------------------------------
// READ dump (BLE_GATT.md v1): [ver u8][total_keys u16][entries...], one entry
// = [type u8][value u32][name_len u16][name bytes]. WRITE: [ver u8][type u8]
// [value u32][name_len u16][name bytes].

// Fills `specs` via ISettings::list (first spec_cap in registration order),
// then writes the dump into `out`. Truncates at an ENTRY boundary; the
// header always reports the true total so the phone can ask a bigger read
// (snprintf convention).
//  Err::InvalidArg  out == nullptr or specs == nullptr.
Result<size_t> encodeConfigDump(ISettings& settings, SettingSpec* specs, size_t spec_cap,
                                uint8_t* out, size_t cap);

// Decodes one Config write and applies it through ISettings::set (which
// clamps out-of-range values, services.md 5 - clamp, never reject).
//  Err::InvalidArg  malformed length, unknown version byte, or type mismatch
//                   between the wire `type` and the registered setting.
//  Err::NotFound    no setting registered under that name.
//                   Both are COUNTED semantic rejects (config_rejected on the
//                   task side), never ATT errors (audit AUD-5).
Result<void> applyConfigWrite(ISettings& settings, SettingSpec* specs, size_t spec_cap,
                              const uint8_t* data, size_t len);

// ---- Battery ----------------------------------------------------------------
// Standard 0x2A19 image: one u8 percent 0..100, or 0xFF = "unknown" (BLE
// Battery Service; BLE_GATT.md v1). Lives here - not in the device-only
// BleServer - so the mapping is pure and host-tested (audit AUD-7):
//  !reading.valid                    -> kBatteryUnknownPercent (0xFF)
//  non-finite percent_estimate       -> 0xFF (bad data is not a level)
//  p <= 0                            -> 0, p >= 100 -> 100 (clamped)
//  otherwise                         -> round(p) = uint8(p + 0.5)
uint8_t encodeBatteryLevel(const BatteryReading& reading);

// ---- Sync Control (write side only) ----------------------------------------
// F19 is a dumb byte pipe (CR-019 ruling 1): opcodes, versions and payload
// semantics are F20's. F19 enforces only the characteristic write length.
//  Err::InvalidArg  len == 0 or len > kMaxSyncControlWriteBytes.
Result<void> checkSyncControlWrite(const uint8_t* data, size_t len);

// ---- Event -----------------------------------------------------------------
// Event Notify carries the CANONICAL 48-byte bw::Event image verbatim
// (data_decision.md 4.5 / CR-002). No second encoding exists (CR-019
// ruling 3): this is a passthrough, pinned by static_assert + the host
// contract test.

//  Err::InvalidArg  cap < sizeof(Event).
Result<size_t> encodeEvent(const Event& event, uint8_t* out, size_t cap);

}  // namespace bw::ble
