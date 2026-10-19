#include "wire_sync.h"

#include <bw/sync/sync_session.h>

namespace f20 {

namespace {

// Static-storage instance (no heap: constructed before any task runs, embedded
// conventions). Named constants live in bw::sync's config (F20 §8 / D6).
bw::sync::SyncSession s_session{};

}  // namespace

void wire(bw::ISyncTransport* transport, bw::IEventStore* store) {
    s_session.bind(transport, store);
}

void pumpOnce(bw::TimeMs now) {
    (void)s_session.step(now);
}

bw::ISyncStatusProvider& statusProvider() {
    return s_session;
}

}  // namespace f20
