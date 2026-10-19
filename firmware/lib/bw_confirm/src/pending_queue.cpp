#include <bw/confirm/pending_queue.h>

namespace bw::confirm {

void PendingEventQueue::clear() {
    head_ = 0;
    count_ = 0;
}

bool PendingEventQueue::push(const PendingEntry& entry, PendingEntry& evicted) {
    if (full()) {
        // Drop-oldest (spec §9): free the head slot, advance, then write the
        // new entry at the tail — the freed slot IS the new tail slot in a
        // ring whose count stays at capacity.
        evicted = entries_[head_];
        head_ = static_cast<uint8_t>((head_ + 1) % kCapacity);
        entries_[physical(count_ - 1)] = entry;
        return true;
    }
    entries_[physical(count_)] = entry;
    ++count_;
    return false;
}

bool PendingEventQueue::pushFront(const PendingEntry& entry, PendingEntry& evicted) {
    if (full()) {
        // Oldest slot doubles as the new front: the dropped entry IS the one
        // that occupied it, so no other entry moves.
        evicted = entries_[head_];
        entries_[head_] = entry;
        return true;
    }
    head_ = static_cast<uint8_t>((head_ + kCapacity - 1) % kCapacity);
    entries_[head_] = entry;
    ++count_;
    return false;
}

bool PendingEventQueue::pop(PendingEntry& out) {
    if (empty()) {
        return false;
    }
    out = entries_[head_];
    head_ = static_cast<uint8_t>(physical(1));
    --count_;
    return true;
}

const PendingEntry* PendingEventQueue::front() const {
    return empty() ? nullptr : &entries_[head_];
}

bool PendingEventQueue::removeBySeq(uint32_t seq, PendingEntry& out) {
    for (size_t i = 0; i < count_; ++i) {
        if (entries_[physical(i)].seq != seq) {
            continue;
        }
        out = entries_[physical(i)];
        // Shift the newer entries one logical step toward the head so ring
        // order survives the removal (capacity 4 — a 4-iteration loop).
        for (size_t j = i; j + 1 < count_; ++j) {
            entries_[physical(j)] = entries_[physical(j + 1)];
        }
        --count_;
        return true;
    }
    return false;
}

}  // namespace bw::confirm
