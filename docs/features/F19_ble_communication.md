# F19 — BLE communication

> Track **D** · Milestone **M4** · Runs on **firmware** · Contract changes only via change request.

## 1. Responsibility
Dedicated BLE layer between wearable and companion: advertising, secure bonding, GATT services (device info, battery, live status, HR, event notify, command, config, sync), with no application logic.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- `IStateProvider`, `IFeatureProvider` (HR), `IBatteryMonitor`, `ISettings`, `ICommandSink` (writes), `IFaultReporter` (BleDown report)
- `IEventStore` — newest-event read for the Event Notify characteristic (D7; replaces the earlier "IEventSink notifications" wording, which no contract actually supports: `IEventSink` only has `publish()`)
- Sync engine hooks (`bw::ISyncTransport` — CR-019 Accepted and applied; F19's `SyncTransport` implements it. §15 records the implemented surface for G1 reconciliation)

**Outputs (produced contracts)**
- GATT database per contract (`docs/contracts/BLE_GATT.md`, completed per §17)
- `Command` via `ICommandSink` (encrypted writes only)
- Connection state provider for F12/F23 — new contract via **CR-018** (draft in §16; `BleLinkState` + `IBleLinkProvider`)

F19 emits **no** `Event` records of its own; registry `detector_id = 19` is therefore not used.

## 4. Dependencies
- `bw_core` only (compile-time). Host tests: Unity + `bw_testkit` fakes (`InMemoryEventStore`, settings/provider fakes).
- Device-only: NimBLE-Arduino 2.5.1 (already in the `esp32dev` dependency graph). All NimBLE calls live in device-glue modules (§7), never in pure logic.

## 5. Must not
- Contain decision logic.
- Expose health data on unencrypted links.
- Block real-time tasks (runs on core 0; alerting F10 on core 1 never waits on BLE — hard non-negotiable from `00`).

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Security** *(unchanged)*
- Simplest: standard NimBLE bonding (Just Works) with encrypted command/config characteristics.
- Professional/scalable: LE Secure Connections with a passkey shown on the OLED and a pairing window.
- Trade-offs: passkey adds UI work (F12/F11) for protection that a breadboard prototype does not need.
- **Choice:** the simplest; stronger pairing is a later change request (the `PairingManager` module keeps it replaceable).

**D2. Live data cadence** *(unchanged)*
- Simplest: Notify at sensor rate.
- Professional/scalable: 1 Hz status, event-driven others.
- Trade-offs: Lower rate saves power/bandwidth.
- **Choice:** ≤ 1 Hz while subscribed (`kStatusNotifyIntervalMs`).

**D3. UUID generation** *(resolves §13 open question)*
- Problem: the custom service/characteristic UUIDs must be fixed once, recorded in `BLE_GATT.md`, and provably match the code.
- Simplest: generate one random 128-bit UUID per characteristic (one-off `uuidgen`), hard-code the strings.
- Professional/scalable: **one generated 128-bit base + 16-bit index** in the last two bytes (SIG-style). Adding a characteristic = allocating the next index in the contract table; no generator rerun, no randomness, self-documenting order.
- Trade-offs: per-characteristic randoms are fine but every future addition needs another generation step and the code strings are unrelated (doc↔code drift is easier to introduce). Base+index gives derived UUIDs a host test can compute and pin, so an accidental change fails `pio test -e native`.
- **Choice:** base + index. Base generated once on 2026-10-15 (`[guid]::NewGuid()`), now fixed:
  `4bb00319-8811-4387-95a6-f4bb7838%04x` with `%04x` = index. Service = index `0x0000`.

**D4. Service topology and standard-service mapping**
- Problem: which characteristics live in which service; §6.2 of `external.md` mandates standard Device Information (0x180A) and Battery (0x180F) plus custom 128-bit characteristics.
- Simplest: one custom service holding **everything** (including a custom Device Info characteristic).
- Professional/scalable: standard DIS + standard Battery for the tool-friendly device facts, **one** custom service for everything else; Heart Rate keeps the standard 0x2A37 byte format but sits under the custom service (only 0x180A/0x180F are sanctioned as standard services by the locked contract).
- Alternatives rejected: a standard Heart Rate service (0x180D) — not in the locked service list, would need a CR for a benefit only generic apps would notice; four custom characters for Device Info — invents payload where standard strings suffice.
- Trade-offs: standard DIS uses strings (schema versions need a defined string format); a custom packed char would be typed but adds another UUID and special-casing in the companion.
- **Choice:** standard DIS carries `fw version` (0x2A26), `hw rev` (0x2A27), `schema versions` (0x2A28, format defined in the contract, includes the GATT contract version), `device_id` (0x2A25 = lowercase 12-hex BLE address, the `device_id` half of `(device_id, seq)`). Battery Level 0x2A19 as standard. All custom characteristics live in one service (index `0x0000`). The §6.2 "Device Info" row is thus satisfied by four standard DIS characteristics — documented as such in `BLE_GATT.md`.

**D5. GAP advertising payload**
- Alternatives: (a) flags + complete local name only; (b) flags + 128-bit custom service UUID in advertising data, complete local name in scan response.
- Trade-offs: (a) lets a phone show the name but not filter by service; (b) fits the 31-byte advertising limit (3 B flags + 18 B service UUID = 21 B) and lets the companion scan-filter by service, name (11 B) goes to scan response.
- **Choice:** (b). Advertising only while disconnected, interval `kAdvertisingIntervalMs`. Name `kDeviceLocalName` = `"Anarolepsy"` (fixed build identity, recorded in the contract).

**D6. Wire encoding rules, versioning and error mapping**
- Problem: byte layouts must be stable, testable on both ends, and honest about "no data".
- Alternatives: no versioning (rely on the contract document alone) vs. a version byte in every message vs. a version byte only in layouts that may evolve.
- Trade-offs: version bytes everywhere cost 1 B and clutter standard messages; none means any layout change breaks old companions silently.
- **Choice:**
  - **Endianness: little-endian** for all multi-byte integers (xtensa + BLE convention).
  - Version byte `kGattContractVersion` in: Live Status, Config, Sync Control. **Not** in Heart Rate (standard format), Command (the append-only `CommandType` enum is the version channel: an unknown type is rejected, never guessed), Event (carries `Event::kSchemaVersion` inside its pinned 48-byte record), Sync Data (framing owned by F20's `SYNC_PROTOCOL.md`, opaque to F19).
  - **"No data" is explicit:** Live Status read before the first `StateSnapshot` → **empty (0-byte) read** - never a fabricated snapshot (NimBLE 2.5.1 [MEASURED]: the read callback cannot return ATT `0x84`, so an empty value is the honest signal); no HR notification while `hr_valid == false`; battery value `0xFF` (unknown) while `IBatteryMonitor::read().valid == false` **or its `percent_estimate` is non-finite** (`encodeBatteryLevel`, host-tested — audit AUD-7); Event Notify suppressed entirely when ATT MTU < 51 (never a truncated 48-byte record).
  - **Error map - two classes (NimBLE 2.5.1 [MEASURED]):** the 2.x write callback runs *after* the stack stores the value and cannot reject, so F19 separates (a) **real ATT errors** the stack enforces before application code - write longer than a row's `max_len` → `0x0D Invalid Attribute Value Length`, unbonded/unencrypted access to a protected row → `0x05`/`0x0F`, **CCCD write over an unencrypted link → `0x05`/`0x0F` (audit AUD-1)** - from (b) **counted semantic rejects** (drop + `BleStats` counter, no ATT error): unknown `CommandType` → `command_rejected`; `ICommandSink::push` refused → `command_rejected`; Config write unknown name / type mismatch / malformed length / unknown version → `config_rejected`; callback-side length refusals (short Command, Config < 9 B or 65..512 B, Sync Control 33..252 B) → `writes_rejected`. Clamped settings succeed (per `services.md`: clamp, not reject). A due notification on an unencrypted link is suppressed by `PublishGate` and counted in `notifications_dropped`. Full map in `BLE_GATT.md` 5.

**D7. Event Notify source** (characteristic mandated by `external.md` §6.2)
- Problem: F19 must notify the *newest* `Event`, but cannot reach into F17/F18 internals.
- Simplest alternatives considered: (a) app-owned `LatestValue<Event>` mailbox fanned out by F17's wiring — requires editing another feature's `wire_eventlog` or integrator work, and my read of F17 is not permitted; (b) F19 implements nothing and drops the characteristic — forbidden by §6.2; (c) **poll `IEventStore::readFrom()`** (a `bw_core` contract, sanctioned persistence interface) from the CommsTask tick.
- Trade-offs: (c) adds `IEventStore` to F19's inputs (spec §3 updated) but is contract-only, host-testable with `bw_testkit`'s `InMemoryEventStore`, and needs zero cross-feature edits. CommsTask is shared F19/F20 (runtime §3.2), so concurrent-read concerns with F20 do not arise.
- **Choice:** (c). `EventNotifier` keeps `last_notified_seq`; on first subscription it **baselines** to the current tail (no history burst), then per tick reads forward and notifies only the **newest** event (≤ 1 per `kCommsTickMs`); intermediate events are skipped — completeness is the sync path's job (§6.2 "convenience only"). Suppressed when the negotiated MTU cannot carry 48 B (see `kMaxEventNotifyBytes`).

**D8. Callback architecture (NimBLE → CommsTask)**
- Alternatives: (a) raw bytes queued from the write callback, decoded later in CommsTask; (b) decode in the NimBLE callback (Codec is pure), queue typed actions; (c) do all work in the callback (push commands, feed sync directly).
- Trade-offs: (a) moves bytes twice and defers validation; (c) runs queue pushes and provider reads inside the NimBLE host task — re-entrancy and latency risk; (b) is bounded O(1) work in the callback (parse + one bounded queue push, never blocks), and everything downstream stays testable because `Codec` is host-tested on its own.
- **Choice:** (b). Callbacks: parse via `Codec` → push `BleAction` into `SpscQueue<BleAction, kGattActionQueueLen>` + CCCD/subscription flags. CommsTask drains: `ICommandSink::push`, sync-frame queue for F20, cadence ticks. No provider reads, no mutexes, no allocation in callback context.

**D9. Transport frame delivery model (F19 → F20)**
- Problem: link events and phone control writes must reach F20's engine without F19 knowing the sync algorithm, and without callbacks into the engine from the NimBLE host task.
- Alternatives: (a) push/listener callbacks into F20; (b) engine's `step()` receives frames in its `Input` struct assembled by wiring; (c) **bounded queues inside F19's transport adapter, polled by the engine** (`bw::ISyncTransport::receive()`, §15).
- Trade-offs: (a) re-enters engine code from the host task and complicates host tests; (b) couples F19's task structure to F20's input shape before CR-019 exists; (c) keeps F20's `step()` pure and its fake transport trivial, needs no wiring of engine internals, works whatever CR-019's final signature is (a thin shim adapts push-style ports to the queued source).
- **Choice:** (c), stated fully in §15. **F19 does not draft `ISyncTransport`** — CR-019 (F20) defines it; §15 is F19's binding capability requirement for G1 reconciliation.

**D11. BLE failure handling**
- Problem: NimBLE init/stack failure must not stop local monitoring; restart policy eventually belongs to F26 (wave 6).
- Alternatives: (a) no restart (BLE dead until reboot); (b) F19 restarts itself with local backoff now, behind a replaceable method F26 can own later; (c) wait for F26 — leaves M4 without recovery.
- Trade-offs: (b) duplicates a policy that F26 will later own; (a)/(c) are weaker for a milestone whose contract already promises backoff restart.
- **Choice:** (b): report `FaultCode::BleDown` (already exists — evidence: `bw/fault.h`, F26 §3, severity `Warning`: device still functions) via `IFaultReporter`, restart attempt with exponential backoff (`kBleRestartInitialBackoffMs` → `kBleRestartMaxBackoffMs`), exposed as `BleServer::restart()` so F26 can take over the trigger. **The device keeps sensing, storing and alerting throughout** (`00` non-negotiable: alerting never depends on BLE).

## 7. Module structure
Library `firmware/lib/bw_ble/` (`include/bw/ble/…`, `src/…`):

`CommsTask`: core **0** (DEC-008), name/period from runtime §3.2 (`CommsTask`, event-driven, low-med) — no new registry value needed; stack `kCommsTaskStackBytes`. Loop: wait on action queue with timeout = next cadence deadline; on wake drain actions, run ticks, publish live status/battery/event, never block (every queue wait has a timeout).

## 8. Constants (named, tagged, justified)

| Name | Value | Tag | Why |
|---|---|---|---|
| `kAdvertisingIntervalMs` | 1000 (idle) | `EXPERIMENTAL` | Slower advertising saves power; connect latency ~ 1–2 s |
| `kPreferredMtu` | 185 | `EXPERIMENTAL` | Fewer chunks per sync record batch (NimBLE clamps to the stack max 255 if unsupported) |
| `kStatusNotifyIntervalMs` | 1000 | `EXPERIMENTAL` | Live-view smoothness vs power (D2) |
| `kCommsTickMs` | 1000 | `DERIVED` | = D2's live-status period; one `vTaskDelayUntil` period drives all periodic CommsTask work so `TaskHealth` lateness stays meaningful |
| `kHrNotifyIntervalMs` | 1000 | `EXPERIMENTAL` | **AUD-6:** "≤ 1 Hz HR" enforced in `PublishGate`, not by tick count — CommsTask may wake early on phone writes, so only the gate can make > 1 HR notify/s impossible |
| `kBatteryNotifyIntervalMs` | 60 000 | `EXPERIMENTAL` | Battery % is quasi-static; 1 Hz would be ~60× useless traffic. Tune with measured discharge (F25/F28) |
| `kConfigRefreshIntervalMs` | 5000 | `EXPERIMENTAL` | Config dump buffer refresh while connected (no `onRead` hook, D8); bounds "just changed a setting" staleness |
| `kGattUuidBasePrefix` + `kUuidIdx*` | `4bb00319-8811-4387-95a6-f4bb7838` + `%04x`, indices 0x0000–0x0007 (service, Live Status, HR, Event Notify, Command, Config, Sync Control, Sync Data) | `DERIVED` | D3 scheme; append-only index space allocated by `BLE_GATT.md` §17 table — pinned by host test, never change without a CR |
| `kGattUuidCount` | 8 | `DERIVED` | Size of the D3 index space = number of custom UUIDs (service + 7 characteristics); a new characteristic allocates the next index |
| `kGattUuidStringBytes` | 37 | `DERIVED` | 36-char UUID + NUL; buffer size for `formatGattUuid` |
| `kGattContractVersion` | 1 | `DERIVED` | Version byte in evolving payloads + SW-Rev string; fixed by the contract table, bump only via CR (02 §6.6) |
| `kDefaultAttMtu` | 23 | `HW` | Bluetooth Core Spec: ATT default MTU = 23; floor when a peer negotiates nothing higher |
| `kAttHeaderBytes` | 3 | `HW` | Notification PDU = 1 B opcode + 2 B attribute handle → payload cap = negotiated MTU − 3 |
| `kMaxEventNotifyBytes` | 48 | `DERIVED` | `sizeof(Event)` pinned to 48 B by CR-002's contract test; notify must carry it whole |
| `kMinEventNotifyMtu` | 51 (= 48 + 3) | `DERIVED` | Event-notify floor only: 48 B record + 3 B ATT header, else Event Notify is suppressed (contract §3 edge case). **Not** the sync-engine floor — that is 52 usable B ⇒ ATT MTU ≥ 55 (`bw/sync_transport.h`, CR-019; **AUD-5**) |
| `kMaxConfigDumpBytes` | 512 | `HW` | ATT attribute value cap (BLE Core Spec Vol 3 Part F 3.2.8; NimBLE `BLE_ATT_ATTR_MAX_LEN` = 512 [MEASURED]) — the Config dump buffer is the attribute value, so it can never exceed this; overflow truncates at an entry boundary with `total_count` reported |
| `kCommandBytes` | 9 | `DERIVED` | Command wire image `[type u8][arg0 u32][arg1 u32]`; pinned by `test_codec_command` |
| `kLiveStatusBytes` | 14 | `DERIVED` | Compact Live Status image (contract §4) ≤ the 20 B mandate of `external.md` §6.2 |
| `kHeartRateMaxBytes` | 3 | `HW` | Standard HR Measurement: 1 flag byte + UINT8/UINT16 bpm |
| `kMaxSyncControlWriteBytes` | 32 | `DERIVED` | Sync Control write bound: v1 layouts 8 B/6 B, 4× headroom; 33..252 B writes are *counted* (`writes_rejected`), not ATT-rejected |
| `kMaxConfigWriteBytes` | 64 | `EXPERIMENTAL` | Config write bound: 9 B header + short name identifiers (~55-char names); 65..512 B writes are counted, not ATT-rejected |
| `kMaxActionBytes` | 64 (= `kMaxConfigWriteBytes`) | `DERIVED` | Largest blob any GATT write carries into the action queue = the longest accepted write |
| `kGattActionQueueLen` | 16 | `EXPERIMENTAL` | NimBLE-callback → CommsTask typed actions (writes + CCCD changes); sized for fast manual phone use, push never blocks (Overflow counted if hit) |
| `kSyncFrameQueueLen` | 16 | `EXPERIMENTAL` | F19 → F20 frames; a phone's Request/Ack bursts arrive faster than an engine sleep period only in misuse; drops counted (`drops()`), recovery = phone re-request (F20's protocol) |
| `kBatteryUnknownPercent` | 0xFF | `DERIVED` | Standard Battery Service "unknown" byte; emitted for invalid **or non-finite** readings (AUD-7) — never a fabricated level |
| `kMaxConfigSettings` | 32 | `EXPERIMENTAL` | Upper bound on registered settings per dump/write; `ISettings::list` reports the true total, so a bigger registry truncates honestly |
| `kMaxIdentityStringBytes` | 64 | `DERIVED` | DIS string cap (e.g. full `2026-10-15`-style revision) + NimBLE `max_len` for the four DIS rows |
| `kCommsTaskStackBytes` | 4096 | `EXPERIMENTAL` | ≤ 64 B encode buffers + provider reads + NimBLE calls; verify with `uxTaskGetStackHighWaterMark` (lives in `wire_ble.h`, the composition root) |
| `kBleRestartInitialBackoffMs` | 1000 | `EXPERIMENTAL` | First restart delay after `BleDown`; ×2 growth so a dead radio doesn't spin |
| `kBleRestartMaxBackoffMs` | 60 000 | `EXPERIMENTAL` | Backoff cap; F26 policy supersedes when reliability lands |
| `kDeviceLocalName` | `"Anarolepsy"` | `DERIVED` | Fixed product identity for this build, advertised in scan response (D5); recorded in the contract |

Rows mirror `ble_config.h` (code is truth — audit **AUD-4**); a constant added to `ble_config.h` gets a row here in the same commit.

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
**Host (`pio test -e native`, Unity + `bw_testkit`; new files under `firmware/test/bw_ble/`, 0 exist today → 0 removed):**

| Suite | Covers |
|---|---|
| `test_codec_live_status.cpp` | round trip ≤ 20 B; version byte; every enum value; `in_state_ms` saturation at 0xFFFF s; confidence clamp 0..1; reasons mask full 32 bits |
| `test_codec_hr.cpp` | UINT8 vs UINT16 format flag; refusal (`Err::InvalidArg`) on invalid/non-positive/non-finite bpm; round-trip values |
| `test_codec_command.cpp` | 9 B happy path; wrong length → `InvalidArg`; unknown `CommandType` → `InvalidArg`; `t_ms` stamped from passed `TimeMs`, `source` = Ble |
| `test_codec_config.cpp` | dump from fake `ISettings`: order, entry-boundary truncation + `total_count`; write decode: happy, unknown name, type mismatch, out-of-range → clamp (fake's own clamp honored) |
| `test_codec_sync_control.cpp` | length bounds only (`len == 0` / `> 32` → `InvalidArg`) + opaque passthrough — re-scoped by CR-019 ruling 1 (no Request/Ack parse; that is F20's) |
| `test_codec_battery.cpp` | **AUD-7:** invalid reading → `0xFF`; NaN/±Inf → `0xFF` (never 0 / never a fabricated level); valid values round half-up and clamp to `[0,100]` |
| `test_codec_event.cpp` | 48 B passthrough + `static_assert` contract (sizeof/packing/`kSchemaVersion`) |
| `test_publish_gate.cpp` | **AUD-1 layer 2 / AUD-6:** unencrypted link → every channel `DropInsecure` (counted) or `NotDue`, never `Send`; encrypted link delivers; `NoLink` never counts; HR ≤ 1 Hz under a simulated 100 Hz tick; per-channel cadence independent |
| `test_event_notifier.cpp` | with `InMemoryEventStore`: first-subscribe baseline (no history burst), one notify per new event (newest only), empty store, store error → skip silently, unsubscribe stops reads |
| `test_link_state_tracker.cpp` | Up/Down/MtuChanged/encrypted transitions; `BleLinkState` fields on each; disconnect resets mtu to `kDefaultAttMtu` |
| `test_sync_frame_queue.cpp` | inbound F19→F20 queue: FIFO round trip, length bounds, counted full/oversize drops, generation discard on disconnect |
| `test_sync_transport.cpp` | **M4 bench bugs / D12:** no link source = honest `NotReady`; fake link bound via `bindMonitor()` drives `linkUp()` up AND down; `maxSendBytes()` follows; `send()` attempts the hand-off (recorded) when usable, `Err::NotReady` + zero attempts when not; argument bounds; `Err::Busy` propagates for retry (CR-019); host base `deliver()` never fabricates success |
| `test_ble_contract.cpp` | UUIDs match D3 base+index scheme; full characteristic table (index, props, security flags) pinned — accidental contract drift fails the build |
| `test_wire_ble_compile.cpp` | composition root compiles on host: getters are honest nullptrs before `wire()`, idempotent re-wire, task constants pinned, **`applyBindings()` binds the transport to the link slot — H10 revert guard for M4 bench bug 1, the test fails if that call is removed** (order-dependent — runs last) |

**Device/companion checklist (bench, results in status log):**
1. First bond (Just Works) and bond persistence across power-cycle; reconnect without re-pair.
2. Disconnect (out of range) → advertising resumes at `kAdvertisingIntervalMs` → auto-reconnect; sync resumable (verified with F20 at integration).
3. Encrypted-write enforcement: write Command/Config/Sync Control on an unbonded link → ATT `0x05`/`0x0F`; accepted after bonding.
4. **CCCD enforcement (audit AUD-1):** an unbonded/unencrypted central's write to ANY Client Configuration Descriptor (Live Status, HR, Event Notify, Sync Data, Battery) is refused by the stack with ATT `0x05` (no bond) / `0x0F` (bond, link not encrypted); after bonding + encryption the same subscribe succeeds and notifications flow. Battery *value* reads stay unencrypted.
5. MTU: peer negotiates 185 → `maxSendBytes() = 182`; peer at 23 → 20 and Event Notify suppressed (48 > 20); the sync engine streams only while `maxSendBytes() >= 52` (ATT MTU ≥ 55 — CR-019 engine floor, audit AUD-5).
6. Live Status cadence: phone-side log shows spacing ≥ `kStatusNotifyIntervalMs`.
7. HR: notifications only while F02 reports `hr_valid`; none when invalid/not worn; **notify spacing ≥ `kHrNotifyIntervalMs` (1 s) even when CommsTask wakes early on phone writes (audit AUD-6).**
8. **Alert independence:** with the phone disconnected/BLE off, alerts still fire (non-negotiable `00` §7.1).
9. Stack-failure drill (or developer bench action with F26): device continues locally, `BleDown` reported, restart with backoff.

## 10. Failure behavior
- NimBLE init/stack failure ⇒ report `FaultCode::BleDown` (`Warning`), device continues sensing/storage/alerting locally; restart with backoff (D11; F26 takes over policy later).
- Queue-full in callback ⇒ drop counted (`Err::Overflow`), never block the host task.
- `ICommandSink` refused (queue full) → dropped + counted in `BleStats.command_rejected` (NimBLE 2.5.1 [MEASURED] write callback cannot return an ATT error); the phone may retry; no silent loss - diagnostics carry the refusal.
- No snapshot / invalid HR / invalid battery → explicit "no data": empty Live Status read, no HR notification, battery `0xFF` (also when `percent_estimate` is NaN/±Inf — AUD-7) - never fabricated values.
- Link connected but not encrypted → `PublishGate` suppresses every notification (`PublishVerdict::DropInsecure`) and counts it in `BleStats::notifications_dropped`; nothing is queued for later and the cadence anchor advances, so an unencrypted link cannot busy-loop the counter (audit AUD-1).
- Sync transport `Err::NotReady`/`Busy` ⇒ F20's flow control + resumability (its contract); F19 queues nothing across a disconnect.

## 11. Budget
| Item | Budget | How measured |
|---|---|---|
| `bw_ble` static RAM | ≤ 12 KB `[EXPERIMENTAL]` | `.bss + .data` of the lib (link map) |
| `CommsTask` stack | 4096 B `[EXPERIMENTAL]` | `uxTaskGetStackHighWaterMark` after 10 min run |
| NimBLE host task stack + host pool | part of the total below | watermark + `ESP.getMinFreeHeap()` delta before/after `BleServer::start()` |
| **Total BLE incl. stack** | **≤ 60 KB** `[EXPERIMENTAL]` (retained from §11 v1) | as above, recorded at DoD |
| Flash | ≤ 200 KB incl. NimBLE `[EXPERIMENTAL]`; measured **190 KB** on 2026-10-15 (631 073 B vs G0 base 436 537 B = +194 536 B), dominated by NimBLE | `pio run -e esp32dev` size output (H3); delta vs G0 base `5085955`; `firmware.map` breakdown: NimBLE stack 189 173 B (host 37 029 + controller 125 308 + PHY/NVS/rtc/stdc++ support) · `bw_ble` + `wire_ble` 3 830 B · link/align residual 1 596 B |
| CPU | event-driven + 1 Hz tick; ≤ 1 % of core 0 average `[EXPERIMENTAL]` | `TaskHealth` lateness of `CommsTask` |
| Heap after startup | 0 allocations in F19 paths (NimBLE's init-time pool excepted — allocated before `run()` finishes) | allocation hook where practical + review |

## 12. Out of scope
- Wi-Fi.
- Sync algorithm (F20) — F19 only transports opaque bytes and control frames.
- Passkey/pairing-window pairing (future CR; `PairingManager` keeps it cheap).
- Companion app behavior (F21).

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- `BLE_GATT.md` complete and matches implementation (§17; host test `test_ble_contract.cpp` pins the table).

---
*(plan continues below — §15 transport surface for F20, §16 CR-018 draft, §17 contract completion plan, §18 wiring plan, §19 risks)*

## 15. Transport surface F19 offers F20 (implemented — CR-019 Accepted 2026-10-15)
**Gate note:** CR-019 defined `bw::ISyncTransport` in `bw_core` (`bw/sync_transport.h`) and it is **applied**; F19's device-only `SyncTransport` (`bw/ble/sync_transport.h`) implements it. This section records the implemented surface for G1 reconciliation (audit AUD-3 replaced the earlier PLAN-stage sketch — `SyncControlFrame`/`LinkEvent` as public API and the `poll*` methods do **not** exist; `LinkEvent` survives only as an internal `bw_ble` type feeding `LinkStateTracker`).

```cpp
// namespace bw — from bw_core/include/bw/sync_transport.h (CR-019, locked)
enum class SyncChannel : uint8_t { Control = 0, Data = 1 };   // append-only
constexpr uint16_t kMaxSyncFrameBytes = 252;                  // 255 MTU - 3 B ATT header

class ISyncTransport {
 public:
    // device -> phone. Requires 0 < len <= kMaxSyncFrameBytes.
    //  Ok            frame accepted (Data: queued for notification;
    //                               Control: queued for indication)
    //  Err::NotReady link down (linkUp() false); nothing sent; retry later
    //  Err::Busy     link congested (e.g. previous indication unconfirmed);
    //                nothing consumed; retry on a later pump
    //  Err::InvalidArg len == 0 or len > kMaxSyncFrameBytes (caller bug)
    virtual Result<void> send(SyncChannel ch, const uint8_t* data, uint16_t len) = 0;

    // phone -> device: copies the OLDEST pending inbound frame (Sync Control
    // writes only — Sync Data is device->phone) and its channel.
    //  Ok(0)         nothing pending (also right after a disconnect:
    //                inbound frames of a dropped link are discarded)
    //  Ok(n > 0)     one frame of n bytes delivered
    //  Err::Overflow pending frame larger than `cap` (callers pass >= 252)
    virtual Result<size_t> receive(SyncChannel* ch_out, uint8_t* out, size_t cap) = 0;

    // connected + encrypted + peer enabled Sync Data notifications (CR-019);
    // false before the first Up event. Polled every pump.
    virtual bool linkUp() const = 0;

    // ATT MTU - 3, clamped to [0, kMaxSyncFrameBytes], 0 without a link.
    // [MEASURED] engine floor: >= 52 usable B (one 48 B record + 4 B framing)
    // => ATT MTU >= 55 (SYNC_PROTOCOL.md §3.3 / CR-019; audit AUD-5 — the
    // 51 B figure is only the Event-notify floor).
    virtual uint16_t maxSendBytes() const = 0;
};
// Implemented: bw::ble::SyncTransport (pure gate over the ISyncLink seam +
// device-only deliver(), ble_server graph; spec D12 - the gate and the
// wiring bind are host-tested, the radio hand-off is device code).
// send() additionally requires linkUsable() (encrypted + subscribed) and maps
// stack refusal -> Err::Busy; nothing is ever queued across a disconnect.
// Exposed to F20 by the composition root as bw::ISyncTransport* (§18).
```
- **Not F19's surface:** `bw::SyncStatus` / `bw::ISyncStatusProvider` (CR-019 part B) belong to **F20's** status producer; F19 neither implements nor polls them.
- Threading: `send`/`receive`/`maxSendBytes` are called from the task driving F20's engine (CommsTask per runtime §3.2) — exactly one caller, per the `bw_core` contract; internally non-blocking.
- Inbound control writes flow through `SyncFrameQueue` (D9) with drop counters (`drops()`, `oversizeDrops()`); recovery semantics belong to `SYNC_PROTOCOL.md`.
- Chunk **payload framing** (chunk counter, CRC — `BLE_GATT.md` Sync Data row) is entirely F20's; F19 transmits opaque bytes capped by `maxSendBytes()`.

## 17. `BLE_GATT.md` completion plan (status STUB → complete at IMPLEMENT)
Sections per the stub: purpose/scope · exact byte layouts (versioning, endianness) · error/edge-case behavior · test vectors · version history. Planned content:

**Services/characteristic table (pin exact UUIDs):**
| Service | UUID | Characteristic | UUID | Props | Sec |
|---|---|---|---|---|---|
| Device Information | `0x180A` | Firmware Rev `0x2A26` / HW Rev `0x2A27` / Software Rev `0x2A28` (= schema versions, format `ev1;fs…;g1`) / Serial `0x2A25` (= device_id, 12-hex lowercase) | standard | R | no `[ASSUMED]` |
| Battery | `0x180F` | Battery Level `0x2A19` (0–100, `0xFF` unknown) | standard | R, N | no `[ASSUMED]` |
| Bracelet (custom) | `4bb00319-8811-4387-95a6-f4bb78380000` | Live Status | `…78380001` | R, N | encrypted |
| | | Heart Rate (0x2A37 format) | `…78380002` | N | encrypted |
| | | Event Notify (48 B `Event`) | `…78380003` | N | encrypted |
| | | Command (9 B) | `…78380004` | W | encrypted |
| | | Config (name-keyed, D6) | `…78380005` | R, W | encrypted |
| | | Sync Control (opaque frames W, indications I; F20 parses) | `.78380006` | W, I | encrypted |
| | | Sync Data (opaque chunks) | `…78380007` | N | encrypted |

**Payload layouts (LE):** Live Status `[ver][alertness][wear][activity][quality][mode][power][in_state_s u16 sat][confidence_pct u8][reasons u32]` = 14 B → 20 B (6.2) → Command `[type u8][arg0 u32][arg1 u32]` = 9 B (`t_ms`/`source` stamped by device) → Config read `[ver][total_keys u16][entries: type u8, value u32, name_len u16, name]`, write `[ver][type u8][value u32][name_len u16][name]` → Sync Control = dumb byte pipe, write bound 0 < len <= 32 (CR-019 ruling 1: F19 does NOT parse Request/Ack/opcode; framing owned by F20 `SYNC_PROTOCOL.md`) → Sync Data = opaque, size = MTU-3, max 252 B/frame (framing: F20) → Event Notify = canonical 48 B `bw::Event` verbatim (CR-019 ruling 3) → GAP adv payload (D5).

**Error/edge cases:** full D6 error map (two classes: real ATT `0x0D`/`0x05`/`0x0F` vs counted semantic rejects) → disconnect mid-notify (nothing buffered, F20 re-requests) → MTU < 51 → Event Notify suppressed → no snapshot → empty read → config truncation contract → unknown opcode/type → rejection, never guessing → bonding restored from NVS after reboot.

**Test vectors (>= 5 hex examples shared by firmware host tests and F21, all in `BLE_GATT.md` 6):** Live Status of a fixed sample snapshot; Command `MarkEvent` write; HR measurement bytes for 72 bpm (uint8) and 300 bpm (uint16); Config write of a Bool setting; Config dump of 2 named settings; Event 48 B reference record (first 16 B); UUID base+index scheme.

## 18. Wiring plan (`firmware/src/app/wiring/wire_ble.h/.cpp`)
- Constructs `BleServer` + `SyncTransport` with constructor-injected: `IStateProvider*`, `IFeatureProvider*`, `IBatteryMonitor*`, `ISettings*`, `ICommandSink*`, `IEventStore*`, `IFaultReporter*`, `DeviceIdentity{fw, hw, schema_versions}` (app-supplied build strings — the app layer may include any feature's constants), `ble::Config`.
- Creates `CommsTask` on core 0, name `CommsTask`, stack `kCommsTaskStackBytes`, low-med priority (runtime §3.2 — no new registry value); the task **is** the input pump (H7): queue-wait with timeout = next cadence deadline, drain NimBLE action queue every wake.
- Exposes getters consumed by other wirings at integration (I do not edit their files): `bw::ISyncTransport* syncTransport()` (→ F20), `bw::IBleLinkProvider* linkProvider()` (→ F23/F12), `BleServer* server()` (→ F26 restart policy later).
- `app.cpp` call-site is the integrator's file (not mine); flagged for H7.

## 19. Risks / assumptions (also reported)
- NimBLE 2.5.1 API assumptions (§13) — device-glue level; two failed attempts ⇒ `BLOCKED` per stuck rule.
- G1 reconciliation risk: CR-019 signature vs §15 (push-style ports need a thin adapter shim — accounted for in D9).
- CR-018 must be decided at a G0 before IMPLEMENT wiring of F23's `ble_connected` (F19 itself compiles without it — `BleLinkState` ships in `bw_ble` types first? **No** — to avoid a second contract edit, F19 implements `IBleLinkProvider` only after CR-018 is applied; until then the provider getter is not compiled. Not a blocker for F19's own DoD, only for H8 with real F23).
- Budgets are estimates until measured (§11); exceeding `bw_ble` static 12 KB or stack 4096 B is re-tuned and re-recorded, not silently raised.

---
