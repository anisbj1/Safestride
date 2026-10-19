# BLE GATT contract (BLE_GATT.md)

> **Status: v1.1 (2026-10-16; v1 = 2026-10-15), Accepted for implementation - matches `firmware/lib/bw_ble` byte-for-byte.** Owner: **F19**. The firmware-side source of truth for everything below is `bw/ble/gatt_table.h` + `bw/ble/ble_config.h`, pinned host-side by `test/bw_ble/test_bw_ble/test_ble_contract.cpp`; a doc/code drift fails the host suite (H9). Changes after v1 go through a change request (02 6.6); v1.1 is the integration-approved audit clarification recorded in section 7.

## 1. Purpose and scope

This contract defines the complete BLE surface a companion app sees when connected to the bracelet: GAP identity, services, characteristics, payload byte layouts, and error/edge-case behavior. It is written from the DEVICE's point of view; the companion (F21) implements the mirror side.

Out of scope here: the sync protocol's Request/Ack semantics (F20, `SYNC_PROTOCOL.md`) and alert acknowledgment flows (F15). F19 is a **dumb byte pipe** for sync frames (CR-019 ruling 1): it forwards opaque bytes over Sync Control/Sync Data and enforces only length bounds.

Versioning rule: every evolving custom payload (Live Status, Config, Sync Control writes) starts with a version byte, currently `1` (`kGattContractVersion`). A version bump is a contract change and requires a CR; readers reject unknown version bytes (never guess a layout).

Endianness: **little-endian** for all multi-byte integers (spec D6).

## 2. GAP identity and advertising

| Item | Value |
| --- | --- |
| Device local name | `Anarolepsy` (complete name in the **scan response**; the ADV payload carries flags + the 128-bit service UUID only, keeping the 31-byte ADV limit comfortable) |
| Service UUID in ADV | `4bb00319-8811-4387-95a6-f4bb78380000` (companion filters by this) |
| Advertising interval | 1000 ms (`kAdvertisingIntervalMs`, [EXPERIMENTAL]) |
| Preferred ATT MTU | 185 (`kPreferredMtu`, [EXPERIMENTAL]; stack clamps to its max 255 if lower). **The device sends `ATT_MTU_REQ` itself** when a link comes up (CR-026) — a central that never requests still ends up negotiated; see §4.7 |
| Security | Bonding + LE Secure Connections, MITM off (Just Works), IO cap NoInputNoOutput. Bonds persist in NVS and are restored on reboot: a reconnecting phone needs no re-pairing |
| device_id | the device's own BLE address, lowercase 12-hex, no separators; readable as the Serial Number characteristic |

UUID scheme (D3): one random 128-bit base `4bb00319-8811-4387-95a6-f4bb7838` + a 16-bit index in the last four hex digits (SIG-style). The index space is **append-only**: published indices are never renumbered; index `0x0008+` is unallocated and rejected by the firmware (`formatGattUuid`).

## 3. Services and characteristics

12 characteristic rows across 3 services. "Encrypted" means the row requires a bonded + encrypted link (enforced by the stack via `READ_ENC`/`WRITE_ENC` property flags: an unbonded access is refused with ATT `0x05`/`0x0F` before any application callback runs). "max_len" is the NimBLE attribute value cap set at creation: the stack refuses any **longer** write with ATT `0x0D` (Invalid Attribute Value Length) - the only genuine length ATT error the NimBLE 2.5.1 callback surface can still produce.

**CCCD permissions (audit AUD-1, added 2026-10-16):** the "Encrypted" column above governs the characteristic **value**. The Client Configuration Descriptor (subscription) of **every** notify/indicate characteristic — Live Status, Heart Rate, Event Notify, Battery Level, Sync Control, Sync Data — additionally requires an **encrypted** link to write: `GattDatabase::create()` calls `ble_gatts_set_clt_cfg_perm_flags(BLE_ATT_F_READ | BLE_ATT_F_WRITE | BLE_ATT_F_WRITE_ENC)` before `server->start()` (NimBLE 2.5.1 [VERIFIED]: default is `READ|WRITE`, applied when the CCCD is registered). Consequences a companion must expect:

- An unbonded or unencrypted central cannot subscribe to ANY notification/indication — its CCCD write is refused with ATT `0x05` (no bond) / `0x0F` (bond, link not encrypted).
- **Battery Level is the one interaction between the two rules:** the *value* stays readable unencrypted (`0x180F` standard row), but *subscribing* to Battery notifications now requires an encrypted link like every other CCCD.
- Duty cycle: no notification/indication of any kind is emitted over an unencrypted link (`PublishGate`, double enforcement on the send side; suppressed sends are counted in `notifications_dropped`, section 5).

| # | Characteristic | UUID | Properties | Encrypted | max_len | Payload |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | Firmware Revision | `0x2A26` (DIS) | Read | no | 64 | ASCII build string |
| 1 | Hardware Revision | `0x2A27` (DIS) | Read | no | 64 | ASCII hw revision |
| 2 | Software Revision | `0x2A28` (DIS) | Read | no | 64 | ASCII schema-versions string |
| 3 | Serial Number | `0x2A25` (DIS) | Read | no | 64 | device_id (12 lowercase hex) |
| 4 | Battery Level | `0x2A19` | Read, Notify | no | 1 | uint8 percent, `0xFF` = unknown |
| 5 | Live Status | `...78380001` | Read, Notify | **yes** | 14 | section 4.1 |
| 6 | Heart Rate | `...78380002` | Notify | **yes** | 3 | section 4.2 (standard `0x2A37` image) |
| 7 | Event Notify | `...78380003` | Notify | **yes** | 48 | canonical `bw::Event`, section 4.6 |
| 8 | Command | `...78380004` | Write | **yes** | 9 | section 4.3 |
| 9 | Config | `...78380005` | Read, Write | **yes** | 512 | section 4.4 |
| 10 | Sync Control | `...78380006` | Write, Indicate | **yes** | 252 | opaque F20 frames, section 4.7 |
| 11 | Sync Data | `...78380007` | Notify | **yes** | 252 | opaque F20 frames, section 4.7 |

Services: Device Information `0x180A` (rows 0-3), Battery `0x180F` (row 4), custom service "Bracelet" = `4bb00319-8811-4387-95a6-f4bb78380000` (rows 5-11).

`[ASSUMED]` (spec 13): DIS and Battery are readable **unencrypted**; external.md 6.2 mandates encryption for "state, events or commands", and device facts are not health records. Everything else is encrypted.

## 4. Payload layouts

All integers little-endian. `u8`/`u16`/`u32`/`u64` are unsigned.

### 4.1 Live Status (14 bytes, version 1)

Sent as Read and Notify, at most once per second while a peer is subscribed.

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0 | 1 | `ver` | = 1 |
| 1 | 1 | `alertness` | `AlertnessState`: 0 Unknown, 1 NotWorn, 2 Awake, 3 Drowsy, 4 HighDrowsinessRisk, 5 PossibleSleep |
| 2 | 1 | `wear` | `WearState`: 0 Uncertain, 1 Worn, 2 NotWorn |
| 3 | 1 | `activity` | `ActivityContext`: 0 Unknown, 1 Stationary, 2 Sitting, 3 Standing, 4 Walking, 5 Running, 6 LyingResting, 7 InactiveProlonged |
| 4 | 1 | `quality` | worst-of IMU/PPG `DataQuality`: 0 Good, 1 Degraded, 2 Insufficient |
| 5 | 1 | `mode` | `OperatingMode`: 0 Day, 1 Night, 2 Nap, 3 Calibration, 4 Research |
| 6 | 1 | `power` | `PowerProfile`: 0 Normal, 1 LowPower |
| 7 | 2 | `in_state_s` | seconds in the current state, saturated at 65535 |
| 9 | 1 | `confidence_pct` | heuristic confidence 0-100 (clamped from the internal 0..1 float); detection confidence, NOT clinical certainty |
| 10 | 4 | `reasons` | full 32-bit `ReasonMask` of active reason codes |

The 14-byte image stays under external.md 6.2's 20-byte cap (pinned by `test_contract_live_status_fits_mandate`).

### 4.2 Heart Rate (standard `0x2A37` image, 2-3 bytes)

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0 | 1 | `flags` | bit0 = 1: bpm is uint16; bit0 = 0: bpm is uint8. No other bits are ever set |
| 1 | 1 or 2 | `bpm` | beats per minute, LE when 16-bit |

Notifications stop while `hr_valid == false` (not worn / poor signal): the standard profile has no honest "invalid" byte, so silence is the answer. A read is not offered (Notify-only row).

Cadence: **at most one HR notification per second** (`kHrNotifyIntervalMs` = 1000 ms, enforced by `PublishGate` at the sample site — audit AUD-6). The device task may wake more often than once per second (phone writes wake it early), which is exactly why the cap sits where the sample is taken: the companion can rely on HR notify spacing >= 1 s regardless of the task wake rate.

### 4.3 Command (9 bytes, written by the phone)

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0 | 1 | `type` | `CommandType`: 0 AcknowledgeAlert, 1 CancelAlert, 2 NextScreen, 3 OpenMenu, 4 MarkEvent, 5 ConfirmEvent, 6 RejectEvent, 7 StartNap, 8 CancelNap, 9 SetMode, 10 SetSetting, 11 SetTime, ... (append-only enum) |
| 1 | 4 | `arg0` | LE |
| 5 | 4 | `arg1` | LE |

`t_ms` and `source` are stamped by the DEVICE from its own clock (`source = 1`, BLE): a phone cannot spoof either (D6, data_decision.md 4.6). Unknown `type` values are rejected, never coerced to the nearest known command.

### 4.4 Config

**READ - dump** (version 1):

| Offset | Size | Field | Notes |
| --- | --- | --- | --- |
| 0 | 1 | `ver` | = 1 |
| 1 | 2 | `total_keys` | u16: the TRUE number of registered settings, even when truncated below |
| 3 | ... | `entries` | zero or more entries, in registration order |

Entry: `type u8` (0 Bool, 1 Int, 2 Float), `value u32` (Bool: 0/1; Int: two's-complement; Float: IEEE-754 binary32 bits), `name_len u16`, `name` bytes (ASCII). The dump truncates at an **entry boundary** when it exceeds the 512-byte attribute cap; `total_keys` still reports the full count so the companion knows more entries exist (snprintf convention).

**WRITE** (version 1): `ver u8`, `type u8`, `value u32`, `name_len u16`, `name` bytes - same entry layout with the version byte in front. The device applies it via `ISettings::set`, which **clamps** out-of-range values to the registered bounds (services.md 5: clamp, never reject) and reports success. An unknown name, a type mismatch with the registered setting, a malformed length or an unknown version byte is refused and counted (no ATT error - see section 5).

### 4.5 Battery Level

1 byte, standard profile: 0-100 percent, `0xFF` (255) = unknown. The device sends `0xFF` whenever the reading is not trustworthy — `IBatteryMonitor::read().valid == false` **or** its `percent_estimate` is NaN or +/-infinity (audit AUD-7; `encodeBatteryLevel` in `codec`, host-pinned by `test_battery_nan_and_infinity_are_unknown`). A valid estimate is clamped to [0, 100] and rounded half-up; it is never a fabricated level. Subscribing to this row's notifications additionally requires an encrypted link (section 3, CCCD rule); reading the value does not.

### 4.6 Event Notify (48 bytes, canonical `bw::Event`)

The characteristic carries the **canonical 48-byte Event image verbatim** (CR-002 / data_decision.md 4.5): no second encoding exists (CR-019 ruling 3). Field order and offsets are pinned by the `bw_core` contract test (`contract_checks.h`):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 8 | `t_start_ms` (u64 monotonic within boot_id) |
| 8 | 4 | `seq` (u32, assigned by F17, monotonic per device, never reused) |
| 12 | 4 | `boot_id` (u32, increments each boot) |
| 16 | 4 | `duration_ms` (u32) |
| 20 | 4 | `t_start_unix` (u32 seconds; 0 = unknown) |
| 24 | 4 | `confidence` (IEEE-754 binary32, 0..1) |
| 28 | 4 | `reasons` (u32 ReasonMask) |
| 32 | 8 | `payload` (type-specific small payload) |
| 40 | 1 | `quality` (SourceQuality) |
| 42 | 1 | `type` (EventType) |
| 43 | 1 | `label` (UserLabel) |
| 44 | 1 | `context` (ActivityContext) |
| 45 | 1 | `alertness_at_start` (AlertnessState) |
| 46 | 1 | `detector_id` |
| 47 | 1 | `schema_version` (= 1) |

Delivery is **newest-only**: if the phone drains slower than events arrive, intermediate records are skipped (counted in diagnostics); `seq` gaps are expected and are how F20 requests history. A 48-byte record needs an ATT MTU >= 51 (48 + 3-byte ATT header); below that the characteristic is **suppressed entirely** - never truncated (section 5).

### 4.7 Sync Control / Sync Data (opaque F20 frames)

F19 forwards bytes verbatim in both directions; Request/Ack framing, opcodes, CRCs and protocol versioning belong to F20 (`SYNC_PROTOCOL.md`, CR-019). F19's only rule is the Sync Control **write length bound**: `0 < len <= 32` (v1's Request is 8 B and Ack 6 B; 32 B is headroom for evolution under the same version byte). Writes outside that bound are **counted** (`writes_rejected`, section 5) and dropped — the attribute's max_len is 252 B, so a 33..252 B write succeeds at ATT level and is refused in the callback; only writes > 252 B are a real ATT `0x0D`. Device-to-phone frames travel as Indications on Sync Control and Notifications on Sync Data, up to `kMaxSyncFrameBytes` = 252 bytes each (ATT MTU 255 - 3-byte header). A sync frame is never split across characteristics.

**MTU floors (audit AUD-5):** two different numbers, easy to confuse. `51 B` ATT MTU is the **Event-notify** floor (48 B record + 3 B header) — below it Event Notify is suppressed. The **sync engine** streams only while `maxSendBytes() >= 52` usable bytes, i.e. ATT MTU >= 55 (one 48 B record + 4 B framing; `bw/sync_transport.h` / `SYNC_PROTOCOL.md` §3.3 / CR-019). Between 51 and 54 the characteristics are all enabled but the engine will not start a stream.

**Who initiates the exchange (CR-026):** the *device* sends `ATT_MTU_REQ` on connection (`ConnectionMonitor::onConnect` → `ble_gattc_exchange_mtu`, preferred = `kPreferredMtu`); the peer's `MTU_RSP` arrives as `BLE_GAP_EVENT_MTU` → `onMTUChange` → `BleLinkState.mtu` (CR-018) and the floors above then apply unchanged. A failed or unanswered exchange leaves the link at `kDefaultAttMtu` = 23 — exactly the fail-closed state above, never a fabricated pass. Rationale: no tested central initiates (Windows/bleak exposes no `ATT_MTU_REQ` API at all — measured 23; the W4 nRF Connect session sent none either), so a passive peripheral deadlocked F20's streaming leg at 23 on every PC-side run (queue 10).

## 5. Error and edge-case behavior

NimBLE 2.5.1 `[MEASURED]`: the write callback runs **after** the stack has stored the value and offers no reject path (the 1.x `setWriteFlag`/`setWriteErrorCode` pair is gone), and reads cannot fail from the callback either. Therefore F19 distinguishes two error classes:

**Real ATT errors** (refused by the stack before application code):

| Condition | ATT error |
| --- | --- |
| Write longer than the row's max_len (Command > 9, Config > 512, Sync Control > 252) | `0x0D` Invalid Attribute Value Length |
| Read/write of an encrypted row on an unbonded or unencrypted link | `0x05` Insufficient Encryption / `0x0F` Insufficient Authentication |
| **CCCD (subscription) write of ANY notify/indicate row on an unencrypted link** (section 3) | `0x05` (no bond) / `0x0F` (bond, link not encrypted) |

**Diagnostics counters — complete list (audit AUD-2).** Everything F19 can lose is counted; the condition→counter mapping below matches `BleStats` (`bw/ble/ble_server.h`) and the two `SyncFrameQueue` accessors **exactly, name for name** — a counter added in code gets a row here in the same commit. `BleServer::stats()` merges the monitor/notifier/gate counters; diagnostics (H-card) read it.

| Counter | Source | Conditions counted (each = dropped, never blocked, no ATT error) |
| --- | --- | --- |
| `restarts` | `BleStats` | one per failed `BleServer::start()` attempt = one backoff episode (D11) |
| `command_rejected` | `BleStats` | Command image with unknown `type` or malformed length reaching the task; `ICommandSink::push` full / refused |
| `config_rejected` | `BleStats` | Config write: malformed length, unknown version byte, unknown setting name, type mismatch; `ISettings` unavailable |
| `writes_rejected` | `ConnectionStats` (via `BleStats`) | callback-side length refusals within the attribute cap: short/wrong-length Command (!= 9), Config write < 9 B or 65..512 B, Sync Control write 33..252 B |
| `actions_dropped` | `ConnectionStats` (via `BleStats`) | action queue full (`kGattActionQueueLen`): a write or CCCD change was received at ATT level but its queue push was refused |
| `store_errors` | `EventNotifier` (via `BleStats`) | `IEventStore` read failures while notifying events — the event is skipped, sync (F20) remains the completeness path |
| `notifications_dropped` | `PublishGate` (via `BleStats`) | a **due** notification on a connected-but-unencrypted link, suppressed by the gate (audit AUD-1); one per interval per channel at most, never queued |
| `drops()` | `SyncFrameQueue` (inbound, transport-side; NOT part of `BleStats`) | inbound frame push refused because the queue is full (`kSyncFrameQueueLen`) — recovery is a phone re-request |
| `oversizeDrops()` | `SyncFrameQueue` (inbound, transport-side; NOT part of `BleStats`) | inbound frame longer than the receive cap was discarded on `receive()` |

**"No data" is explicit, never fabricated** (D6, 00 1):

| Condition | Behavior |
| --- | --- |
| Live Status read before the first `StateSnapshot` | **empty (0-byte) read** - the characteristic value stays empty until the first real snapshot; `[MEASURED]` the 2.5.1 read callback cannot return ATT `0x84`, so an empty value is the honest signal. After the first snapshot the LAST value persists (including across a peer disconnect) |
| HR while `hr_valid == false` | no notification (silence = unknown; the standard profile has no invalid byte) |
| Battery invalid reading **or non-finite `percent_estimate` (NaN/±Inf)** | `0xFF` — bad data is not a percentage (audit AUD-7); host-pinned by `test_codec_battery.cpp` |
| Event Notify with ATT MTU < 51 | characteristic suppressed: no notifications at all, never a truncated 48-byte record |
| Link connected but not encrypted | every notification suppressed at the gate (`PublishVerdict::DropInsecure`), counted in `notifications_dropped`; nothing queued (audit AUD-1) |

**Link lifecycle:**

| Condition | Behavior |
| --- | --- |
| Disconnect | nothing is buffered across a dropped link: inbound sync queues are discarded (generation bump) and per-link state resets. `ISyncTransport::receive()` reports "nothing pending" (CR-019). Outbound notifications are never queued for a down link |
| Reconnect | advertising resumes automatically; the phone must share an existing bond (Just Works, restored from NVS) to reach encrypted rows |
| BLE radio failure | the device keeps sensing, storing and alerting: **immediate alerting never depends on BLE/phone** (00 7.1). The stack is restarted with exponential backoff (1 s doubling to a 60 s cap) and one `BleDown` fault report per down episode |
| CCCD off | notify loops no-op; Event-Notify store reads stop entirely on unsubscribe |

## 6. Test vectors

Host-pinned in `firmware/test/bw_ble/test_bw_ble/`; companion tests should assert these exact bytes.

**V1 - Live Status** (`test_live_status_round_trip`): Drowsy/Worn/Sitting, quality Degraded, Day/Normal, in_state 45 s, confidence 0.5, reasons `0x12345678`:

```
01 03 01 02 01 00 00 2D 00 32 78 56 34 12
```

**V2 - Heart Rate uint8 72 bpm**: `00 48`  - **uint16 300 bpm**: `01 2C 01`

**V3 - Command** MarkEvent, arg0 `0x01020304`, arg1 `0xAABBCCDD` (`test_command_happy_path`):

```
04 04 03 02 01 DD CC BB AA
```

**V4 - Config WRITE** set Bool `alert_enabled` = true (`test_config_write_happy_paths`):

```
01 00 01 00 00 00 0D 00 61 6C 65 72 74 5F 65 6E 61 62 6C 65 64
```
(`ver=01`, `type=Bool=00`, `value=1` LE, `name_len=13` LE, `"alert_enabled"`)

**V5 - Config READ dump** with `alert_enabled` (Bool, default false) and `drowsy_thr` (Float 0.5f) registered (`test_config_dump_order_and_totals`):

```
01 02 00 00 00 00 00 0D 00 61 6C 65 72 74 5F 65 6E 61 62 6C 65 64
   02 00 00 00 00 3F 0A 00 64 72 6F 77 73 79 5F 74 68 72
```
(`ver=01`, `total=2` LE, entry1 Bool 0 `"alert_enabled"`, entry2 Float 0.5f = `00 00 00 3F` LE `"drowsy_thr"`; shown wrapped at the entry boundary)

**V6 - Event Notify** first 16 bytes of a record with `t_start_ms=1000`, `seq=42`, `boot_id=7`:

```
E8 03 00 00 00 00 00 00 2A 00 00 00 07 00 00 00
```
(full 48-byte layout in section 4.6; the complete image is pinned by the `bw_core` contract test)

**V7 - UUID scheme** (`test_contract_uuid_scheme`): index `0x0005` -> `4bb00319-8811-4387-95a6-f4bb78380005`; index `0x0008` is rejected.

## 7. Version history

| Version | Date | Change |
| --- | --- | --- |
| v1.2 | 2026-10-17 | **CR-026 clarification (no payload/layout/field change):** documented that the *device* sends `ATT_MTU_REQ` on connection (§2 preferred-MTU row, new §4.7 "who initiates" paragraph) — no tested central ever initiates; MTU floors, suppression and `mtu_too_low` behavior unchanged |
| v1.1 | 2026-10-16 | **Audit-driven clarification (findings AUD-1/2/5/6/7, integration-approved FIX; documentation of behavior fixes, no payload layout change, no CR):** §3 added the CCCD encrypted-write rule (all subscription descriptors require an encrypted link; Battery VALUE still readable unencrypted); §5 replaced the partial "counted semantic rejects" list with the **complete diagnostics counter table** (9 counters, exact code names), added the CCCD ATT-error row and the battery/`notifications_dropped` "no data" rows; §4.2 documents the 1 Hz HR cap, §4.5 the NaN/±Inf → `0xFF` rule, §4.7 the two MTU floors (51 B Event vs 52/55 B sync engine) |
| v1 | 2026-10-15 | Initial contract: 3 services / 12 characteristics, UUID base+index scheme, Live Status 14 B, Command 9 B, Config dump/write, canonical 48 B Event passthrough, sync byte pipes (CR-018/CR-019 G1 rulings), measured NimBLE 2.5.1 error map (real ATT errors vs counted semantic rejects) |
