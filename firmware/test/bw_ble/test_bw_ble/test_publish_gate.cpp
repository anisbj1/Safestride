// test_publish_gate.cpp - audit AUD-1 layer 2 + AUD-6: the ONE gate every
// outgoing BLE notification passes through.
//
// Revert proof (H10): removing the encryption condition in
// PublishGate::take() fails test_publish_gate_unencrypted_link_emits_nothing
// and test_publish_gate_drop_is_counted_not_queued; bypassing the per-channel
// interval fails test_publish_gate_hr_is_capped_at_1hz. Each test asserts the
// VERDICT (what the caller is allowed to do), so the security property
// "unencrypted link never notifies" and the cadence property "HR > 1 Hz is
// impossible" are checked where they are enforced, not re-implemented.

#include <bw/ble/ble_config.h>
#include <bw/ble/publish_gate.h>

#include <cstddef>

#include <unity.h>

using bw::TimeMs;
using bw::ble::kHrNotifyIntervalMs;
using bw::ble::NotifyChannel;
using bw::ble::PublishGate;
using bw::ble::PublishVerdict;

namespace {

// [DERIVED from ble_config.h] the production cadence set BleServer::bind()
// installs: status 1 s, HR 1 Hz cap, event 1 tick, battery 60 s.
PublishGate::Intervals productionIntervals() {
    return PublishGate::Intervals{1000, kHrNotifyIntervalMs, 1000, 60000};
}

}  // namespace

void test_publish_gate_unencrypted_link_emits_nothing(void) {
    PublishGate gate;
    gate.setIntervals(productionIntervals());

    // Connected but NOT encrypted (bonded phone, encryption not up yet):
    // the first due publish must be suppressed, never Send.
    gate.setLink(/*connected=*/true, /*encrypted=*/false);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PublishVerdict::DropInsecure),
                          static_cast<int>(gate.take(NotifyChannel::LiveStatus, 1000)));

    // Still inside the interval: nothing due, so nothing is counted twice.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PublishVerdict::NotDue),
                          static_cast<int>(gate.take(NotifyChannel::LiveStatus, 1500)));
    TEST_ASSERT_EQUAL_UINT32(1, gate.insecureDrops());
}

void test_publish_gate_encrypted_link_delivers(void) {
    PublishGate gate;
    gate.setIntervals(productionIntervals());
    gate.setLink(/*connected=*/true, /*encrypted=*/true);

    // First due publish over an encrypted link goes out.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PublishVerdict::Send),
                          static_cast<int>(gate.take(NotifyChannel::LiveStatus, 1000)));
    TEST_ASSERT_EQUAL_UINT32(0, gate.insecureDrops());

    // Dropping encryption mid-link re-closes the gate (re-keying, D10).
    gate.setLink(true, false);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PublishVerdict::DropInsecure),
                          static_cast<int>(gate.take(NotifyChannel::LiveStatus, 5000)));
    TEST_ASSERT_EQUAL_UINT32(1, gate.insecureDrops());

    // Encrypted again: the gate reopens on the next due sample.
    gate.setLink(true, true);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PublishVerdict::Send),
                          static_cast<int>(gate.take(NotifyChannel::LiveStatus, 6000)));
}

void test_publish_gate_drop_is_counted_not_queued(void) {
    PublishGate gate;
    gate.setIntervals(productionIntervals());
    gate.setLink(true, /*encrypted=*/false);

    // One suppressed notification = one count, once per interval per channel;
    // the anchor advances like a send so an unencrypted link cannot make the
    // counter (or the caller) race ahead of the cadence.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PublishVerdict::DropInsecure),
                          static_cast<int>(gate.take(NotifyChannel::HeartRate, 10000)));
    TEST_ASSERT_EQUAL_UINT32(1, gate.insecureDrops());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PublishVerdict::NotDue),
                          static_cast<int>(gate.take(NotifyChannel::HeartRate, 10500)));
    TEST_ASSERT_EQUAL_UINT32(1, gate.insecureDrops());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PublishVerdict::DropInsecure),
                          static_cast<int>(gate.take(NotifyChannel::HeartRate, 11000)));
    TEST_ASSERT_EQUAL_UINT32(2, gate.insecureDrops());

    // The verdict is never Send on this link, whatever the cadence says.
    TEST_ASSERT_TRUE(gate.take(NotifyChannel::HeartRate, 11001) != PublishVerdict::Send);
    TEST_ASSERT_TRUE(gate.take(NotifyChannel::Battery, 0) != PublishVerdict::Send);
}

void test_publish_gate_no_link_never_counts(void) {
    PublishGate gate;
    gate.setIntervals(productionIntervals());
    gate.setLink(/*connected=*/false, /*encrypted=*/true);  // disconnected

    // NoLink on every channel: there is no traffic to suppress, so nothing is
    // counted (notifications_dropped stays a policy counter, not a link
    // counter) and no cadence anchor moves.
    for (std::size_t i = 0; i < bw::ble::kNotifyChannelCount; ++i) {
        const auto channel = static_cast<NotifyChannel>(i);
        TEST_ASSERT_EQUAL_INT(static_cast<int>(PublishVerdict::NoLink),
                              static_cast<int>(gate.take(channel, 5000)));
    }
    TEST_ASSERT_EQUAL_UINT32(0, gate.insecureDrops());

    // Because no anchor moved, the first sample after (re)connect still goes
    // out immediately instead of waiting an interval.
    gate.setLink(true, true);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PublishVerdict::Send),
                          static_cast<int>(gate.take(NotifyChannel::LiveStatus, 5001)));
}

void test_publish_gate_hr_is_capped_at_1hz(void) {
    PublishGate gate;
    // The pinned production cap itself: if ble_config.h's value drifts above
    // 1 Hz, this test fails instead of the device silently over-notifying.
    TEST_ASSERT_EQUAL_UINT32(1000, kHrNotifyIntervalMs);

    PublishGate::Intervals iv = productionIntervals();
    iv.hr_ms = kHrNotifyIntervalMs;
    gate.setIntervals(iv);
    gate.setLink(true, true);

    // Fast-tick simulation: CommsTask waking every 10 ms (phone-write early
    // wake-ups, audit AUD-6). Over 10 s at 100 Hz the gate may release at
    // most ONE sample per second: 11 Sends (t = 0, 1000, ..., 10000).
    TimeMs sends = 0;
    for (TimeMs t = 0; t <= 10000; t += 10) {
        if (gate.take(NotifyChannel::HeartRate, t) == PublishVerdict::Send) {
            ++sends;
        }
    }
    TEST_ASSERT_EQUAL_UINT64(11, sends);
}

void test_publish_gate_cadence_per_channel_independent(void) {
    PublishGate gate;
    gate.setIntervals(productionIntervals());
    gate.setLink(true, true);

    // Both channels emit their first sample immediately (fresh link).
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PublishVerdict::Send),
                          static_cast<int>(gate.take(NotifyChannel::LiveStatus, 0)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PublishVerdict::Send),
                          static_cast<int>(gate.take(NotifyChannel::Battery, 0)));

    // 2 s later: status due again, battery (60 s) still quiet - each channel
    // keeps its OWN anchor, one cadence never delays or frees another.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PublishVerdict::Send),
                          static_cast<int>(gate.take(NotifyChannel::LiveStatus, 2000)));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PublishVerdict::NotDue),
                          static_cast<int>(gate.take(NotifyChannel::Battery, 2000)));

    // Battery becomes due at its own interval.
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PublishVerdict::Send),
                          static_cast<int>(gate.take(NotifyChannel::Battery, 60000)));
}
