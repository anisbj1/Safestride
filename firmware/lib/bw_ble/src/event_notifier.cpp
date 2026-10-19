#include <bw/ble/event_notifier.h>

namespace bw::ble {

EventNotifier::EventNotifier(IEventStore* store)
    : store_(store), subscribed_(false), baselined_(false), last_notified_seq_(0), store_errors_(0),
      skipped_intermediate_(0) {}

void EventNotifier::bind(IEventStore* store) {
    store_ = store;
    baselined_ = false;  // a new store starts a new cursor world
}

void EventNotifier::subscribe(TimeMs now_ms) {
    (void)now_ms;  // cadence is the caller's tick; nothing time-based here yet
    subscribed_ = true;
    baselined_ = false;  // re-baseline: a new subscriber gets no history burst
}

void EventNotifier::unsubscribe() {
    subscribed_ = false;
}

void EventNotifier::baseline() {
    if (store_ == nullptr) {
        return;  // stay un-baselined: an honest "nothing to send"
    }
    Event* buf = scan_buf_;  // static-storage member, never a stack array
    uint32_t cursor = 1;
    for (;;) {
        const Result<size_t> read = store_->readFrom(cursor, buf, kScanChunkEvents);
        if (!read.ok()) {
            ++store_errors_;  // skip silently (spec 9), but never pretend
            return;
        }
        if (read.value == 0) {
            break;  // cursor is past the tail
        }
        cursor = buf[read.value - 1].seq + 1;
        if (read.value < kScanChunkEvents) {
            break;  // a short read means the tail was reached
        }
    }
    last_notified_seq_ = cursor - 1;  // tail seq (0 = empty store)
    baselined_ = true;
}

bool EventNotifier::poll(TimeMs now_ms, Event& out) {
    (void)now_ms;  // notification cadence is paced by CommsTask, not by the
                   // notifier - this stays a pure pull function (spec D9)
    if (!subscribed_ || store_ == nullptr) {
        return false;
    }
    if (!baselined_) {
        baseline();
        if (!baselined_) {
            return false;  // store error during baseline: retry next tick
        }
    }

    const Result<size_t> read =
        store_->readFrom(last_notified_seq_ + 1, scan_buf_, kScanChunkEvents);
    if (!read.ok()) {
        ++store_errors_;
        return false;  // transient store trouble: skip silently (spec 9)
    }
    if (read.value == 0) {
        return false;  // nothing new
    }
    // Newest of the batch wins; the intermediates are counted, not notified
    // (spec D7 - completeness is the sync path's job).
    skipped_intermediate_ += static_cast<uint32_t>(read.value - 1);
    out = scan_buf_[read.value - 1];
    last_notified_seq_ = out.seq;
    return true;
}

}  // namespace bw::ble
