#pragma once

// bw/result.h — docs/02_INTERFACES.md §2: Err, Result<T>, Result<void>.
// No exceptions anywhere (00 §7.2); every fallible call returns one of these.

#include <type_traits>

#include <bw/types.h>

namespace bw {

// Error codes are append-only: never renumber, reorder or remove (02 §6.6).
// Numeric values are pinned by test_result.cpp so a change fails the build.
//
// Code meanings — CR-004 (accepted 2026-09-30) pins this partition:
//  Capacity family (these two are deliberately distinct):
//   Full     — the destination is at capacity and the operation is REFUSED: nothing
//              is written and the input is left untouched. e.g. SpscQueue::push on a
//              full queue, IEventStore::append when the append-only log is full (F18).
//   Overflow — an input stream OVERRAN its buffer: data was dropped or overwritten
//              because it arrived faster than it was drained. e.g. MAX30102 FIFO
//              overrun flag (F01), RingBuffer pushed past capacity (oldest
//              overwritten), a chunk/sequence counter that exceeded its range.
//  Rule of thumb: if the caller can retry and succeed without anything being
//  consumed, it is Full; if something has already been lost, it is Overflow. The
//  caller-facing drop counter (F00 containers) always increments in the Overflow case.
enum class Err : uint8_t {
    Ok,
    Timeout,
    BusError,
    NotFound,
    InvalidArg,
    NotReady,
    Overflow,
    Corrupt,
    Unsupported,
    Busy,
    HwFault,
    Full,
};

// Result<T> — 02 §2 shape verbatim (F00 D5): a struct that always stores a T.
// `value` is undefined unless ok(); it may be a default object on the error path.
// Callers must check ok() first — that is 02 §1's validity-flag rule applied to the
// error path. Trivially copyable so it can travel through queues/mailboxes (02 §1).
template <class T> struct [[nodiscard]] Result {
    Err err;
    T value;

    constexpr bool ok() const { return err == Err::Ok; }

    // The container/queue contract (02 §1) forbids types that cannot be copied
    // blindly through a fixed-size buffer; Result is one of them.
    static_assert(std::is_trivially_copyable_v<T>,
                  "Result<T>: T must be trivially copyable (02 §1)");
    static_assert(std::is_default_constructible_v<T>,
                  "Result<T>: T is default-constructed into `value`; it must be so (F00 D5)");
};

// Result<void> — specialization for operations with no success value (02 §2).
// Holds only the error code; no dummy byte, no `value` to misread.
template <> struct [[nodiscard]] Result<void> {
    Err err;

    constexpr bool ok() const { return err == Err::Ok; }
};

}  // namespace bw
