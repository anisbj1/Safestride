# CR-004 — Split `Err::Overflow` from `Err::Full` with a usage table

- Raised by: F00 / 2026-09-30
- Type: **clarification**
- Problem: `02_INTERFACES.md` §2 lists twelve codes but documents none of them:

  ```cpp
  enum class Err : uint8_t { Ok, Timeout, BusError, NotFound, InvalidArg, NotReady,
                             Overflow, Corrupt, Unsupported, Busy, HwFault, Full };
  ```

  `Overflow` and `Full` both plausibly mean "no space", and §2 gives no rule for choosing between them. That is a real hazard here rather than a cosmetic one: `Err` is the return path of *every* fallible operation in the project (`01` §7), several features will fail for capacity reasons at the same milestone, and consumers will `switch` on the value. If F17 appends an event to a full log and returns `Full` while F18's reader checks `Overflow` — or F19's FIFO overrun and F18's full log report the same code — parallel features built against the same contract will disagree silently, and nothing in a test will catch it, because both codes are "correct" under the current text.

  F00 cannot resolve this unilaterally: F00 §5 forbids inventing contract semantics, and choosing one mapping is exactly that.

- Proposal: exact diff to `docs/02_INTERFACES.md` §2 — **no value added, removed or reordered** (the enum stays append-only and byte-compatible); a documented partition only.

  ```diff
    enum class Err : uint8_t { Ok, Timeout, BusError, NotFound, InvalidArg, NotReady,
                               Overflow, Corrupt, Unsupported, Busy, HwFault, Full };
  + // Code meanings — F00 pins these values with a contract test; additions are append-only (§6.6).
  + //  Capacity family (these two are deliberately distinct):
  + //   Full     — the destination is at capacity and the operation is REFUSED: nothing is written
  + //              and the input is left untouched.  e.g. SpscQueue::push on a full queue,
  + //              IEventStore::append when the append-only log is full (F18).
  + //   Overflow — an input stream OVERRAN its buffer: data was dropped or overwritten because it
  + //              arrived faster than it was drained.  e.g. MAX30102 FIFO overrun flag (F01),
  + //              RingBuffer pushed past capacity (oldest overwritten), a chunk/sequence counter
  + //              that exceeded its allowed range.
  + //  Rule of thumb: if the caller can retry and succeed without anything being consumed, it is
  + //  Full; if something has already been lost, it is Overflow.  The caller-facing drop counter
  + //  (F00 containers) always increments in the Overflow case.
  ```

- Alternatives considered (simplest vs scalable) and why rejected:
  - **Simplest: do nothing — each feature picks whichever of the two reads better.** Rejected: this is precisely the class of silent cross-feature disagreement the contract layer exists to prevent, and it would surface only as an integration bug between F17/F18/F19 at M3/M4, long after `bw_core` is locked.
  - **Simplest fix variant: delete one code (`Overflow`) and keep only `Full`.** Rejected: removing an enumerator is a **breaking** change to an append-only enum (`02` §6.6) and would silently renumber the values F00 is about to pin in a contract test.
  - **Scalable: replace `Err` with a richer error object carrying a subsystem id, a retryable flag and a message id.** Rejected: `02` §1 and `01` §7 require errors as plain `Err` codes with no exceptions and no allocation, richer errors would grow every `Result<T>` in a memory-budgeted design, and the twelve codes already cover every failure mode the specs name. Over-engineering for a 520 KB MCU.
  - **Chosen: document the partition, keep all twelve values.** Zero code impact, no renumbering, and the rule ("caller can retry with nothing consumed → `Full`; something already lost → `Overflow`") is checkable in a unit test, so F00 can pin it in `test_result.cpp`.

- Affected features: F00 (writes the contract test) · F17/F18 (append vs read of a capacity-limited log) · F19 (BLE FIFO/MTU overruns) · F20 (sync chunks) · F01 (sensor FIFO overrun) · F02/F23 (window overflow).

- Migration / compatibility notes: **None.** The enumerators and their numeric order are unchanged, so `static_assert` pinning of `Err`'s values is unaffected and no serialized form exists yet. Only documentation is added.

- Status: **accepted** — 2026-09-30, by the developer/integrator.
- Application: **applied by the integrator 2026-10-01** to `docs/interfaces/core.md` §2 (split of `02` by CR-005). No enum value changes either way, so F00 can pin `Err`'s values immediately.
