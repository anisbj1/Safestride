#include <bw/ble/link_state_tracker.h>

namespace bw::ble {

void LinkStateTracker::apply(const LinkEvent& ev) {
    switch (ev.kind) {
    case LinkEvent::Kind::Up:
        connected_ = true;
        encrypted_ = false;             // a fresh link starts unencrypted (D1)
        sync_data_subscribed_ = false;  // CCCD state never survives a link
        mtu_ = ev.mtu != 0 ? ev.mtu : kDefaultAttMtu;
        break;
    case LinkEvent::Kind::Down:
        // Per-link state never leaks into the next link (spec 9): mtu
        // falls back to the ATT default, encryption and subscriptions
        // are gone until the peer re-establishes them.
        connected_ = false;
        encrypted_ = false;
        sync_data_subscribed_ = false;
        mtu_ = kDefaultAttMtu;
        break;
    case LinkEvent::Kind::MtuChanged:
        mtu_ = ev.mtu != 0 ? ev.mtu : kDefaultAttMtu;
        break;
    case LinkEvent::Kind::EncryptedChanged:
        encrypted_ = ev.flag;
        break;
    case LinkEvent::Kind::SyncDataSubscribedChanged:
        sync_data_subscribed_ = ev.flag;
        break;
    }

    // CR-018 snapshot produced at the EVENT's time: IBleLinkProvider::latest
    // has no time parameter, so t_ms records when the state was observed.
    state_.t_ms = ev.t_ms;
    state_.connected = connected_;
    state_.encrypted = encrypted_;
    state_.mtu = mtu_;  // kDefaultAttMtu (23) before negotiation (CR-018)
    has_state_ = true;
}

bool LinkStateTracker::snapshot(BleLinkState& out) const {
    if (!has_state_) {
        return false;  // "no snapshot yet" is an ordinary state (core.md 1)
    }
    out = state_;
    return true;
}

bool LinkStateTracker::flags(LinkFlags& out) const {
    if (!has_state_) {
        return false;
    }
    out.connected = connected_;
    out.encrypted = encrypted_;
    out.sync_data_subscribed = sync_data_subscribed_;
    out.mtu = mtu_;
    return true;
}

bool LinkStateTracker::linkUp() const {
    // CR-019: connected + encrypted per bonding policy + peer enabled Sync
    // Data notifications; false means "abort any transfer immediately".
    return connected_ && encrypted_ && sync_data_subscribed_;
}

uint16_t LinkStateTracker::maxSendBytes() const {
    if (!connected_ || mtu_ <= kAttHeaderBytes) {
        return 0;  // no link / nothing usable (ISyncTransport contract)
    }
    uint16_t usable = static_cast<uint16_t>(mtu_ - kAttHeaderBytes);
    if (usable > kMaxSyncFrameBytes) {
        usable = kMaxSyncFrameBytes;  // 255 MTU - 3 is the stack ceiling anyway
    }
    return usable;
}

}  // namespace bw::ble
