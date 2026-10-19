# F20 — Data synchronization

> Track **D** · Milestone **M4** · Runs on **firmware + companion** · Contract changes only via change request.

## 1. Responsibility
Reliable, idempotent transfer of unsynchronized events from wearable to phone with acknowledgment, tolerant of disconnects, partial transfers and duplicates. The wearable never depends on it for alerting.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `IEventStore.readFrom/lastSyncedSeq/markSyncedThrough`
- `ISyncTransport` (chunk send/receive; implemented by F19 wiring) — **exists in `bw_core` since CR-019 (accepted 2026-10-15, `bw/sync_transport.h`)**; the engine parses RAW control frames from `receive()` itself and diffs `linkUp()`/`maxSendBytes()` each pump (G1 ruling 1: F19 is a dumb byte pipe, no `SyncControlFrame`/`LinkEvent`)
- `TimeSync` events (consumed by the companion-side reconstruction rule, §6.8; the device half does not read them)

**Outputs (produced contracts)**
- Sync protocol implementation (device + companion)
- Sync status for UI/BLE (`SyncStatus` + `ISyncStatusProvider`, CR-019 part B)

## 4. Dependencies
- `bw_core` only.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Delete events (only mark synced).
- Block on the phone.
- Parse event semantics.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Protocol**
- Simplest: Send everything each time.
- Professional/scalable: Cursor + ack, chunked, CRC per chunk, idempotent on `(device_id, seq)`.
- Trade-offs: Needs sequence discipline (F17/F18).
- **Choice:** Cursor + ack (DEC-011).

**D2. Transport seam (`ISyncTransport`, CR-019)** — *problem:* `SyncSession` must not include F19 headers (layering rule 1) and must run on the host against a lossy fake; `bw_core` has no transport contract.
- Simplest: call F19's concrete BLE adapter directly. Rejected — violates "a feature includes only `bw_core` headers" and makes host tests depend on the BLE stack.
- Professional/scalable alt: push-style interface (F19 pushes inbound frames and link edges into an F20-owned sink). Event-driven and natural for NimBLE callbacks, but needs a *second* interface (sink), allows re-entrant calls from the BLE host task, and makes deterministic host tests harder.
- **Choice:** pull-style duplex byte pipe (CR-019, §15): `send(channel, bytes, len)`, `receive(ch_out, buf, cap)`, `linkUp()`, `maxSendBytes()`. `SyncSession::step(now)` drives everything; one interface, one caller (the sync pump), fully deterministic tests with a lossy fake. The F19 adapter serializes NimBLE-callback → queue internally (F19's concern, noted in the contract's call-context comment). `SyncChannel { Control, Data }` maps 1:1 onto the two GATT characteristics of `02` §6.2. Part B of the same CR adds `SyncStatus` + `ISyncStatusProvider` because §3 of this spec outputs "Sync status for UI/BLE" and a second CR later would be avoidable churn.

**D3. Chunk packing** — *problem:* fit whole `Event` records (48 B, CR-002) into MTU-sized notifications.
- Simplest: one event per chunk (55 B frame for 48 B data ≈ 13 % framing overhead, one ack per event).
- Scalable-but-heavy: byte-stream with cross-chunk fragmentation and reassembly (arbitrary sizes, but reassembly state, torn-chunk resume logic, many more failure modes).
- **Choice:** pack `payload / kEventRecordBytes` **whole** records per chunk; no record is ever split. Requires negotiated ATT MTU ≥ `kMinSyncMtuBytes` (55); at MTU 255 a chunk carries 5 events (240 of the 248 usable payload bytes, 8 B slack). Below-floor MTU ⇒ sync stays off with a status flag (no crashes, no partial behavior).

**D4. Ack / commit semantics** — *problem:* what makes `markSyncedThrough` safe against lost acks, reboots and a confused phone.
- Simplest: device marks each chunk as sent. Rejected — a notification is not durable delivery; the phone might never receive it.
- **Choice:** **cumulative ack only** (`ack_seq` = highest seq the companion has durably stored, written *after* its SQLite commit). The device calls `markSyncedThrough(ack_seq)` only when the ack is monotonic (`> lastSyncedSeq`) **and** exists in the store (`readFrom(ack, &e, 1)` returns exactly that seq — `IEventStore` has no head query, so this doubles as a head check). Anything else is ignored and counted. Lost ack ⇒ timeout ⇒ resend from the ack point ⇒ companion upserts idempotently ⇒ re-ack converges. Device reboot mid-transfer ⇒ marker unchanged ⇒ resend on next request (conservative-after-power-cut is F18's tested behavior). The marker is only an eviction hint for F18, never a deletion trigger for this feature (§5).

**D5. Raw-window sync (§13 open question, RESOLVED)** — *problem:* do raw sensor windows (≈36 KB/60 s) sync automatically or on request?
- Automatic: one path for everything, but windows would dominate the stream, starve the event cursor, blow past `kMaxUnackedChunks` flow control and the < 3 KB RAM budget, and couple sync to F18's window eviction.
- On request: event sync stays the always-on, bounded path; bulk pull is an explicit future op.
- **Choice: on request** `[DECIDED 2026-10-15]`. §12 stays as-is (separate characteristic/CR later); the control-frame type space reserves `0x02` for a future raw-window request.

**D7. Frame versioning and malformed input** — *problem:* keep a dumb phone or a corrupted write from wedging the device.
- Every frame starts with `kSyncProtocolVersion` (1) + a type byte; unknown version, unknown type or wrong length ⇒ frame dropped, counters incremented, no crash, no NACK (simplest: the companion's timeout/re-request loop is the recovery path). Malformed input is covered by `Err`-style defensive checks, never `abort()`.

**D8. Event wire encoding** — chunks carry the **byte image of `bw::Event` schema v1** (48 B, offsets pinned by `bw/contract_checks.h`, little-endian; both ends are LE: Xtensa/x86/phone-ARM). `SYNC_PROTOCOL.md` §3.4 is the normative field/offset table; the companion decodes by offset (Dart is endian-explicit), never by struct cast. Reconciliation at G1: F19's `Codec` (Event Notify) must produce the identical byte image.

**D10. Companion ack = contiguous committed prefix** — `SyncPort.ack(seq)` (F21's seam, signature unchanged) derives the wire value from `base`, not from the argument: send `ack_seq = base` only when `base > last_sent_ack`; the view-model's `seq` is the trigger, clamped by construction. This is the companion half of §4.7's "no ack sent" row and keeps `marker ≤ committed high_water` (P04 mirror) at every step. Re-acks of an unchanged base are suppressed (no redundant CURSOR write, counted).

**D14. Subscription timing** — *problem:* the device may indicate STATUS on link-up before the client subscribes, and `SyncPort` has no `open()` step. *Choice:* subscribe lazily and idempotently inside the first async call (`request_from`/`ack`), then immediately write one all-zero **status-query CURSOR** (§3.1) so `status()` reflects the device before the first transfer completes. *Rejected:* a separate `async open()` — changes F21's frozen seam and adds a lifecycle state for a ~2-write startup.

## 7. Module structure
Firmware library `firmware/lib/bw_sync/` (namespace `bw::sync`, pure logic, host-tested):
- `include/bw/sync/sync_config.h` — all §8 constants + `static_assert`s (frame sizes, `kEventRecordBytes == kEventBytes`, payload formula pins).
- `include/bw/sync/crc16.h` + `src/crc16.cpp` — CRC-16/CCITT-FALSE, bitwise (no lookup table: saves flash, table not needed at this rate).
- `include/bw/sync/chunk_packetizer.h` + `src/chunk_packetizer.cpp` — encode data chunks (counter + whole records + CRC), parse/validate control frames (version/type/length), decode chunks (used by the host harness and to document receiver semantics for W5; production firmware calls encode only).
- `include/bw/sync/sync_session.h` + `src/sync_session.cpp` — the state machine: `Idle → Streaming → WaitAck → (Complete | Aborted)`; deps injected by constructor (`IEventStore&`, `ISyncTransport&`); `step(TimeMs now)`; emits `SyncStatus` snapshots (CR-019 part B).
- `src/wire/f20::wire(ISyncTransport*, IEventStore*)` lives in `firmware/src/app/wiring/wire_sync.{h,cpp}` — **includes only `bw_core` + `bw_sync`** (no F19 header; the integrator passes F19's adapter instance in `app.cpp`). Exposes exactly three seams (G1 ruling 4): the `wire(transport, store)` injection point, `f20::pumpOnce(TimeMs)` (F08 pattern; the integrator calls it from the app service cadence — D6, no task) and `f20::statusProvider()` returning `bw::ISyncStatusProvider&` for the P4 UI/BLE binding.

Tests `firmware/test/bw_sync/test_bw_sync/` (Unity, `pio test -e native`, auto-picked by CI):
- `support/lossy_transport_fake.{h,cpp}` — private fake implementing `ISyncTransport`: two inboxes, link flag, scriptable faults (drop/duplicate/hold-then-reorder the nth Data frame, `failNextSend(err, times)` for `Busy`/error injection, `setUsableBytes` MTU override, link up/down). Deterministic scripts only, no randomness; `reorderedByFault()` counts released out-of-order frames so the matrix can assert the fault actually fired.
- `support/phone_harness.{h,cpp}` — plays the companion: receives chunks, CRC-checks, upserts by seq into a set, validates marker semantics, emits acks/requests; used to assert zero loss / zero duplicates.
- `support/failing_event_store.h` — `IEventStore` wrapper over `MemEventStore` (bw_testkit) that fails `readFrom` on the Nth call (fault injection at the store seam).
- `test_crc16.cpp`, `test_packetizer.cpp`, `test_sync_session.cpp` (flow/ack/window/timeouts), `test_sync_fault_injection.cpp` (matrix), `test_wire_sync.cpp` (wiring compile + pumpOnce drives the real path + rewire test, F12/F08 pattern).
- `main.cpp` — registration.

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kChunkPayloadBytes` | MTU−7 | `DERIVED` | ATT header 3 B + our 4 B framing (2 B chunk counter + 2 B trailing CRC16); payload is always a multiple of `kEventRecordBytes` |
| `kAckTimeoutMs` | 5000 | `EXPERIMENTAL` | Resend chunk window |
| `kMaxUnackedChunks` | 4 | `EXPERIMENTAL` | Simple flow control (4 chunks ≈ 20 events in flight at MTU 255) |
| `kSyncProtocolVersion` | 1 | `DERIVED` | First byte of every frame; any layout change bumps it and needs a CR on `SYNC_PROTOCOL.md` |
| `kEventRecordBytes` | 48 (`kEventBytes`) | `DERIVED` | `sizeof(Event)` schema v1, zero padding, pinned by `contract_checks.h`; packetizer `static_assert`s it |
| `kMinSyncMtuBytes` | 55 | `DERIVED` | Smallest MTU that carries one whole record: 3 ATT + 2 counter + 48 record + 2 CRC; below it sync is off (status flag), never partial |
| `kMaxSyncMtuBytes` | 255 | `DERIVED` | Cap on using the negotiated MTU; bounds the tx staging buffer (`kMaxSyncFrameBytes` = 252 B) and single-byte length arithmetic; larger MTUs buy nothing at 5 events/chunk |
| `kMaxEventsPerRead` | 16 | `DERIVED` | Store batch per pump (768 B buffer); bounds one `step()` while still filling ≥3 chunks at MTU 255 |
| `kCursorFrameBytes` | 12 | `DERIVED` | `1 ver + 1 type + 4 from_seq + 2 max + 4 ack_seq`; `static_assert`ed |
| `kStatusFrameBytes` | 11 | `DERIVED` | `1 ver + 1 type + 4 last_seq + 4 last_synced + 1 flags`; `static_assert`ed |
| `kMaxAckTimeoutsPerTransfer` | 3 | `EXPERIMENTAL` | After ~3×`kAckTimeoutMs` with no ack, abort the attempt (status flag), wait for the phone to re-request; bounds wasted radio on a wedged peer |

Companion (`companion/lib/sync/**`, PEP 8 UPPER_SNAKE per F21's style; mirrors of contract values keep the contract's meaning):

| Name | Value | Tag | Why |
|---|---|---|---|
| `SYNC_PROTOCOL_VERSION` | 1 | `DERIVED` | SYNC §5 first byte of every frame; `decode_*` drops mismatches (§4.7), never guesses |
| `FRAME_TYPE_CURSOR` / `FRAME_TYPE_STATUS` | 0x01 | `DERIVED` | SYNC §3.1/§3.2 type byte (same value, disambiguated by channel/direction); `FRAME_TYPE_RESERVED_RAW_WINDOW` = 0x02 is recognized-and-dropped (D5 reserved) |
| `CURSOR_FRAME_BYTES` / `STATUS_FRAME_BYTES` | 12 / 11 | `DERIVED` | SYNC §3.1/§3.2 layout sums; exact-length decode guard |
| `EVENT_RECORD_BYTES` | 48 | `DERIVED` | SYNC §3.4 / CR-002; every chunk payload a multiple of it |
| `CHUNK_OVERHEAD_BYTES` | 4 | `DERIVED` | SYNC §3.3 framing (2 B `chunk_no` + 2 B CRC16); `MIN_CHUNK_FRAME_BYTES` = 52, `MAX_CHUNK_FRAME_BYTES` = 252 (= `kMaxSyncFrameBytes`) |
| `MAX_RECORDS_PER_CHUNK` | 5 | `DERIVED` | (252 − 4) / 48 — encode-side guard for `encode_chunk` (fake device + golden vectors) |
| `CRC16_CCITT_FALSE_POLY` / `_INIT` | 0x1021 / 0xFFFF | `HW` | SYNC §3.3 CRC parameters (check value V1 `"123456789"` → 0x29B1 pins the implementation) |
| `STATUS_FLAG_LINK_UP` / `_STREAMING` / `_MTU_TOO_LOW` / `_TRANSFER_ABORTED` | 0x01/0x02/0x04/0x08 | `DERIVED` | SYNC §3.2 bits; reserved bits 4–7 ignored, never surfaced |
| `MAX_HELD_RECORDS` | 64 | `DERIVED` | D9 contiguity-gate hold buffer: device in-flight worst case = `kMaxUnackedChunks` 4 × `MAX_RECORDS_PER_CHUNK` 5 = 20 records ahead of a hole; 64 ≈ 3× headroom. Overflow ⇒ drop held + count (safe: nothing beyond `base` is acked ⇒ always retransmitted) |
| `SYNC_RUN_TICK_S` | 0.05 | `EXPERIMENTAL` | `runner.main` pump cadence (mirrors bench `SYNC_CLI_TICK_S`); injected as 0 in tests so the suite never sleeps |
| `SYNC_RUN_MAX_ROUNDS` | 2000 | `EXPERIMENTAL` | Bounds a scripted `runner` run while covering a real transfer (≫ bench's 200-round *scripted* bound that `bench.py` line 31 defers to F20's loop); tune with bench use |

Companion constants re-use, never duplicate: `MIN_SYNC_MTU_BYTES` (55) from `transport/ports.py`, `SYNC_REQUEST_BATCH`/`SYNC_STALL_TIMEOUT_MS` from `features/sync_control.py`.

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
Host suite `bw_sync` (Unity, deterministic, no wall clock — `step(now)` takes time explicitly). CI: `ci/check.py` step 1/4 (`pio test -e native`) picks it up automatically. Target ≥ 34 new tests, 0 removed.

Flow / ack / window:
- T01 happy path: link up → request → stream → ack → `markSyncedThrough(head)`; complete.
- T02 first call / empty store: status frame only, no data sends.
- T03 request from mid-history (`from_seq > 1`).
- T04 `max_events` cap honored exactly; `max_events = 0` + `from_seq ≥ 1` = unbounded-by-request.
- T05 status query (all-zero cursor frame) ⇒ status indication.
- T06 status indication on link up and on transfer complete (flags correct).
- T07 flow control: in-flight chunks never exceed `kMaxUnackedChunks` (fake counts).
- T08 ack advances the marker exactly once, to the acked seq.
- T09 duplicate ack → single marker advance, no error.
- T10 backward ack ignored; T11 ack of nonexistent seq ignored; T12 ack beyond store head ignored.
- T13 new request mid-stream aborts and restarts from the new cursor.
- T14 no request ⇒ no data sends (never talk first).

Packetizer / CRC:
- T15 CRC-16/CCITT-FALSE check value (`"123456789"` → `0x29B1`).
- T16 chunk round trip encode→decode; T17 corrupted CRC rejected by decode.
- T18 MTU floor boundary: MTU 55 ⇒ exactly 1 event/chunk; T19 MTU 255 ⇒ 5 events/chunk.
- T20 property: every encoded chunk's payload is a multiple of 48, never a partial record.
- T21 unknown version dropped; T22 short/malformed control frame dropped (both: session keeps working, counters increment).

Fault injection (lossy fake, scripted):
- F01 dropped chunk ⇒ resend after `kAckTimeoutMs`, zero loss.
- F02 duplicated chunk ⇒ harness upsert set unchanged, marker advances once.
- F03 reordered chunks ⇒ harness assembles by seq, device unaffected.
- **F04 disconnect mid-sync ⇒ resume cleanly (M4 exit criterion): marker unchanged at disconnect; next link-up + request delivers the remainder; harness set == source set, each event exactly once.**
- F05 link flap (down+up within one run) behaves as F04 twice.
- F06 reboot mid-sync: fresh `SyncSession` + same (persisted) store; phone re-requests ⇒ complete, no loss/dup.
- F07 persistent send failure ⇒ abort after `kMaxAckTimeoutsPerTransfer`, marker unchanged (spec §10).
- F08 lost ack ⇒ timeout-resend path converges.
- F09 `Err::Busy` backpressure ⇒ retried next pump, no loss.
- F10 MTU below floor ⇒ status flag set, zero data sends.
- F11 store `readFrom` failure (Nth-call fault wrapper) ⇒ attempt retried later, marker unchanged.
- F12 inbound garbage bytes ⇒ dropped, session healthy.

Consistency (property-style, harness assertions):
- P01 idempotent replay: run the whole transfer twice ⇒ companion set identical, size N.
- P02 marker monotonic: never decreases across every scripted run of the fault matrix.
- P03 zero loss + zero duplicates across the whole fault matrix: the script table (16 scripts = `None` + `{Drop, Duplicate, Reorder}` × fault-at nth Data frame for nth = 1..5, `kMatrixScriptCount`/`kMatrixMaxNth` in `test_sync_fault_injection.cpp`) is looped inside both P02 and P03; each faulted script also asserts the fault actually fired (`droppedByFault()` / `duplicateDeliveries()` / `reorderedByFault()` > 0) so a silently broken injector fails the test.
- P04 marker never ahead of the companion's committed high-water.

Wiring:
- W01 `wire_sync` compiles against a fake transport; `f20::wire` + `pumpOnce` drives the real path natively.
- W02 rewire binds a second live context (F12/F08 pattern).

Companion suite `companion/lib/sync/tests/` (pytest, `python -m pytest companion/lib/sync/tests -q`, no BLE hardware, no sleeps — F21's `FakeTransport` + a scripted `fake_device` + real `EventRepository` over tmp SQLite):

Frames (golden vectors from SYNC §6, re-declared from the contract):
- CT01 `crc16_ccitt_false` check value V1 (`"123456789"` → 0x29B1).
- CT02 `encode_cursor` golden V2; CT03 status-query CURSOR (all-zero body) and request+ack combined frame; CT04 `decode_cursor` round trip.
- CT05 `encode_status`/`decode_status` golden V3 incl. flag-bit mapping (reserved bits ignored).
- CT06 `encode_chunk`/`decode_chunk` golden V4 (52 B, CRC 0xD2A9) round trip; 5-record chunk at 244 B.
- CT07 corrupted CRC rejected; CT08 wrong length (`% 48 != 4`, < 52 B, > 252 B) rejected; CT09 unknown version / reserved type / wrong STATUS length dropped (`FrameError`, counters — §4.7).

Client (FakeTransport drives subscriptions and captures writes):
- CT10 first `request_from` subscribes both characteristics, then writes CURSOR + status query (D14); MTU < 55 ⇒ `SyncMtuError`, zero writes (defense in depth behind F21's gate).
- CT11 `ack` writes `ack_seq = base`, monotonic; CT12 duplicate ack (base unchanged) suppressed + counted.
- CT13 contiguity gate: records 11–15 arriving before 6–10 ⇒ sink not called for them, `ack(15)` sends nothing past `base` (D9/D10); gap fill releases held records in order in one sink call; `seq ≤ base` duplicates dropped + counted.
- CT14 record-sink exception ⇒ `base` not advanced, `sink_errors` counted, no ack (failure behavior).
- CT15 STATUS frame ⇒ `status()` + `status_sink` fire; `linked=True` from construction; flags surfaced, never swallowed.
- CT16 malformed data-channel frames dropped, counters increment, client still works on the next good chunk.

Full loop (floor — FakeTransport + `fake_device` + real repo + F21 `SyncControlViewModel`, driven by `run()` with a `FakeClock`; the fake device applies §4.3 ack validation, §4.2 window/resend, and records every CURSOR):
- CT17 **happy path**: 24 events over 5 chunks ⇒ rows == 24, `high_water` == 24, device marker == 24, view `done`; per-round invariant `marker ≤ committed high_water` and every request `from_seq-1 ≤ high_water` (P04 mirror + "cursor never ahead of committed rows").
- CT18 **idempotent resume**: second full run transfers nothing (row count unchanged, zero dupes); simulated companion reboot (fresh view-model + client, same DB) resumes from `hw+1` and converges.
- CT19 dropped chunk mid-stream ⇒ no ack past the hole, device resend after `kAckTimeoutMs` fills it ⇒ zero loss, zero dupes, invariant holds every round (§4.7 row).
- CT20 corrupted chunk (CRC flipped) ⇒ dropped at the phone, resend heals, same invariants.
- CT21 duplicated chunk delivery ⇒ `event_count` unchanged, marker advances exactly once.
- CT22 device stall (stops serving, then answers the view-model's stall re-request) ⇒ converges; view returns `stalled` at least once (companion-side F04/F08 analog).
- CT23 lossy matrix (loop over `drop@{1..5}` × `corrupt@{1..5}` × `duplicate@{1..5}` scripts, fake device asserts the fault fired): final set == source set, zero dupes, marker == head, invariant every round.

Not covered on host (deliberate, noted at Validate): real bleak/WinRT callback threading (the seam is synchronous-callback by contract `[ASSUMED]`), real indication-confirmation timing, multi-device sessions.

Not covered on host (deliberate, noted at Validate): real NimBLE adapter behavior (F19's device-only glue — device/companion checklist in `SYNC_PROTOCOL.md` §10), real MTU negotiation, phone-side Dart tests (W5). No-heap: static storage is enforced structurally (fixed members, bounded reads); add an allocation-hook test only if a native hook pattern exists in the repo at IMPLEMENT, else state the non-coverage.

## 10. Failure behavior
- Any sync error leaves `lastSyncedSeq` unchanged.
- `Err::Busy` on send ⇒ retry on a later pump; link down ⇒ immediate abort, no marker change; `Err::NotReady` should not be observed (the session polls `linkUp()` first; if it happens, treat as abort).
- Ack timeouts ⇒ bounded resends (`kMaxAckTimeoutsPerTransfer`), then abort + status flag; the phone re-requests.
- Malformed / unknown-version / oversized frames ⇒ dropped and counted, never a crash, never a NACK.
- MTU < `kMinSyncMtuBytes` ⇒ sync disabled with a status flag; events keep accumulating locally.
- Store read failure ⇒ attempt abandoned and retried; the marker never moves backward and never moves without a valid ack.

Companion (`lib/sync`, W5):
- CRC/length/version/type violation in a chunk or STATUS ⇒ frame dropped + counter; the record sink is never called with invalid bytes (the sink contract is "exactly-48-B slices", `sync_port.py`).
- A hole (dropped chunk, out-of-order arrival) ⇒ records beyond it are held, nothing beyond `base` is acked, no cursor advance; healed by device resend (§4.2) or the view-model's stall re-request; hold-buffer overflow ⇒ drop held + count (still nothing acked past `base`, so retransmission covers it).
- Record-sink exception (DB locked/corrupt) ⇒ `base` not advanced, `sink_errors` counted, no ack written; the session stays alive and the stall path retries (device resends from `base+1`).
- `request_from`/`ack` with `transport.mtu_size < 55` ⇒ `SyncMtuError`, **zero writes** (defense in depth behind F21's D8 gate; composition roots gate before constructing).
- Disconnected transport under a write ⇒ exception propagates to the composition root (bench/app already wrap `PermissionError`/link loss); the client keeps no state that would lie after reconnect — a fresh `build_sync_port` + `start()` re-subscribes (§4.6).

## 11. Budget
- **RAM < 3 KB** (spec): F20-owned = `sizeof(SyncSession)` = **1456 B** (host-measured via a `printf` in a throwaway test run, FIX loop 1) — read buffer 768 B (16×48), tx staging 252 B, control rx **252 B** (the CR-019 contract requires `receive` cap ≥ `kMaxSyncFrameBytes`; the plan's 12 B was corrected at IMPLEMENT), flow-control window 4×16 B = 64 B, remaining state (counters/flags/window metadata) 120 B. Under the 3 KB budget with ~1.6 KB headroom; the single instance is owned by the wiring layer, no per-frame heap. `[ASSUMED]` the F19 transport adapter's queues fit F19's own budget (~1 KB), counted there.
- No new FreeRTOS task (D6) ⇒ no stack cost; `pumpOnce` bounded (≤ 16 event reads + ≤ window sends + ≤ 8 inbound control frames per call); CRC bitwise over ≤ 252 B/frame. `[EXPERIMENTAL]` pump ≤ ~2 ms on 240 MHz; measure at Validate (H3).
- Flash: measured at IMPLEMENT via `pio run -e esp32dev` (H3); packetizer/CRC are small (no CRC table by design).
- No heap after startup, no `String`, no exceptions/RTTI, no blocking calls (pure `step()`).

Companion (W5, host Python — no embedded budget applies; stated so the DoD has numbers):
- `SyncClient` state: latest `SyncStatus` snapshot + hold buffer ≤ `MAX_HELD_RECORDS` 64 × 48 B ≈ **3 KB** worst case (typical 0 with in-order delivery) + a few int counters; no threads, no timers, no tasks (D11), no caching of records beyond the hold buffer (records live in SQLite).
- Per-event cost: one `decode_event48` for the seq + one CRC pass over ≤ 252 B per chunk — negligible at prototype rates; suite target **≥ 25 tests, runtime < 3 s** (baseline F21 suite: 94 tests / 1.47 s).

## 12. Out of scope
- Bulk raw-window transfer (later; separate characteristic/CR — D5 keeps it on request).

## 13. Open questions / assumptions
- ~~Whether raw windows sync automatically or on request~~ → **RESOLVED by D5: on request** `[DECIDED 2026-10-15]`.
- ~~`[ASSUMED]` F19 requests/accepts an ATT MTU ≥ 55 and reports subscription state in `linkUp()`~~ → **reconciled at G1 (2026-10-15): accepted** — `bw/sync_transport.h` pins `maxSendBytes()` = MTU − 3 with "≥ 52 B usable (ATT MTU ≥ 55) to stream" and `linkUp()` = connected + encrypted + subscribed.
- ~~`[ASSUMED]` the transport adapter is called only from the sync pump context~~ → **reconciled at G1: pinned in the CR-019 contract comment** (single caller = the F20 sync pump; F19 serializes internally).
- `[ASSUMED]` app cadence for `pumpOnce` is ≥ every few seconds when connected (exact call site is the integrator's, H7/H8).

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Fault-injection test suite in CI (F01–F12 run under `pio test -e native` in `ci/check.py`).
- M4 exit evidence: F04 (disconnect mid-sync resumes cleanly) green on host.
- `SYNC_PROTOCOL.md` complete per §16 checklist (W5 implementer needs no firmware knowledge).
- CR-019 decided (G1) before IMPLEMENT; no `bw_core` edit without it.

> **G1 outcome (2026-10-15): accepted with two edits.** The normative text is now `firmware/lib/bw_core/include/bw/sync_transport.h`: (a) the floor comment reads **"≥ 52 B usable (48 B record + 4 B framing ⇒ ATT MTU ≥ 55)"** — the "51" below was an arithmetic slip; (b) F19 is a dumb byte pipe: no `SyncControlFrame`/`LinkEvent` — the engine parses raw control frames from `receive()` and diffs `linkUp()`/`maxSendBytes()` per pump. The transcription below is kept as the filing record.

**Title:** CR-019 — `bw::ISyncTransport` + `bw::SyncStatus` / `ISyncStatusProvider` (F20 sync seam)

**Problem:** F20's `SyncSession` must run on the host against a lossy fake and must not include F19's headers (01 §1 hard rule 1), but `bw_core` has no transport contract. The F20 spec's §3 also promises "sync status for UI/BLE", which needs a read-only provider interface. F19 (adapter) and F20 (engine) are implementing in the same wave and need one agreed seam now.

**Proposal — new additive header `firmware/lib/bw_core/include/bw/sync_transport.h`:**

```cpp
#pragma once
// bw/sync_transport.h — CR-019 (proposed). Transport seam between the F20 sync
// engine and the F19 BLE adapter; see docs/contracts/SYNC_PROTOCOL.md.

#include <cstddef>
#include <cstdint>

#include <bw/result.h>
#include <bw/types.h>

namespace bw {

// Which logical pipe a sync frame belongs to (02 §6.2 characteristics).
// Append-only: new values may be appended, never renumbered.
enum class SyncChannel : uint8_t {
    Control = 0,  // Sync Control: Write phone->device, Indicate device->phone
    Data    = 1,  // Sync Data: Notify device->phone only
};

// [DERIVED] Largest frame the transport ever passes: ATT MTU is capped at
// 255 B and the 3 B ATT notification header is stripped by the stack,
// leaving 252 usable bytes (kMaxSyncMtuBytes - 3, SYNC_PROTOCOL.md §3.3).
constexpr uint16_t kMaxSyncFrameBytes = 252;

// Duplex byte pipe for the sync protocol. Call context: exactly one caller —
// the F20 sync pump (a single task) — invokes send()/receive()/maxSendBytes();
// implementations must serialize internally if their own callbacks run in
// another context (e.g. the NimBLE host task). Frames are opaque to the
// transport; all protocol semantics live in the F20 engine.
class ISyncTransport {
 public:
    // device -> phone. Requires 0 < len <= kMaxSyncFrameBytes.
    //  Ok           frame accepted (Data: queued for notification;
    //                              Control: queued for indication).
    //  Err::NotReady  link is down; nothing sent; retry later.
    //  Err::Busy      link congested (e.g. previous indication unconfirmed);
    //                 nothing consumed; retry on a later pump.
    //  Err::InvalidArg len == 0 or len > kMaxSyncFrameBytes (caller bug;
    //                 nothing sent).
    virtual Result<void> send(SyncChannel ch, const uint8_t* data, uint16_t len) = 0;

    // phone -> device. Copies the OLDEST pending inbound frame into `out`
    // (capacity `cap`) and reports its channel in `ch_out`.
    //  Ok(0)    no frame pending (also right after a disconnect: inbound
    //           queues are discarded when the link drops).
    //  Ok(n>0)  one frame of n bytes delivered; n <= cap.
    //  Err::Overflow  a pending frame was larger than `cap` and was dropped
    //                 (correct callers pass cap >= kMaxSyncFrameBytes).
    virtual Result<size_t> receive(SyncChannel* ch_out, uint8_t* out, size_t cap) = 0;

    // True when the sync link is usable: connected, encrypted per bonding
    // policy, and the peer has enabled Sync Data notifications.
    // Polled every pump; false means "abort any transfer immediately".
    virtual bool linkUp() const = 0;

    // Usable bytes per Data frame (negotiated ATT MTU minus the 3 B ATT
    // header), in [0, kMaxSyncFrameBytes]; 0 when no MTU has been negotiated.
    // Meaningful only while linkUp() is true. The engine needs >= 51 B
    // (one 48 B record + 4 B framing) before it streams (SYNC_PROTOCOL.md §3.3).
    virtual uint16_t maxSendBytes() const = 0;
};

// CR-019 part B — sync status for UI/BLE (F20 spec §3 "Sync status").
struct SyncStatus {
    static constexpr uint8_t kSchemaVersion = 1;
    TimeMs   t_ms;               // snapshot time
    uint32_t last_seq;           // newest seq known to the engine (store head seen)
    uint32_t last_synced_seq;    // last cumulative ack committed (= IEventStore.lastSyncedSeq)
    uint32_t events_sent;        // cumulative this boot (diagnostics/UI)
    uint32_t chunks_resent;      // cumulative this boot (diagnostics)
    uint16_t protocol_version;   // kSyncProtocolVersion the engine speaks
    uint8_t  flags;              // bit0 linkUp, bit1 streaming, bit2 mtu_too_low,
                                 // bit3 transfer_aborted (last attempt gave up);
                                 // bits4..7 reserved (0)
};
class ISyncStatusProvider {
 public:
    virtual bool latest(SyncStatus&) const = 0;  // false = no snapshot yet
};
}  // namespace bw
```

**Interface doc section:** append the block above to `docs/interfaces/services.md` §5 (after `IConfirmPromptProvider`, marked `// CR-019 (...)`), matching the file's existing style. `docs/interfaces/external.md` §6.3 needs no change; optionally append one sentence: "Transport seam: `bw::ISyncTransport` (services §5, CR-019)." — included in this CR for one-pass editing.

**Migration / compatibility:** purely additive (new header, new enum, new struct, new provider); no existing type, enum value or signature changes; no behavior change for features that ignore it. Enums append-only per 02 §6.6. `SyncStatus` carries `kSchemaVersion` for future persistence.

**Test plan:** `bw_sync` host suite pins the semantics (T07 window, F09 Busy, F10 MTU floor, F12 garbage); a contract test asserts `kMaxSyncFrameBytes == kMaxSyncMtuBytes - 3` and the two frame-size `static_assert`s.

~~`companion/lib/sync/event_repository.*`~~ → **superseded.** The only SQLite writer is F21's `data/event_repository.py` (schema v2, `data/db.py`): `upsert(device_id, records)` keyed `(device_id, seq)`, durable `high_water`, `wall_attempted`. The upsert path is `SyncControlViewModel.on_records → repository.upsert(...)` (commit) → `tick → port.ack(...)` — commit strictly before ack, no second writer, no raw SQL in `lib/sync`. CR-023 analysis: **no gap** under D9 (contiguity gate keeps `MAX(seq)` == committed prefix); the pre-allocation is returned unraised.

~~`companion/lib/sync/time_reconstruction.*`~~ → **superseded.** The only wall-time resolver is F21's `domain/time_reconstruct.py` (SYNC §4.5), invoked inside `EventRepository.upsert` (`_recompute_wall_times`). No second implementation; `lib/sync` never touches wall time.

Checklist for "complete enough" (DoD): frame layouts both directions + endianness · versioning rule · flow control + timers (owned: device §4.2 + view-model stall; client none, D11) · error/edge matrix SYNC §4.7 (+ companion rows in spec §10) · idempotency keys SYNC §5 · reconstruction rule SYNC §4.5 (consumed via F21's resolver) · test vectors SYNC §6 · characteristic mapping from BLE_GATT.md (F21's `transport/ports.py` UUIDs) · MTU floor (F21 gate + client defense-in-depth).

---
