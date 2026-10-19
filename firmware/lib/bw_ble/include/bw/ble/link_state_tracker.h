#pragma once

// bw/ble/link_state_tracker.h - F19 spec 7 (LinkStateTracker row) + spec D10 /
// CR-018: pure (LinkEvent, TimeMs) -> BleLinkState transitions. The device
// ConnectionMonitor feeds it from NimBLE callbacks; the host suite drives it
// directly, so every transition rule is testable without a radio (spec 9).
//
// This tracker also derives the two values ISyncTransport needs (CR-019):
//   linkUp()        = connected + encrypted + peer enabled Sync Data notify
//   maxSendBytes()  = negotiated ATT MTU - kAttHeaderBytes, clamped to
//                     [0, kMaxSyncFrameBytes]

#include <cstdint>

#include <bw/ble/ble_config.h>
#include <bw/containers/latest_value.h>
#include <bw/providers.h>
#include <bw/sync_transport.h>  // kMaxSyncFrameBytes: the CR-019 clamp maxSendBytes uses
#include <bw/types.h>

namespace bw::ble {

// Internal F19 event (spec 15's LinkEvent, kept private to bw_ble after the
// CR-019 G1 ruling - the public seam is BleLinkState via IBleLinkProvider).
struct LinkEvent {
    enum class Kind : uint8_t {
        Up,                         // a GAP link came up (mtu = initial ATT MTU)
        Down,                       // the last GAP link dropped
        MtuChanged,                 // peer negotiated a new ATT MTU
        EncryptedChanged,           // link encryption/bonding state (flag)
        SyncDataSubscribedChanged,  // peer toggled Sync Data CCCD (flag)
    };
    Kind kind;
    uint16_t mtu;  // on Up (initial value) and MtuChanged; 0 otherwise
    bool flag;     // on EncryptedChanged / SyncDataSubscribedChanged
    TimeMs t_ms;
};

// Link flags the transport and monitor read - internal, not a bw_core type
// (CR-018 exposes the snapshot, not these).
struct LinkFlags {
    bool connected;
    bool encrypted;
    bool sync_data_subscribed;
    uint16_t mtu;  // ATT MTU, kDefaultAttMtu before negotiation
};

class LinkStateTracker {
public:
    // Applies one event and refreshes the CR-018 snapshot at the event's own
    // t_ms (IBleLinkProvider::latest() has no time parameter, so the value is
    // stamped when PRODUCED, providers.h CR-018).
    void apply(const LinkEvent& ev);

    // CR-018 snapshot (false = no link event ever seen, provider says "no
    // data yet" - a snapshot is produced only once the stack has reported
    // something, never fabricated). The mtu field is kDefaultAttMtu (23)
    // before any negotiation (CR-018).
    bool snapshot(BleLinkState& out) const;

    bool flags(LinkFlags& out) const;

    // CR-019 linkUp(): connected + encrypted + peer enabled Sync Data
    // notifications. False before the first Up event.
    bool linkUp() const;

    // CR-019 maxSendBytes(): ATT MTU - 3 B ATT header, clamped into
    // [0, kMaxSyncFrameBytes] (255 - 3 is the stack's own ceiling).
    // [MEASURED] The sync engine needs 52 usable bytes => ATT MTU >= 55
    // (CR-019); 51 B is only the Event-notify floor (audit AUD-5).
    uint16_t maxSendBytes() const;

private:
    bool has_state_ = false;
    bool connected_ = false;
    bool encrypted_ = false;
    bool sync_data_subscribed_ = false;
    uint16_t mtu_ = kDefaultAttMtu;
    BleLinkState state_{};  // last produced CR-018 snapshot (t_ms stamped on apply)
};

// CR-018 provider adapter: publishes the tracker's cached snapshot through
// bw::IBleLinkProvider over a LatestValue mailbox (thread-safe by contract,
// runtime 3.1), so F23/F12 consume bw_core types only and the host suite can
// pin the production semantics without any app wiring.
class LinkStateProvider final : public IBleLinkProvider {
public:
    explicit LinkStateProvider(const LatestValue<BleLinkState>* mailbox = nullptr)
        : mailbox_(mailbox) {}

    void bind(const LatestValue<BleLinkState>* mailbox) { mailbox_ = mailbox; }

    bool latest(BleLinkState& out) const override {
        return mailbox_ != nullptr && mailbox_->get(out);
    }

private:
    const LatestValue<BleLinkState>* mailbox_;
};

}  // namespace bw::ble
