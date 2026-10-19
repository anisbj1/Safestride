#pragma once

// bw/containers/ring_buffer.h — docs/02_INTERFACES.md §2:
// "static storage, push/overwrite-oldest, indexed read".
//
// Ownership: a RingBuffer is a VALUE type with no internal lock (01 §3.1: it is the
// per-task sliding window — one task owns it). Cross-task hand-off goes through
// SpscQueue or LatestValue instead.
//
// Overflow policy follows CR-004 (accepted): pushing a full buffer overwrites the
// oldest element, which is data ALREADY LOST, so push() returns Err::Overflow and the
// overwrites() counter increments — the drop counter F23 reads (F00 §6 D10).

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include <bw/result.h>

namespace bw {

template <class T, size_t N> class RingBuffer {
    static_assert(N > 0, "RingBuffer needs at least one slot");
    static_assert(std::is_trivially_copyable_v<T>,
                  "RingBuffer stores contract value types: T must be trivially "
                  "copyable (02 §1)");

public:
    // Appends v. When the buffer is full the OLDEST element is overwritten and
    // Err::Overflow is returned (nothing was refused: the new value is always stored).
    Err push(const T& v) {
        Err outcome = Err::Ok;
        if (size_ == N) {
            head_ = nextIndex(head_);
            --size_;
            ++overwrites_;
            outcome = Err::Overflow;
        }
        buf_[nextWriteIndex()] = v;
        ++size_;
        return outcome;
    }

    // Indexed read: i == 0 is the OLDEST element, i == size()-1 the newest.
    // Returns nullptr when i is out of range — never indexes out of bounds (§10).
    const T* at(size_t i) const {
        if (i >= size_) {
            return nullptr;
        }
        return &buf_[(head_ + i) % N];
    }

    size_t size() const { return size_; }
    static constexpr size_t capacity() { return N; }
    bool empty() const { return size_ == 0; }
    bool full() const { return size_ == N; }

    // Empties the buffer. Lifetime counters are statistics, not data: clear() keeps
    // them (F23 wants cumulative drops for the boot, not per-test-iteration drops).
    void clear() {
        head_ = 0;
        size_ = 0;
    }

    // Total elements overwritten by push() since construction (CR-004 drop counter).
    uint32_t overwrites() const { return overwrites_; }

private:
    size_t nextIndex(size_t i) const { return (i + 1) % N; }
    size_t nextWriteIndex() const { return (head_ + size_) % N; }

    T buf_[N];
    size_t head_ = 0;  // index of the oldest element
    size_t size_ = 0;
    uint32_t overwrites_ = 0;
};

}  // namespace bw
