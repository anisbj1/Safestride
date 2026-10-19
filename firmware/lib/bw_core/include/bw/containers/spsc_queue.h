#pragma once

// bw/containers/spsc_queue.h — docs/02_INTERFACES.md §2:
// "single producer/consumer, counts drops".
//
// Textbook lock-free SPSC ring: the producer alone owns tail_, the consumer alone
// owns head_ (release/acquire pairing publishes the slot's contents). No critical
// section and no interrupt masking — ordered messages between two tasks (01 §3.1)
// such as ButtonEvent, Command, Event or raw sample batches.
//
// Overflow policy follows CR-004 (accepted):
//  - push() on a FULL queue is REFUSED: nothing is written, stored items are
//    untouched, push returns Err::Full, and drops() increments — the refused item is
//    then the caller's to drop, and the counter records that loss (F00 §9 requires it).
//  - pop() on an empty queue simply returns false.
//
// Index arithmetic: head_/tail_ are monotonic and only ever compared as a difference.
// Even after size_t wraps (2^32 pushes on Xtensa), tail - head stays correct unsigned
// arithmetic as long as fewer than 2^32 items are in flight — which holds by design.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <bw/result.h>

namespace bw {

template <class T, size_t N> class SpscQueue {
    static_assert(N > 0, "SpscQueue needs at least one slot");
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscQueue carries contract value types: T must be trivially "
                  "copyable (02 §1)");

public:
    // Producer side. Err::Full = refused, nothing written (CR-004).
    Err push(const T& v) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_.load(std::memory_order_acquire) >= N) {
            drops_.fetch_add(1, std::memory_order_relaxed);
            return Err::Full;
        }
        buf_[tail % N] = v;
        tail_.store(tail + 1, std::memory_order_release);
        return Err::Ok;
    }

    // Consumer side. false = queue empty, out untouched.
    bool pop(T& out) {
        const size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        out = buf_[head % N];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    size_t size() const {
        return tail_.load(std::memory_order_acquire) - head_.load(std::memory_order_acquire);
    }
    static constexpr size_t capacity() { return N; }
    bool empty() const { return size() == 0; }
    bool full() const { return size() == N; }

    // Refused pushes since construction — the drop counter F23 reads (F00 §6 D10).
    // Invariant (pinned by test_spsc_queue): drops() <= push attempts.
    uint32_t drops() const { return drops_.load(std::memory_order_relaxed); }

private:
    T buf_[N];
    std::atomic<size_t> head_{0};  // consumer-owned: index of the next element out
    std::atomic<size_t> tail_{0};  // producer-owned: index of the next free slot
    std::atomic<uint32_t> drops_{0};
};

}  // namespace bw
