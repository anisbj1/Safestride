// publish_gate.h — one gate every outgoing BLE notification passes through.
//
// AUD-1 (audit 2026-10-16): enforcement lived only in test scaffolding and in
// BleServer's private doc comments; this class is the literal condition inside
// every publish path (see BleServer::publish*). AUD-6 (same audit): the HR
// 1 Hz cap previously rode on CommsTask::tick() alone, which is not guaranteed
// to run at >= 1 Hz because BLE writes wake the task early; the per-channel
// cadence now lives here, where the sample is actually taken.
//
// bw_ble layer 1, pure logic: no FreeRTOS, no NimBLE, no I/O — host-testable
// (test_publish_gate.cpp). All fields are TimeMs (uint64_t) or bool: no heap,
// no allocation after startup, fixed size (bw-embedded-cpp-conventions).
#pragma once

#include <cstddef>
#include <cstdint>

#include <bw/types.h>

namespace bw::ble {

// Outgoing notification channels. Append-only: the entries are used as stable
// indices into per-channel cadence state, so existing values are never reused
// for another channel.
enum class NotifyChannel : uint8_t {
    LiveStatus = 0,
    HeartRate = 1,
    Event = 2,
    Battery = 3,
};

// Number of entries in NotifyChannel. Compile-time cross-check against the
// enum lives in publish_gate.cpp (static_assert); count/queries are constexpr.
inline constexpr std::size_t kNotifyChannelCount = 4;

constexpr std::size_t channelIndex(NotifyChannel channel) {
    return static_cast<std::size_t>(channel);
}

// Outcome of one gate evaluation. Append-only; DropInsecure was added by the
// audit FIX, never insert into the middle.
enum class PublishVerdict : uint8_t {
    Send = 0,          // due, link up and encrypted: caller emits the notify
    NoLink = 1,        // not connected: nothing counted, cadence not advanced
    NotDue = 2,        // connected but interval not elapsed: nothing counted,
                       // cadence not advanced (only when nothing was sent)
    DropInsecure = 3,  // connected but NOT encrypted: suppressed, counted in
                       // insecureDrops(); cadence anchor advances exactly like
                       // a send so unencrypted traffic cannot busy-loop
};

// Cadence + security policy for outgoing notifications.
//
// guard every notify; simplest option — a boolean at each call site; scalable
// option — one queried object; choice — one gate, so "unencrypted link never
// notifies" and "HR >= 1 Hz is impossible" are properties of the code path,
// not of the caller's discipline, and both are host-testable in isolation).
class PublishGate {
public:
    // Per-channel minimum send intervals, milliseconds. Zero disables the
    // cadence check for that channel (Event keeps "every event immediately" —
    // that path is protected by store dedup instead of a rate limit).
    struct Intervals {
        TimeMs status_ms = 0;
        TimeMs hr_ms = 0;
        TimeMs event_ms = 0;
        TimeMs battery_ms = 0;
    };

    // Reset all cadence anchors to "nothing sent yet" and drop the link state.
    void reset();

    // Link state; call once per BleServer::tick() from the connection monitor.
    // Unencrypted or disconnected links make every channel DropInsecure/NoLink.
    void setLink(bool connected, bool encrypted);

    // Evaluate one publish at sample time `now`. Returns Send only when the
    // link is connected AND encrypted and the channel interval has elapsed.
    // NoLink/NotDue leave all anchors untouched (a retry later in the same
    // interval still sends); Send/DropInsecure advance the channel's anchor to
    // `now`. DropInsecure is returned only when a notification was DUE and the
    // unencrypted link suppressed it (so insecureDrops() counts one per
    // interval per channel at most, never one per tick).
    PublishVerdict take(NotifyChannel channel, TimeMs now);

    // Count of DropInsecure verdicts since the last reset(); exposed so
    // BleServer::stats() can fold it into BleStats::notifications_dropped.
    // Returns the count and does not reset (stats() is called repeatedly; a
    // reset-on-read would make the counter non-monotonic across polls).
    uint32_t insecureDrops() const;

    // Evaluated interval for a channel, in milliseconds (Intervals field by
    // channel index); used by tests to assert the configured cadence.
    TimeMs intervalMs(NotifyChannel channel) const;

    // Configure the per-channel intervals once (BleServer::bind). Order:
    // {status_ms, hr_ms, event_ms, battery_ms}.
    void setIntervals(const Intervals& intervals) { intervals_ = intervals; }

    // Reset only the counters (cadence state and link state are untouched).
    // Used by tests; nothing in firmware calls it after startup.
    void resetCounters();

private:
    TimeMs intervalFor(NotifyChannel channel) const;

    Intervals intervals_{};
    TimeMs last_send_ms_[kNotifyChannelCount] = {};
    bool ever_sent_[kNotifyChannelCount] = {};
    bool connected_ = false;
    bool encrypted_ = false;
    uint32_t insecure_drops_ = 0;
};

}  // namespace bw::ble
