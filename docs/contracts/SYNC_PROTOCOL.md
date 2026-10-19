# Sync protocol contract

> **Status: v1 COMPLETE** (F20, IMPLEMENT, 2026-10-15 — frame layouts are frozen; changes go through a change request + version bump, per §5). Owner: **F20**. Expands `docs/02_INTERFACES.md` §6.3.
> Transport seam: `bw::ISyncTransport` (**CR-019**, accepted 2026-10-15, `docs/interfaces/services.md` §5). Characteristic UUIDs, bonding and MTU negotiation are owned by F19 → `docs/contracts/BLE_GATT.md`; the UUID column below carries `[F19]` placeholders until that file is filled.

## 1. Purpose and scope
Cursor-based, idempotent transfer of `Event` records (48 B, schema v1) from the wearable to the phone, tolerant of disconnects, partial transfers, duplication and reordering. `(device_id, seq)` uniquely identifies an event; the companion upserts by that key, so retransmission is always safe. The device **marks** events synced (`IEventStore.markSyncedThrough`) and never deletes; the wearable never depends on sync for alerting or storage (00 non-negotiables). Raw sensor windows are **not** part of v1 (on request only, F20 spec D5; type `0x02` reserved).

## 2. Link, characteristics, direction
Little-endian everywhere. Frames are opaque to F19's transport (`bw::ISyncTransport`, `SyncChannel`).

| Role | Characteristic (F19 owns UUID) | GATT props | Direction | Frame(s) |
|---|---|---|---|---|
| Sync Control | `[F19]` custom, encrypted | W, I | phone→device (W): §3.1 · device→phone (I): §3.2 | CURSOR, STATUS |
| Sync Data | `[F19]` custom, encrypted | N | device→phone only | chunk (§3.3) |
| Device Info | `[F19]` custom | R | device→phone | `device_id` read by the companion once per connection (upsert key) |

MTU floor: **`kMinSyncMtuBytes` = 55** `[DERIVED]` (3 ATT + 2 counter + 48 record + 2 CRC). Below it the device sets the `mtu_too_low` status flag and sends no chunks. Usable frame bytes = negotiated ATT MTU − 3 (ATT opcode + handle), capped at `kMaxSyncFrameBytes` = 252 (`kMaxSyncMtuBytes` 255 − 3).

## 3. Frame layouts
### 3.1 CURSOR — phone → device (Sync Control Write), `kCursorFrameBytes` = 12 B
| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 1 | `version` | `kSyncProtocolVersion` (1) |
| 1 | 1 | `type` | `0x01` = CURSOR (`0x02` reserved: future raw-window request, F20 §12) |
| 2 | 4 | `from_seq` u32 | ≥ 1: start/restart streaming from this seq. `0` = no request. |
| 6 | 2 | `max` u16 | cap on events for this round; `0` = no request-cap (window only) |
| 8 | 4 | `ack_seq` u32 | highest seq durably stored by the companion; `0` = no ack |

`from_seq = 0 && max = 0 && ack_seq = 0` is a **status query** → device replies with §3.2. Fields are processed independently: one frame may carry a new request *and* an ack. The newest CURSOR write is authoritative: a request mid-stream aborts and restarts at the new `from_seq`.

### 3.2 STATUS — device → phone (Sync Control Indicate), `kStatusFrameBytes` = 11 B
| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 1 | `version` | 1 |
| 1 | 1 | `type` | `0x01` = STATUS |
| 2 | 4 | `last_seq` u32 | newest seq known to the device (store head) |
| 6 | 4 | `last_synced` u32 | device's committed marker |
| 10 | 1 | `flags` | bit0 `link_up`, bit1 `streaming`, bit2 `mtu_too_low`, bit3 `transfer_aborted`; bits4–7 reserved, 0 |

Sent: on link up, on status query, on transfer complete, on abort (§4.7).

### 3.3 CHUNK — device → phone (Sync Data Notify), 52…252 B
| Offset | Size | Field | Meaning |
|---|---|---|---|
| 0 | 2 | `chunk_no` u16 | per-transfer counter starting at 1, +1 per sent chunk; a resend reuses the original `chunk_no`. Informational only (diagnostics, resend matching) — **correctness uses event `seq`** |
| 2 | 48·n | payload | `n ≥ 1` **whole** `Event` records (§3.4); payload length is always a multiple of 48; records are never split across chunks |
| 2+48n | 2 | `crc16` u16 | CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflect, xorout 0x0000) over bytes `[0, 2+48n)` |

`n = (frame_len − 4) / 48`; `frame_len % 48 == 4` must hold. `n = 5` at MTU 255: 240 payload bytes used of the 248 available (255 − 3 ATT overhead − 2 `chunk_no` − 2 `crc16`), 8 B slack since 248 % 48 ≠ 0.

### 3.4 Event record encoding
The record is **not an independent encoding**: it is the byte image of the canonical 48-byte `bw::Event` record (schema v1) defined in `firmware/lib/bw_core/include/bw/events.h` — `docs/02_INTERFACES.md` §4.5 / CR-002 / `F28-EVENT-VECTOR-48B` — whose field offsets and zero-padding are pinned at build time by `bw/contract_checks.h`. The table below is a **transcription of that contract for companion implementers**, not a second definition; if the two ever disagree, the C++ contract wins and this doc is drift (a CR).
All integers little-endian; `confidence` is IEEE-754 binary32 LE; enums are 1-byte (`uint8_t`-backed) raw values. On the firmware side a chunk payload is a verbatim memcpy of the struct (both ends are LE: Xtensa/x86/phone-ARM, D8); consumers decode **by the contract's offsets** (never by struct cast).

| Off | Size | Field | Off | Size | Field |
|---|---|---|---|---|---|
| 0 | 8 | `t_start_ms` u64 | 32 | 8 | `payload[8]` |
| 8 | 4 | `seq` u32 | 40 | 2 | `quality` (SourceQuality) |
| 12 | 4 | `boot_id` u32 | 42 | 1 | `type` (EventType) |
| 16 | 4 | `duration_ms` u32 | 43 | 1 | `label` (UserLabel) |
| 20 | 4 | `t_start_unix` u32 (0 = unknown) | 44 | 1 | `context` (ActivityContext) |
| 24 | 4 | `confidence` f32 | 45 | 1 | `alertness_at_start` |
| 28 | 4 | `reasons` (ReasonMask) | 46 | 1 | `detector_id` |
| | | | 47 | 1 | `schema_version` (= 1) |

Consumers decode **by offset** (never by struct cast). F19's Event-notify `Codec` must emit the identical byte image (G1 reconciliation).

## 4. Semantics
### 4.1 Cursor model
Phone drives: it stores its own durable high-water `hw` (max committed seq, persisted) and requests `from_seq = hw + 1`. Device streams from `from_seq` while events exist, until `max` is reached or the flow-control window fills. Device state is disposable: after a reboot it just serves requests; only `lastSyncedSeq` (persisted by F18) matters.

### 4.2 Streaming and flow control
At most `kMaxUnackedChunks` (4) chunks in flight ahead of the last ack; `kAckTimeoutMs` (5000 ms) after the oldest unacked chunk expires ⇒ resend the unacked suffix (same `chunk_no`s) from the store. At most `kMaxEventsPerRead` (16) events are read from the store per pump; the engine never blocks.

### 4.3 Ack and commit (device side)
`ack_seq` is applied only if `ack_seq > last_synced` **and** `readFrom(ack_seq, &e, 1)` returns exactly that seq (exists-in-store check; also bounds `ack_seq` by the store head). Then `markSyncedThrough(ack_seq)` is called exactly once and the window slides. Duplicate, backward, zero, unknown or out-of-range acks are ignored and counted. The marker never moves without a valid ack and never backward.

### 4.4 Idempotency
`(device_id, seq)` is the upsert key (companion SQLite). Resends, duplicate chunks and re-runs cannot create duplicates; the companion drops any record with `seq ≤ hw` after commit or upserts it as a no-op.

### 4.5 Time reconstruction (companion; 02 §6.1 — rule owned by F20)
For an event `e`:
1. If `e.t_start_unix != 0` → wall time is that value.
2. Else pick the `TimeSync` event `T` with the same `boot_id`, `T.unix_s != 0`, minimizing `|e.t_start_ms − T.t_mono_ms|` (tie → the earlier one); wall time = `T.unix_s + (e.t_start_ms − T.t_mono_ms) / 1000` (signed ms math).
3. Else wall time is **unknown** — stored as 0 and flagged in the UI; never fabricated.

Drift over hours is `[ASSUMED]` acceptable (02 §6.1).

### 4.6 Disconnect / reboot
Disconnect mid-transfer: inbound queues are discarded (`receive` → 0), no ack ⇒ marker unchanged; on reconnect the phone re-requests. Device reboot mid-transfer: same; the phone's re-request carries the truth. Companion reboot: its persisted `hw` is the resume point.

### 4.7 Error / edge matrix
| Situation | Required behavior |
|---|---|
| Link drops mid-chunk | Device aborts, marker unchanged; phone re-requests on next link-up |
| Chunk corrupted (CRC) | Phone drops the chunk; timeout ⇒ device resends; no ack sent |
| Ack lost | Device resends after `kAckTimeoutMs`; phone upserts idempotently, re-acks |
| Duplicate / backward ack | Ignored + counted; marker monotonic |
| Phone re-requests mid-stream | Old transfer aborted; stream restarts at new `from_seq` |
| Device reboot mid-stream | Stateless restart; phone's `hw` drives resume; zero loss/dup |
| Unknown `version` / `type`, wrong length, garbage | Frame dropped, counters incremented; no crash; no NACK (phone recovers via timeout) |
| MTU < 55 | `mtu_too_low` flag in STATUS; no chunks |
| No ack for `kMaxAckTimeoutsPerTransfer` (3) consecutive `kAckTimeoutMs` timeouts | Abort attempt, STATUS with `transfer_aborted`; phone re-requests |
| Store read error mid-stream | Attempt abandoned and retried on a later pump; marker unchanged |
| `max = 0` with `from_seq ≥ 1` | Stream until window/exhaustion (no per-request cap) |

## 5. Versioning
`kSyncProtocolVersion` = 1, first byte of every frame. Any layout/semantic change: bump, CR on this doc (and on `BLE_GATT.md` if UUIDs/props change). Enums/flag bits are append-only.

## 6. Test vectors
- **V1** CRC-16/CCITT-FALSE check value: `"123456789"` → `0x29B1`.
- **V2** CURSOR `version=1, type=1, from_seq=1, max=64, ack_seq=0` → `01 01 01 00 00 00 40 00 00 00 00 00` (12 B).
- **V3** STATUS `version=1, type=1, last_seq=100, last_synced=32, flags=0x01` → `01 01 64 00 00 00 20 00 00 00 01` (11 B).
- **V4** 1-event CHUNK, generated by the host test `test_packetizer_v4_chunk_golden` (frame = 52 B). Fixture record: `t_start_ms=1000, seq=7, boot_id=3, duration_ms=250, t_start_unix=0, confidence=0.5f, reasons=0, payload=0, quality={Good,Good}, type=DrowsinessEvent(1), label=Unlabeled(0), context=Unknown(0), alertness=Unknown(0), detector_id=9, schema_version=1`:

  ```
  01 00 E8 03 00 00 00 00 00 00 07 00 00 00 03 00
  00 00 FA 00 00 00 00 00 00 00 00 00 00 3F 00 00
  00 00 00 00 00 00 00 00 00 00 00 00 01 00 00 00
  09 01 A9 D2
  ```
  (`01 00` = chunk_no 1; bytes 2..49 = the record's byte image; `A9 D2` = CRC-16/CCITT-FALSE = `0xD2A9` LE over bytes [0,50).)

- **V5** End-to-end transcript with one lost ack and the resulting resend, exported by the harness of host test `test_fault_lost_ack_timeout_resend_converges` (5 events, MTU 255, ack suppressed for the first round, then `kAckTimeoutMs` elapses). `P>` = phone→device CURSOR write, `D<` = device→phone frame:

  ```
  D< STATUS last=0 synced=0 flags=0x01
  P> CURSOR from=1 max=0 ack=0
  D< CHUNK#1 seq=1..5 (5 ev)
  D< CHUNK#1 seq=1..5 (5 ev)      <- resend, same chunk_no, after kAckTimeoutMs
  P> CURSOR(ack) from=0 max=0 ack=5
  D< STATUS last=5 synced=5 flags=0x01
  ```
  The resent chunk is a byte-identical duplicate; the companion upserts it as a no-op (§4.4) and its cumulative ack moves the device marker exactly once (§4.3).

## 7. Device/companion bench checklist (never replaces host tests)
Pair/bond · reconnect resumes from companion `hw` · encrypted-write enforcement on both characteristics · MTU negotiation ≥ 55 · notification subscription precedes streaming (else `link_up` false) · companion shows reconstructed wall times after a `TimeSync`.

## 8. Characteristic/UUID fill-in (F19, from BLE_GATT.md)
| Role | UUID | Notes |
|---|---|---|
| Sync Control | `[F19]` | W + I, encrypted + bonded |
| Sync Data | `[F19]` | N, encrypted + bonded |
| Device Info | `[F19]` | `device_id` field offset defined in BLE_GATT.md |

## 9. Version history
| v | Date | Author | Change |
|---|---|---|---|
| 1 | 2026-10-15 | F20 (PLAN) | Initial draft: frames, semantics, edge matrix, reconstruction rule, vectors V1–V3. |
| 1 | 2026-10-15 | F20 (IMPLEMENT) | Marked COMPLETE: §3.4 reworded to reference the canonical `bw::Event` contract by name (G1 ruling 2); vectors V4–V5 filled from the host suite; floor comment aligned with CR-019 (52 B usable ⇒ ATT MTU ≥ 55). |
