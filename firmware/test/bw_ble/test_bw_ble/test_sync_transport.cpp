// bw_ble - SyncTransport gate tests (spec 15 / CR-019; D12 host shape).
//
// The M4 bench bugs' host contract (backlog 36):
//  - a link source bound via bindMonitor() drives linkUp() up AND down
//    (bench bug 1: with the monitor unbound, linkUp() was false forever);
//  - send() attempts the radio hand-off exactly when the link is usable
//    and is refused with the documented Err::NotReady when it is not;
//  - Err::Busy from the hand-off propagates so F20 can retry (CR-019);
//  - the host base never fabricates a success (no radio exists here).
// NOT covered here: the device hand-off itself (NimBLE indication vs
// notification) - that is radio glue, bench checklist + INTEGRATION_REPORT

#include <cstring>

#include <bw/ble/sync_link.h>
#include <bw/ble/sync_transport.h>
#include <unity.h>

#include "ble_test_fakes.h"

using bw::Err;
using bw::Result;
using bw::SyncChannel;
using bw_ble_test::FakeSyncLink;

namespace {

// The host stand-in for the radio hand-off (D12): records every send
// attempt and scripts the stack's answer, so the gate and error map of
// send() are observable without a radio.
class RecordingTransport final : public bw::ble::SyncTransport {
public:
    Result<void> deliver(SyncChannel ch, const uint8_t* data, uint16_t len) override {
        ++attempts;
        last_channel = ch;
        last_len = len;
        if (len <= sizeof(last_bytes)) {
            std::memcpy(last_bytes, data, len);
        }
        return {scripted};
    }

    int attempts = 0;
    SyncChannel last_channel = SyncChannel::Control;
    uint16_t last_len = 0;
    uint8_t last_bytes[bw::kMaxSyncFrameBytes] = {};
    Err scripted = Err::Ok;
};

const uint8_t kFrame[4] = {0xA1, 0xB2, 0xC3, 0xD4};

}  // namespace

// First call / empty state: no link source bound = honest "no link"
// (bench bug 1's failure mode, made observable).
void test_transport_no_link_source_is_honest_no_link(void) {
    bw::ble::SyncTransport transport;
    TEST_ASSERT_FALSE(transport.linkUp());
    TEST_ASSERT_EQUAL_UINT16(0, transport.maxSendBytes());

    Result<void> r = transport.send(SyncChannel::Control, kFrame, sizeof(kFrame));
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Err::NotReady), static_cast<uint8_t>(r.err));
}

// The bound link drives linkUp() in BOTH directions - up on connect/subscribe,
// back down on drop, and up again on the next link (six edges on the bench).
void test_transport_link_up_follows_monitor_up_and_down(void) {
    FakeSyncLink link;
    bw::ble::SyncTransport transport;
    transport.bindMonitor(&link);

    link.setLink(false, 0);
    TEST_ASSERT_FALSE(transport.linkUp());

    link.setLink(true, 182);  // connected + encrypted + subscribed
    TEST_ASSERT_TRUE(transport.linkUp());

    link.setLink(false, 0);  // peer dropped
    TEST_ASSERT_FALSE(transport.linkUp());

    link.setLink(true, 182);  // next link
    TEST_ASSERT_TRUE(transport.linkUp());
}

void test_transport_max_send_bytes_follows_monitor(void) {
    FakeSyncLink link;
    bw::ble::SyncTransport transport;
    transport.bindMonitor(&link);

    link.setLink(false, 182);
    TEST_ASSERT_EQUAL_UINT16(0, transport.maxSendBytes());  // 0 without a link

    link.setLink(true, 182);  // peer negotiated ATT MTU 185
    TEST_ASSERT_EQUAL_UINT16(182, transport.maxSendBytes());

    link.setLink(true, 20);  // peer stayed at the 23-byte default (M4 bench)
    TEST_ASSERT_EQUAL_UINT16(20, transport.maxSendBytes());
}

// CR-019 send(): while the link is usable every channel reaches the radio
// hand-off with its bytes intact.
void test_transport_send_attempts_when_link_usable(void) {
    FakeSyncLink link;
    RecordingTransport transport;
    transport.bindMonitor(&link);
    link.setLink(true, 182);

    Result<void> r = transport.send(SyncChannel::Data, kFrame, sizeof(kFrame));
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_INT(1, transport.attempts);
    TEST_ASSERT_TRUE(transport.last_channel == SyncChannel::Data);
    TEST_ASSERT_EQUAL_UINT16(sizeof(kFrame), transport.last_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kFrame, transport.last_bytes, sizeof(kFrame));

    r = transport.send(SyncChannel::Control, kFrame, sizeof(kFrame));
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_INT(2, transport.attempts);
    TEST_ASSERT_TRUE(transport.last_channel == SyncChannel::Control);
}

// CR-019 error map: link down -> Err::NotReady, nothing reaches the radio
// (nothing is queued across a down link, spec 10); the same frame retried
// once the link is usable is accepted.
void test_transport_send_refused_notready_while_link_down(void) {
    FakeSyncLink link;
    RecordingTransport transport;
    transport.bindMonitor(&link);

    link.setLink(false, 182);
    Result<void> r = transport.send(SyncChannel::Control, kFrame, sizeof(kFrame));
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Err::NotReady), static_cast<uint8_t>(r.err));
    TEST_ASSERT_EQUAL_INT(0, transport.attempts);  // refused BEFORE the hand-off

    link.setLink(true, 182);
    r = transport.send(SyncChannel::Control, kFrame, sizeof(kFrame));
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_INT(1, transport.attempts);
}

// Caller-bug bounds are refused before the gate; the documented maximum
// (kMaxSyncFrameBytes = 252) is accepted.
void test_transport_send_argument_bounds(void) {
    FakeSyncLink link;
    RecordingTransport transport;
    transport.bindMonitor(&link);
    link.setLink(true, 182);

    uint8_t frame[bw::kMaxSyncFrameBytes] = {};

    Result<void> r = transport.send(SyncChannel::Control, nullptr, 4);
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Err::InvalidArg), static_cast<uint8_t>(r.err));

    r = transport.send(SyncChannel::Control, frame, 0);
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Err::InvalidArg), static_cast<uint8_t>(r.err));

    r = transport.send(SyncChannel::Control, frame, bw::kMaxSyncFrameBytes + 1);
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Err::InvalidArg), static_cast<uint8_t>(r.err));
    TEST_ASSERT_EQUAL_INT(0, transport.attempts);  // caller bugs never touch the radio

    r = transport.send(SyncChannel::Data, frame, bw::kMaxSyncFrameBytes);
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_INT(1, transport.attempts);
}

// CR-019 Busy/backpressure: a stack refusal (previous indication
// unconfirmed) surfaces as Err::Busy with nothing consumed, and the next
// pump's retry succeeds - the contract bench bug 2 re-broke and the fix
// restored.
void test_transport_busy_from_stack_propagates(void) {
    FakeSyncLink link;
    RecordingTransport transport;
    transport.bindMonitor(&link);
    link.setLink(true, 182);

    transport.scripted = Err::Busy;  // previous indication still unconfirmed
    Result<void> r = transport.send(SyncChannel::Control, kFrame, sizeof(kFrame));
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Err::Busy), static_cast<uint8_t>(r.err));

    transport.scripted = Err::Ok;  // later pump: retry wins, frame delivered
    r = transport.send(SyncChannel::Control, kFrame, sizeof(kFrame));
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_INT(2, transport.attempts);
}

// The host base has no radio: an un-overridden deliver() must say so
// honestly, never return a fabricated success (00 "no fabricated
// confidence"; the suite overrides deliver() everywhere else).
void test_transport_host_base_delivers_nothing_silently(void) {
    FakeSyncLink link;
    bw::ble::SyncTransport transport;
    transport.bindMonitor(&link);
    link.setLink(true, 182);

    Result<void> r = transport.send(SyncChannel::Control, kFrame, sizeof(kFrame));
    TEST_ASSERT_FALSE(r.ok());
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(Err::Unsupported), static_cast<uint8_t>(r.err));
}

// receive() glue: the inbound queue behind CR-019's pull model - empty =
// Ok(0) (never an error), then the oldest frame byte-for-byte.
void test_transport_receive_delegates_inbound_queue(void) {
    bw::ble::SyncTransport transport;
    SyncChannel ch = SyncChannel::Data;
    uint8_t out[8] = {};

    Result<size_t> r = transport.receive(&ch, out, sizeof(out));
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(r.value));  // nothing pending

    const uint8_t frame[2] = {7, 8};
    TEST_ASSERT_TRUE(transport.inbound().push(SyncChannel::Control, frame, sizeof(frame)) ==
                     Err::Ok);
    r = transport.receive(&ch, out, sizeof(out));
    TEST_ASSERT_TRUE(r.ok());
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(r.value));
    TEST_ASSERT_TRUE(ch == SyncChannel::Control);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, out, sizeof(frame));
}
