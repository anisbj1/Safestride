#pragma once

// bw/containers/latest_value.h — docs/02_INTERFACES.md §2:
// "mailbox: set(const T&) / get(T&) -> bool has_value, plus sequence number".
//
// Semantics (F00 §6 D10):
//  - Readers want the NEWEST value; intermediate values are dropped on purpose.
//  - sequence() increments on every set() so a reader can cheaply detect "new data".
//  - overwrites() counts set()s that replaced a value NOBODY HAD READ YET — i.e. real
//    data loss ("arrived faster than drained", CR-004 Overflow sense). A set() after a
//    get() lost nothing and is not counted.
//
// Thread safety: set()/get() copy a multi-field struct as one unit, so they hold the
// platform critical section (bw/platform_sync.h) — a reader never observes a
// half-written value. Single owner per instance is still the house rule; the guard
// only makes the copy tear-free.

#include <type_traits>

#include <bw/platform_sync.h>
#include <bw/types.h>

namespace bw {

template <class T> class LatestValue {
    static_assert(std::is_trivially_copyable_v<T>,
                  "LatestValue stores contract value types: T must be trivially "
                  "copyable (02 §1)");

public:
    // Publishes the newest value. Replaces any unread predecessor (counted).
    void set(const T& v) {
        CriticalSection guard;
        if (has_value_ && unread_) {
            ++overwrites_;
        }
        value_ = v;
        has_value_ = true;
        unread_ = true;
        ++sequence_;
    }

    // Copies the newest value into out. Returns false when nothing was ever set —
    // "no data yet" is an ordinary state, never a magic value (02 §1).
    // Repeated get()s of the same value are allowed; only set() marks unread again.
    bool get(T& out) const {
        CriticalSection guard;
        if (!has_value_) {
            return false;
        }
        out = value_;
        unread_ = false;
        return true;
    }

    bool hasValue() const {
        CriticalSection guard;
        return has_value_;
    }

    // Increments once per set(); wraps at 2^32 (callers compare by difference, which
    // stays correct across the wrap as long as fewer than 2^32 sets happen unread).
    uint32_t sequence() const {
        CriticalSection guard;
        return sequence_;
    }

    // set() calls that replaced a value nobody had read (F23-facing counter, D10).
    uint32_t overwrites() const {
        CriticalSection guard;
        return overwrites_;
    }

private:
    mutable T value_{};  // read by get() → mutable; never read before has_value_
    mutable bool has_value_ = false;
    mutable bool unread_ = false;  // set since the last get() (mutation via get)
    uint32_t sequence_ = 0;
    uint32_t overwrites_ = 0;
};

}  // namespace bw
