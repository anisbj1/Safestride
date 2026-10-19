#include <bw/eventlog/subscribers.h>

namespace bw::eventlog {

bool Subscribers::add(bw::IEventSink* subscriber) {
    if (subscriber == nullptr || count_ >= kMaxEventSubscribers) {
        return false;
    }
    for (size_t i = 0; i < count_; ++i) {
        if (list_[i] == subscriber) {
            return false;  // duplicate: one attachment, one delivery
        }
    }
    list_[count_] = subscriber;
    ++count_;  // publish-after-store: readers see a complete entry (see header)
    return true;
}

size_t Subscribers::fanout(const bw::Event& event) const {
    size_t rejected = 0;
    for (size_t i = 0; i < count_; ++i) {
        const bw::Result<void> delivered = list_[i]->publish(event);
        if (!delivered.ok()) {
            ++rejected;  // §10: counted, not retried, others still served
        }
    }
    return rejected;
}

}  // namespace bw::eventlog
