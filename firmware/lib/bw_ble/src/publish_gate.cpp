// publish_gate.cpp - see publish_gate.h.
//
// Pure logic, host-testable: no platform includes, no heap, no blocking
// condition lives in exactly one compilation unit.
#include <bw/ble/publish_gate.h>

namespace bw::ble {

static_assert(kNotifyChannelCount == 4,
              "NotifyChannel gained entries: extend PublishGate::Intervals, "
              "intervalFor() and the cadence state arrays together");

void PublishGate::reset() {
    for (std::size_t i = 0; i < kNotifyChannelCount; ++i) {
        last_send_ms_[i] = 0;
        ever_sent_[i] = false;
    }
    connected_ = false;
    encrypted_ = false;
    insecure_drops_ = 0;
}

void PublishGate::setLink(bool connected, bool encrypted) {
    connected_ = connected;
    // An unencrypted link can never become "usable" here: connected_ && !encrypted_
    // is the single enforcement point for AUD-1 layer 2.
    encrypted_ = connected && encrypted;
}

TimeMs PublishGate::intervalFor(NotifyChannel channel) const {
    switch (channel) {
    case NotifyChannel::LiveStatus:
        return intervals_.status_ms;
    case NotifyChannel::HeartRate:
        return intervals_.hr_ms;
    case NotifyChannel::Event:
        return intervals_.event_ms;
    case NotifyChannel::Battery:
        return intervals_.battery_ms;
    }
    // Defensive: an out-of-range channel disables the cadence check rather
    // than blocking the link; the security check still applies because it
    // runs before this is reached.
    return 0;
}

TimeMs PublishGate::intervalMs(NotifyChannel channel) const {
    return intervalFor(channel);
}

PublishVerdict PublishGate::take(NotifyChannel channel, TimeMs now) {
    const std::size_t index = channelIndex(channel);
    const TimeMs interval = intervalFor(channel);
    // First send of a channel goes out immediately (ever_sent_ == false): a
    // fresh link must emit an initial status/HR/battery sample without
    // waiting a full interval.
    const bool due = !ever_sent_[index] || interval == 0 || now >= last_send_ms_[index] + interval;

    // Security second, after `due`: on a disconnected link there is no
    // traffic to suppress, so NoLink is never counted anywhere.
    if (!connected_) {
        return PublishVerdict::NoLink;
    }
    if (!encrypted_) {
        if (!due) {
            // Nothing would have been sent this cycle anyway - do not count a
            // drop for a sample that was never due (keeps
            // notifications_dropped == "due notifications suppressed by the
            // encryption policy", one per interval per channel at most).
            return PublishVerdict::NotDue;
        }
        // Due on an unencrypted link: suppress it, count it, and advance the
        // anchor exactly like a send so the count cannot race ahead of the
        // cadence. The caller returns WITHOUT touching the characteristic.
        ever_sent_[index] = true;
        last_send_ms_[index] = now;
        ++insecure_drops_;
        return PublishVerdict::DropInsecure;
    }

    if (!due) {
        return PublishVerdict::NotDue;
    }
    ever_sent_[index] = true;
    last_send_ms_[index] = now;
    return PublishVerdict::Send;
}

uint32_t PublishGate::insecureDrops() const {
    return insecure_drops_;
}

void PublishGate::resetCounters() {
    insecure_drops_ = 0;
}

}  // namespace bw::ble
