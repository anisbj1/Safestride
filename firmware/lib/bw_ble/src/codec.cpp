#include <bw/ble/codec.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace bw::ble {

namespace {

void putU16Le(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}

void putU32Le(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

uint16_t getU16Le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1] << 8);
}

uint32_t getU32Le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Raw 32-bit image of a SettingValue, per CR-003 (data_decision.md 4.6):
// Bool 0/1, Int = two's-complement bits, Float = IEEE-754 bits.
uint32_t settingValueBits(const SettingValue& v) {
    switch (v.type) {
    case SettingType::Bool:
        return v.b ? 1u : 0u;
    case SettingType::Int:
        return static_cast<uint32_t>(v.i);
    case SettingType::Float: {
        uint32_t bits = 0;
        std::memcpy(&bits, &v.f, sizeof(bits));
        return bits;
    }
    }
    return 0;  // unknown enum value: zero is honest "no value", never a guess
}

SettingValue makeSettingValue(SettingType type, uint32_t bits) {
    SettingValue v{};
    v.type = type;
    switch (type) {
    case SettingType::Bool:
        v.b = (bits & 0x1u) != 0;
        break;
    case SettingType::Int:
        v.i = static_cast<int32_t>(bits);
        break;
    case SettingType::Float:
        std::memcpy(&v.f, &bits, sizeof(v.f));
        break;
    }
    return v;
}

// Worst-of rule: the single quality byte must not claim better than the
// weaker source (00 -1: "no reliable data" beats a guess; a dead PPG must
// not be hidden behind a healthy IMU).
uint8_t worstQuality(const SourceQuality& q) {
    const uint8_t imu = static_cast<uint8_t>(q.imu);
    const uint8_t ppg = static_cast<uint8_t>(q.ppg);
    return imu > ppg ? imu : ppg;
}

}  // namespace

Result<void> formatGattUuid(uint16_t index, char* out, size_t cap) {
    if (out == nullptr || cap < kGattUuidStringBytes || index >= kGattUuidCount) {
        return {Err::InvalidArg};
    }
    // D3: base + 16-bit index in the last four hex digits; one formatter
    // feeds both the device GATT table and the host contract test, so doc,
    // code and test can never drift apart silently.
    std::snprintf(out, cap, "%s%04x", kGattUuidBasePrefix, static_cast<unsigned>(index));
    return {Err::Ok};
}

Result<size_t> encodeLiveStatus(const StateSnapshot& snap, uint8_t* out, size_t cap) {
    if (out == nullptr || cap < kLiveStatusBytes) {
        return {Err::InvalidArg, 0};
    }
    // in_state_s saturates (test pin): a 16-bit seconds field must clip, not
    // wrap - wrapped durations would read as "just entered" states.
    uint32_t seconds = snap.in_state_ms / 1000u;
    if (seconds > 0xFFFFu) {
        seconds = 0xFFFFu;
    }
    // Confidence clamp: the contract carries [0,1] as percent; NaN and
    // out-of-range floats collapse to 0 - never a fabricated confidence.
    const float c = snap.confidence;
    uint8_t pct = 0;
    if (c > 0.0f) {  // false for NaN too
        const float clamped = c > 1.0f ? 1.0f : c;
        pct = static_cast<uint8_t>(clamped * 100.0f + 0.5f);
    }

    out[0] = kGattContractVersion;
    out[1] = static_cast<uint8_t>(snap.alertness);
    out[2] = static_cast<uint8_t>(snap.wear);
    out[3] = static_cast<uint8_t>(snap.activity);
    out[4] = worstQuality(snap.quality);
    out[5] = static_cast<uint8_t>(snap.mode);
    out[6] = static_cast<uint8_t>(snap.power);
    putU16Le(&out[7], static_cast<uint16_t>(seconds));
    out[9] = pct;
    putU32Le(&out[10], snap.reasons);
    return {Err::Ok, kLiveStatusBytes};
}

Result<void> decodeLiveStatus(const uint8_t* data, size_t len, LiveStatus& out) {
    if (data == nullptr || len != kLiveStatusBytes || data[0] != kGattContractVersion) {
        return {Err::InvalidArg};
    }
    out.alertness = data[1];
    out.wear = data[2];
    out.activity = data[3];
    out.quality = data[4];
    out.mode = data[5];
    out.power = data[6];
    out.in_state_s = getU16Le(&data[7]);
    out.confidence_pct = data[9];
    out.reasons = getU32Le(&data[10]);
    return {Err::Ok};
}

Result<size_t> encodeHeartRate(const PpgFeatures& ppg, uint8_t* out, size_t cap) {
    // No fabricated heart rate, ever (00 -1, spec D6): invalid, non-positive
    // or non-finite rates are refused so the caller sends NO notification.
    if (!ppg.hr_valid || !(ppg.hr_bpm > 0.0f) || !std::isfinite(ppg.hr_bpm)) {
        return {Err::InvalidArg, 0};
    }
    const bool wide = ppg.hr_bpm > 255.0f;
    const size_t need = wide ? 3u : 2u;
    if (out == nullptr || cap < need) {
        return {Err::InvalidArg, 0};
    }
    const uint16_t bpm = static_cast<uint16_t>(ppg.hr_bpm + 0.5f);  // rounded, then range-clipped
    out[0] = wide ? 0x01u : 0x00u;                                  // flags: bit0 = UINT16 format
    if (wide) {
        putU16Le(&out[1], bpm);
    } else {
        out[1] = static_cast<uint8_t>(bpm > 255u ? 255u : bpm);
    }
    return {Err::Ok, need};
}

Result<HeartRateValue> decodeHeartRate(const uint8_t* data, size_t len) {
    HeartRateValue out{};
    if (data == nullptr || len < 2) {
        return {Err::InvalidArg, out};
    }
    const bool wide = (data[0] & 0x01u) != 0;
    // Reject anything we never encode (RR intervals, energy expended, extra
    // flags): reject, never guess at another layout (spec D6).
    if ((data[0] & ~0x01u) != 0) {
        return {Err::InvalidArg, out};
    }
    if (wide) {
        if (len < 3) {
            return {Err::InvalidArg, out};
        }
        out.wide = true;
        out.bpm = getU16Le(&data[1]);
        return {Err::Ok, out};
    }
    out.wide = false;
    out.bpm = data[1];
    return {Err::Ok, out};
}

Result<Command> decodeCommand(const uint8_t* data, size_t len, TimeMs t_ms) {
    Command cmd{};
    if (data == nullptr || len != kCommandBytes) {
        return {Err::InvalidArg, cmd};
    }
    const uint8_t type = data[0];
    if (type > static_cast<uint8_t>(CommandType::RequestSync)) {
        return {Err::InvalidArg, cmd};  // append-only enum: unknown is rejected
    }
    cmd.type = static_cast<CommandType>(type);
    cmd.arg0 = getU32Le(&data[1]);
    cmd.arg1 = getU32Le(&data[5]);
    // The device stamps time and origin; the phone can set neither (D6).
    cmd.t_ms = t_ms;
    cmd.source = kCommandSourceBle;
    return {Err::Ok, cmd};
}

Result<size_t> encodeConfigDump(ISettings& settings, SettingSpec* specs, size_t spec_cap,
                                uint8_t* out, size_t cap) {
    if (specs == nullptr || out == nullptr || cap < 3) {
        return {Err::InvalidArg, 0};
    }
    // snprintf convention (services.md 5): list() returns the TOTAL key
    // count and fills at most spec_cap entries in registration order.
    const size_t total = settings.list(specs, spec_cap);
    const size_t returned = total < spec_cap ? total : spec_cap;

    size_t n = 0;
    out[n++] = kGattContractVersion;
    putU16Le(&out[n], static_cast<uint16_t>(total));
    n += 2;

    for (size_t i = 0; i < returned; ++i) {
        const SettingSpec& spec = specs[i];
        const size_t name_len = spec.name != nullptr ? std::strlen(spec.name) : 0;
        const size_t entry_bytes = 1 + 4 + 2 + name_len;
        if (n + entry_bytes > cap) {
            break;  // truncate at an ENTRY boundary, total_count still true
        }
        // Current value; an unreadable key falls back to its registered
        // default rather than emitting garbage bits.
        const Result<SettingValue> current = settings.get(spec.key);
        const SettingValue& value = current.ok() ? current.value : spec.default_v;
        out[n++] = static_cast<uint8_t>(spec.type);
        putU32Le(&out[n], settingValueBits(value));
        n += 4;
        putU16Le(&out[n], static_cast<uint16_t>(name_len));
        n += 2;
        if (name_len > 0) {
            std::memcpy(&out[n], spec.name, name_len);
            n += name_len;
        }
    }
    return {Err::Ok, n};
}

Result<void> applyConfigWrite(ISettings& settings, SettingSpec* specs, size_t spec_cap,
                              const uint8_t* data, size_t len) {
    if (specs == nullptr || data == nullptr || len < 9) {
        return {Err::InvalidArg};
    }
    if (data[0] != kGattContractVersion) {
        return {Err::InvalidArg};  // unknown version: reject, never translate
    }
    const uint8_t wire_type = data[1];
    const uint32_t bits = getU32Le(&data[2]);
    const uint16_t name_len = getU16Le(&data[6]);
    if (len != 9u + name_len) {
        // Counted semantic reject (config_rejected, task side): the write was
        // length-legal at ATT level, so no ATT error can follow (AUD-5).
        return {Err::InvalidArg};
    }

    const size_t total = settings.list(specs, spec_cap);
    const size_t returned = total < spec_cap ? total : spec_cap;
    const char* name = reinterpret_cast<const char*>(&data[9]);

    const SettingSpec* match = nullptr;
    for (size_t i = 0; i < returned; ++i) {
        const SettingSpec& spec = specs[i];
        const size_t spec_len = spec.name != nullptr ? std::strlen(spec.name) : 0;
        if (spec_len == name_len && std::memcmp(spec.name, name, name_len) == 0) {
            match = &spec;
            break;
        }
    }
    if (match == nullptr) {
        return {Err::NotFound};  // counted semantic reject -> config_rejected
    }
    if (wire_type != static_cast<uint8_t>(match->type)) {
        return {Err::InvalidArg};  // counted semantic reject -> config_rejected
    }
    // Out-of-range values are CLAMPED by ISettings::set, never rejected
    // (services.md 5) - the fake and the real store both honor that.
    return settings.set(match->key, makeSettingValue(match->type, bits));
}

Result<void> checkSyncControlWrite(const uint8_t* data, size_t len) {
    (void)data;  // content is opaque to F19 (CR-019 ruling 1)
    if (len == 0 || len > kMaxSyncControlWriteBytes) {
        // Length-bound refusal: counted (writes_rejected, callback side),
        // never an ATT error - the attribute's max_len is kMaxSyncFrameBytes,
        // so 33..kMaxSyncFrameBytes reaches the callback (gatt_database.cpp).
        return {Err::InvalidArg};
    }
    return {Err::Ok};
}

uint8_t encodeBatteryLevel(const BatteryReading& reading) {
    if (!reading.valid) {
        return kBatteryUnknownPercent;  // 0xFF: honest unknown, never a fabricated level
    }
    const float p = reading.percent_estimate;
    if (!std::isfinite(p)) {
        // NaN/Inf out of the estimator is BAD DATA, not a percentage: the
        // contract's 0xFF "unknown" byte is the only honest answer. Returning
        // 0 here would broadcast "empty battery" from a broken reading
        // (audit AUD-7; 00 1 "unknown is a valid output").
        return kBatteryUnknownPercent;
    }
    if (p <= 0.0f) {
        return 0;  // clamped: the estimate is at or below empty
    }
    if (p >= 100.0f) {
        return 100;  // clamped: never wrap past 100 into "unknown"
    }
    return static_cast<uint8_t>(p + 0.5f);  // round half up, contract convention
}

Result<size_t> encodeEvent(const Event& event, uint8_t* out, size_t cap) {
    if (out == nullptr || cap < kMaxEventNotifyBytes) {
        return {Err::InvalidArg, 0};
    }
    // Verbatim canonical image: bw::Event is pinned to 48 packed bytes
    // (CR-002, data_decision.md 4.5) on little-endian targets; F19 never
    // re-encodes it (CR-019 ruling 3 - no second event encoding).
    static_assert(sizeof(Event) == kMaxEventNotifyBytes,
                  "Event Notify is pinned to the canonical 48-byte bw::Event image");
    std::memcpy(out, &event, sizeof(Event));
    return {Err::Ok, sizeof(Event)};
}

}  // namespace bw::ble
