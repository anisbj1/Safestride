# 02 — INTERFACES · core (conventions, primitives, deferred types)

> Part of the shared contracts. **Locked: change only via change request.** Section numbers match the original `02` numbering; index in `docs/02_INTERFACES.md`.

## 1. Conventions
- Namespace `bw`. Types `PascalCase`, functions `camelCase`, constants `kPascalCase`, enums `enum class : uint8_t`.
- Units are in the name: `_ms`, `_hz`, `_g`, `_dps`, `_bpm`.
- All structs are trivially copyable (safe for queues/mailboxes). No `String`, no `std::vector` in contracts.
- Time: `using TimeMs = uint64_t;` monotonic ms since boot (`0` = boot). `using UnixSeconds = uint32_t;` (`0` = unknown).
- Probabilities and confidences are `float` in `[0,1]`. "Not available" is expressed by a **validity flag or `DataQuality::Insufficient`**, never by a magic number.
- Every value-type has `kSchemaVersion` where it can be persisted or transmitted.

## 2. Primitives (F00)

```cpp
enum class Err : uint8_t { Ok, Timeout, BusError, NotFound, InvalidArg, NotReady,
                           Overflow, Corrupt, Unsupported, Busy, HwFault, Full };
// Code meanings — F00 pins these values with a contract test; additions are append-only (§6.6).
//  Capacity family (these two are deliberately distinct):
//   Full     — the destination is at capacity and the operation is REFUSED: nothing is written
//              and the input is left untouched.  e.g. SpscQueue::push on a full queue,
//              IEventStore::append when the append-only log is full (F18).
//   Overflow — an input stream OVERRAN its buffer: data was dropped or overwritten because it
//              arrived faster than it was drained.  e.g. MAX30102 FIFO overrun flag (F01),
//              RingBuffer pushed past capacity (oldest overwritten), a chunk/sequence counter
//              that exceeded its allowed range.
//  Rule of thumb: if the caller can retry and succeed without anything being consumed, it is
//  Full; if something has already been lost, it is Overflow.  The caller-facing drop counter
//  (F00 containers) always increments in the Overflow case.
template <class T> struct Result { Err err; T value; bool ok() const; };  // Result<void> specialised
template <class T, size_t N> class RingBuffer;      // static storage, push/overwrite-oldest, indexed read
template <class T, size_t N> class SpscQueue;       // single producer/consumer, counts drops
template <class T> class LatestValue;               // mailbox: set(const T&) / get(T&) -> bool has_value, plus sequence number
struct IClock { virtual TimeMs nowMs() const = 0; virtual UnixSeconds unixSeconds() const = 0; };  // 0 if unknown
```

## 7. Types referenced above but intentionally left to F00 to define
`FontId` (small enum: Small, Medium, Large), `Contribution {uint8_t feature_index; float weighted_value;}`, `FaultCode` / `FaultSeverity` (F00 creates the enums with an initial set derived from F01/F23/F26 specs; afterwards append-only via CR), `TaskHealth`. F00 lists what it chose in its status log so reviewers can check it against these specs.
