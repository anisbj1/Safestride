#pragma once

// bw_ble suite support (spec 9): fakes for the provider contracts F19
// consumes. Host-only by construction - nothing here ships with firmware.

#include <cstring>

#include <bw/ble/ble_config.h>
#include <bw/ble/sync_link.h>
#include <bw/providers.h>
#include <bw/result.h>

namespace bw_ble_test {

// ISettings fake honoring the CR-003 semantics (services.md 5): list() in
// registration order with the snprintf total convention, get() NotFound for
// unregistered keys, set() type-checked and CLAMPING to min..max (never
// rejecting out-of-range - F19 relies on exactly that, spec 9).
class FakeSettings final : public bw::ISettings {
public:
    static bw::SettingValue makeBool(bool b) {
        bw::SettingValue v{};
        v.type = bw::SettingType::Bool;
        v.b = b;
        return v;
    }
    static bw::SettingValue makeInt(int32_t i) {
        bw::SettingValue v{};
        v.type = bw::SettingType::Int;
        v.i = i;
        return v;
    }
    static bw::SettingValue makeFloat(float f) {
        bw::SettingValue v{};
        v.type = bw::SettingType::Float;
        v.f = f;
        return v;
    }

    void add(bw::SettingKey key, bw::SettingType type, bw::SettingValue min, bw::SettingValue max,
             bw::SettingValue def, const char* name) {
        specs_[n_] = bw::SettingSpec{key, type, min, max, def, name};
        values_[n_] = def;
        ++n_;
    }

    bw::Result<bw::SettingValue> get(bw::SettingKey key) const override {
        for (size_t i = 0; i < n_; ++i) {
            if (specs_[i].key == key) {
                return {bw::Err::Ok, values_[i]};
            }
        }
        return {bw::Err::NotFound, bw::SettingValue{}};
    }

    bw::Result<void> set(bw::SettingKey key, const bw::SettingValue& value) override {
        for (size_t i = 0; i < n_; ++i) {
            if (specs_[i].key != key) {
                continue;
            }
            if (value.type != specs_[i].type) {
                return {bw::Err::InvalidArg};  // type mismatch, never coerced
            }
            values_[i] = clamp(specs_[i], value);  // clamp, not reject
            ++set_calls_;
            last_key_ = key;
            return {bw::Err::Ok};
        }
        return {bw::Err::NotFound};
    }

    size_t list(bw::SettingSpec* out, size_t max) const override {
        const size_t returned = n_ < max ? n_ : max;
        for (size_t i = 0; i < returned; ++i) {
            out[i] = specs_[i];
        }
        return n_;  // TOTAL, even when truncated (snprintf convention)
    }

    size_t count() const { return n_; }
    uint32_t setCalls() const { return set_calls_; }
    bw::SettingKey lastKey() const { return last_key_; }
    bw::SettingValue valueAt(size_t i) const { return values_[i]; }

private:
    static bw::SettingValue clamp(const bw::SettingSpec& spec, const bw::SettingValue& v) {
        bw::SettingValue out = v;
        switch (v.type) {
        case bw::SettingType::Int:
            if (v.i < spec.min.i) {
                out.i = spec.min.i;
            }
            if (v.i > spec.max.i) {
                out.i = spec.max.i;
            }
            break;
        case bw::SettingType::Float:
            if (v.f < spec.min.f) {
                out.f = spec.min.f;
            }
            if (v.f > spec.max.f) {
                out.f = spec.max.f;
            }
            break;
        case bw::SettingType::Bool:
            break;  // bools are inherently in range
        }
        return out;
    }

    bw::SettingSpec specs_[bw::ble::kMaxConfigSettings] = {};
    bw::SettingValue values_[bw::ble::kMaxConfigSettings] = {};
    size_t n_ = 0;
    uint32_t set_calls_ = 0;
    bw::SettingKey last_key_ = static_cast<bw::SettingKey>(0);
};

// Scriptable IEventStore for the EventNotifier failure paths (spec 9:
// "store error -> skip silently", "unsubscribe stops reads" - the read
// counter proves no read happens after unsubscribe).
template <size_t Capacity = 16> class FakeEventStore final : public bw::IEventStore {
public:
    bw::Result<uint32_t> append(const bw::Event& event) override {
        if (count_ >= Capacity) {
            return {bw::Err::Full, 0};
        }
        bw::Event stored = event;
        stored.seq = next_seq_++;
        events_[count_++] = stored;
        return {bw::Err::Ok, stored.seq};
    }

    bw::Result<size_t> readFrom(uint32_t from_seq, bw::Event* out, size_t max) override {
        ++reads_;
        if (fail_reads_) {
            return {bw::Err::BusError, 0};  // scripted store trouble
        }
        size_t written = 0;
        for (size_t i = 0; i < count_ && written < max; ++i) {
            if (events_[i].seq >= from_seq) {
                out[written++] = events_[i];
            }
        }
        return {bw::Err::Ok, written};
    }

    bw::Result<void> setLabel(uint32_t, bw::UserLabel) override { return {bw::Err::Ok}; }
    bw::Result<void> markSyncedThrough(uint32_t) override { return {bw::Err::Ok}; }
    uint32_t lastSyncedSeq() const override { return 0; }

    void failReads(bool fail) { fail_reads_ = fail; }
    uint32_t reads() const { return reads_; }

private:
    bw::Event events_[Capacity] = {};
    size_t count_ = 0;
    uint32_t next_seq_ = 1;
    bool fail_reads_ = false;
    uint32_t reads_ = 0;
};

// ISyncLink fake - the host stand-in for the device ConnectionMonitor the
// sync transport observes (M4 bench bug 1, spec D12 / backlog 36): tests
// script the link exactly as the monitor would report it (usable flag +
// the value maxSendBytes() passes through; 0 while not usable, mirroring
// ConnectionMonitor's "no link = nothing usable" rule).
class FakeSyncLink final : public bw::ble::ISyncLink {
public:
    bool linkUsable() const override { return usable_; }
    uint16_t maxSendBytes() const override { return usable_ ? max_send_bytes_ : 0; }

    void setLink(bool usable, uint16_t max_send_bytes) {
        usable_ = usable;
        max_send_bytes_ = max_send_bytes;
    }

private:
    bool usable_ = false;
    uint16_t max_send_bytes_ = 0;
};

// A ready-made Event for notifier tests (all non-essential fields default).
inline bw::Event makeEvent(uint32_t seq, bw::EventType type, bw::TimeMs t_ms) {
    bw::Event e{};
    e.t_start_ms = t_ms;
    e.seq = seq;  // overwritten by the fake stores on append
    e.boot_id = 7;
    e.type = type;
    e.schema_version = bw::Event::kSchemaVersion;
    return e;
}

}  // namespace bw_ble_test
