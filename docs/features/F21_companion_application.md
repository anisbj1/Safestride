# F21 — Companion application

> Track **D** · Milestone **M4** · Runs on **companion** · Contract changes only via change request.

## 1. Responsibility

Local-first **Python desktop companion** (bleak + SQLite): pairing/bonding, live status (device state, battery, link/sync status), a read-only synced event timeline, sync control and time sync. No server, no third-party sends, no duplication of embedded decision logic.

**W5 scope ruling (developer, 2026-10-17):** minimal per DEC-005 "nothing fancy" — the items above **only**. Trends, settings, user labels/feedback, diagnostics and export screens, and the nap/history/explain features are out of this wave (their companion halves live in the excluded paths of §2 / later waves). The app is **never** required for immediate alerting (00 §7.1): it observes, the bracelet decides and alerts on its own.

## 3. Inputs and outputs
**Inputs (consumed contracts — read-only)**
- `docs/contracts/BLE_GATT.md` v1.1 — GATT surface, byte layouts, MTU floors (51/55), error map, diagnostic semantics
- `docs/contracts/SYNC_PROTOCOL.md` v1 — CURSOR/STATUS/CHUNK frames, 48 B records, ack/idempotency semantics, §4.5 wall-time rule
- `docs/interfaces/data_decision.md` §4.5 (`Event`, `StateSnapshot`) and §4.6 (`Command`), `external.md` §6.1/§6.2, `core.md` conventions
- F20's phone-side sync client, to land in `companion/lib/sync/**` against the `SyncPort` seam F21 defines (§7)

**Outputs (produced)**
- Local SQLite DB (schema v1, §7.3) + read-only timeline over it
- Console app + bench CLI (`anarolepsy-bench`, doubling as the M4 bench central)
- Seams for the later companion halves: `GattTransport` (+ fake), `SyncPort`, `EventRepository`, codecs

## 4. Dependencies
- Contracts above; no firmware code; **no `bw_core` compile dependency** (F21 is a host app and edits no firmware file).
- Python **>= 3.11** (repo convention: `tools/pyproject.toml`, `ml/pyproject.toml`).
- Runtime deps: **`bleak` only**; stdlib `sqlite3`, `struct`, `argparse`, `asyncio`. Dev deps: `pytest`, `ruff`.
- Emits no `detector_id`, no `FaultCode`, no `schema_id`, no new `EventType`/`CommandType` (registry: none needed).

## 5. Must not
- Reimplement detection logic — render only.
- Send data to third parties; no server, no telemetry, no health values in system logs.
- Present detections without "possible"/"suspected" **+ confidence value**.
- Imply live data while disconnected: every cached value is shown with its age.
- Be required for alerting: nothing in this app gates firmware behavior.
- Edit the excluded paths (§2) or any locked doc / `app.*`.
- Guess layouts: reject unknown version bytes; unknown enum values render as `unknown (#n)`, never coerced (BLE_GATT §1, SYNC §4.7).

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Stack — [DECIDED: developer ruling 2026-10-17, interim DEC-005 for W5]**
- Problem: platform for pairing, live status, timeline, sync control, time sync.
- Simplest: **Python desktop app, `bleak` + local SQLite** — same language as `tools/`/`ml/`, fastest route to a working M4 bench central.
- Scalable: **Flutter mobile app**, local-first SQLite (the DEC-005 proposal; real wearable-companion form factor).
- Trade-offs: Python is research-oriented and desktop-only (not a phone companion); Flutter needs a mobile toolchain and would not have produced the bench central M4 needs now.
- **Choice:** Python desktop for W5 — recorded here as the **interim DEC-005 ruling; Flutter deferred** to a later wave. Layers (D2) are stack-agnostic, so a later Flutter app reuses the contract knowledge, not the Python code. Django/server: never (DEC-010).

**D3. Presentation — CLI-first**
- Problem: form factor for live status/timeline/sync control under "nothing fancy".
- Simplest: console subcommands + a `watch` loop printing refreshed status; plain-text tables.
- Mid: Textual/rich TUI. Scalable: Flutter GUI (deferred with D1).
- Trade-offs: a TUI adds a dependency and golden-test surface for no scope gain; a GUI is explicitly out of this wave.
- **Choice:** CLI-first; view-models are UI-agnostic so a later GUI replaces only `ui/`.

**D4. Sync protocol ownership — port to F20**
- Problem: the phone-side sync client (CURSOR, CRC-16, ack, flow control) belongs to `companion/lib/sync/**` (F20), but the app and bench CLI need sync now.
- Simplest: F21 implements the whole protocol in its own tree (works immediately, duplicates the protocol, steps on ownership).
- Scalable: F21 **defines** `SyncPort` + `SyncStatus` model and ships `NullSyncPort` ("sync client not linked") + a scripted `FakeSyncPort`; F20's half implements the port in `lib/sync/**`.
- Trade-offs: the CLI `sync` subcommand is inert until F20's half lands (same wave, W5 order: F21 skeleton → halves); no protocol duplication ever; both halves testable alone.
- **Choice:** the port. Split of duty: **F20** = framing/CRC/cursor/ack/flow-control; **F21** = byte codecs, validation, repository upsert, orchestration, UI. The record sink is `on_records(list[bytes])` with each item **exactly 48 B** (raw slices) so decode ownership stays wholly in F21.

**D5. Event storage**
- Problem: idempotent local store feeding the read-only timeline, resumable across companion reboots.
- Simplest: decoded columns keyed `(device_id, seq)` only.
- Scalable: decoded columns **+ `raw` 48 B blob** — re-decode after schema knowledge grows without re-syncing.
- Trade-offs: one extra column; raw bytes are the contract's authoritative image (SYNC §3.4).
- **Choice:** decoded columns + `raw`, `PRIMARY KEY(device_id, seq)` (upsert idempotency, SYNC §4.4), plus `sync_state.high_water` (companion-reboot resume, §4.6) and forward-only numbered migrations in `db.py`.

**D7. SetTime argument layout — CR-021 (accepted 2026-10-17, applied on master `27ad169`)**
- Problem: `external.md` §6.1 says the phone sends `SetTime(unix_seconds)`, but neither `data_decision.md` §4.6 nor `BLE_GATT.md` §4.3 pins **which** `Command` arg carries it. Grep of docs + firmware shows no consumer exists yet (only `SetSetting` arg0/arg1 and `ConfirmEvent arg0=seq` are pinned).
- Simplest: `[ASSUMED] arg0 = unix_seconds` (precedent: payload lives in arg0; `UnixSeconds` fits u32 until 2106) and proceed.
- Scalable: pin it in the contract **now**, while both ends are unimplemented, so the future firmware clock consumer cannot diverge.
- **Choice:** raise **CR-021** (clarification → `data_decision.md` §4.6: `SetTime: arg0 = unix_seconds u32 LE, arg1 = 0`) — **accepted 2026-10-17 (W5 G1) and applied on master `27ad169`**; code against it. F21 never edits the contract file itself.

**D8. ATT MTU (backlog 37 — the bench central must negotiate)**
- Problem: the M4 streaming leg needs a central with ATT MTU ≥ 55 (`kMinSyncMtuBytes`, SYNC §2; BLE_GATT §4.7). A stack default of 23 would silently disable streaming (`mtu_too_low` flag) and Event Notify (floor 51).
- Simplest: trust the OS and proceed.
- Scalable: `connect()` performs an explicit **request** (target `kPreferredPeerMtu` = 185, mirroring the device) and then a **read-back check** of `mtu_size`; sync refuses to start below 55 and `bench connect` prints the negotiated value; below 55 the CLI surfaces `mtu_too_low` and exits non-zero for `sync`.
- **Choice:** request + fail-closed verify. On backends where the OS negotiates automatically (WinRT `[ASSUMED]`) the request is a no-op and the check still runs — identical, verifiable behavior either way.

**D9. Pairing / bonding**
- Simplest: rely on OS *Just Works* bonding (device IO cap NoInputNoOutput, BLE_GATT §2); on ATT `0x05`/`0x0F` (including CCCD refusals, §3) print "bonding required — pair in the OS Bluetooth settings, then retry".
- Scalable: in-app pairing UI (platform APIs bleak does not expose portably).
- **Choice:** OS-managed pairing `[ASSUMED]` + actionable error text. The actual pair/bond bench run stays a developer action (this is PLAN; bench testing is post-W5).

## 7. Module structure

Source root `companion/lib/` maps to the importable package `companion` (pyproject `package-dir`); tests run from `companion/`. Console scripts: `anarolepsy-app`, `anarolepsy-bench`.

```
companion/
  SPEC.md                  pointer to this spec
  pyproject.toml           package-dir mapping, deps (bleak), console scripts, ruff/pytest config
  README.md                run + bench instructions (MTU note included)
  lib/
    app.py                 composition root: wires transport + repository + view-models + ui (no logic)
    bench.py               bench CLI: scan | connect | status | sync   (M4 bench central)
    transport/
      ports.py             GattTransport Protocol + Char constants/UUIDs (BLE_GATT §3, append-only index)
      bleak_transport.py   bleak implementation: connect, MTU request+verify (D8), notify dispatch
      fake_transport.py    scripted fake for pytest: canned byte streams, MTU knob, fault injection
      codec.py             pure codecs by contract offsets: live_status(14B), hr, battery,
                           event48, command9, device_info/serial(device_id) — golden-tested (§9)
      commands.py          frame builders: build_set_time(unix_s) (D7, CR-021)
    data/
      models.py            EventRecord, LiveStatus, Battery, SyncStatus, DeviceInfo dataclasses
      db.py                SQLite open + forward-only migrations (schema v1, §7.3)
      event_repository.py  upsert(device_id, records), high_water, timeline queries, TimeSync rows
    domain/                pure, no bleak/sqlite imports
      time_reconstruct.py  SYNC_PROTOCOL §4.5 rule (D6)
      wording.py           "possible/suspected + confidence", "reason #N" for unknown codes, data-age labels
    sync_port.py           SyncPort Protocol + NullSyncPort (D4) — seam for F20's lib/sync/**
    features/
      live.py              live view-model: device state, battery, HR, link/sync status (1 Hz refresh)
      timeline.py          read-only timeline view-model (seq-desc, wall time or flagged unknown)
      sync_control.py      sync control view-model: start from hw+1, progress, STATUS flags, errors
      # never created/edited by F21: nap/ (F15), history/ (F16), explain/ (F22), lib/sync/ (F20)
    ui/
      render.py            plain-text rendering of view-model state (no framework)
  tests/
    conftest.py            fixtures: FakeTransport scripts, tmp SQLite, byte-vector helpers
    test_codec_*.py, test_time_reconstruct.py, test_event_repository.py,
    test_sync_port.py, test_viewmodels_*.py, test_cli.py, test_wording.py   (§9)
```

**Seam signatures (plan-level; frozen at IMPLEMENT, later changes go through this spec):**
- `GattTransport`: `async connect(address | None) -> DeviceInfo` · `mtu_size -> int` · `async read(char) -> bytes` · `async write(char, data) -> None` · `async start_notify(char, cb) -> None` · `async stop_notify(char) -> None` · `async disconnect() -> None`
- `SyncPort`: `async request_from(from_seq, max_events=0)` · `async ack(seq)` · `status() -> SyncStatus(last_seq, last_synced, link_up, streaming, mtu_too_low, transfer_aborted)` · `set_record_sink(cb: list[bytes])` · `set_status_sink(cb)`
- `EventRepository`: `upsert(device_id, records: list[bytes]) -> int` · `high_water(device_id) -> int` · `timeline(limit, since_seq=None)` · `time_syncs(boot_id)`

The async port matches bleak natively; tests drive scenarios with `asyncio.run(...)` helpers, so **no pytest-asyncio dependency**.

**IMPLEMENT-stage seam refinements (additive only, 2026-10-17):**
- `SyncStatus` gained `linked: bool = True` — `False` only in F21's `NullSyncPort` (D4), so app/CLI can distinguish "sync client not linked" from "link down".
- `EventRepository.timeline(limit, since_seq=None, *, device_id=None)` and `time_syncs(boot_id, *, device_id=None)`: optional keyword device filter (positional signatures unchanged; the DB may hold more than one `device_id`).

**Migration v2 (audit F21-AUD-04, applied 2026-10-17):** `ALTER TABLE events ADD COLUMN wall_attempted INTEGER NOT NULL DEFAULT 0;` — bookkeeping only (no decoded column, raw blob or key change, no backfill needed). `0` = still eligible for §4.5 resolution; `1` = resolution already attempted and its definitive result was 0, which collides with the unknown sentinel — the row is never re-resolved, and the stored value stays 0 (unknown, flagged, never fabricated). Forward-only: a DB from a newer app is refused, never downgraded. Test: `test_schema_v1_database_upgrades_in_place_and_newer_is_refused`.

## 8. Constants (named, tagged, justified)

| Name | Value | Tag | Why |
|---|---|---|---|
| `kGattServiceUuid` | `4bb00319-8811-4387-95a6-f4bb78380000` | `[HW]` | scan filter (BLE_GATT §2) |
| `kDeviceLocalName` | `Anarolepsy` | `[HW]` | scan confirmation (BLE_GATT §2) |
| `kEventRecordBytes` | `48` | `[HW]` | canonical `Event` image (CR-002, BLE_GATT §4.6) |
| `kMinSyncMtuBytes` | `55` | `[DERIVED]` | 3 ATT + 2 counter + 48 record + 2 CRC (SYNC §2); fail-closed sync gate (D8) |
| `kEventNotifyMtuFloor` | `51` | `[DERIVED]` | 48 + 3 ATT header (BLE_GATT §4.7); explains suppressed Event Notify |
| `kPreferredPeerMtu` | `185` | `[HW]` | device's preferred ATT MTU (BLE_GATT §2); request target |
| `kGattContractVersion` | `1` | `[HW]` | version byte; unknown → reject, never guess (BLE_GATT §1) |
| `kSyncProtocolVersion` | `1` | `[HW]` | first byte of every frame (SYNC §5); shown by `status` |
| `kLiveStatusBytes` | `14` | `[HW]` | Live Status image length (BLE_GATT §4.1) |
| `kLiveStatusIntervalMs` | `1000` | `[HW]` | device emits ≤ 1 Hz (BLE_GATT §4.1) → UI refresh cadence |
| `kHrNotifyIntervalMs` | `1000` | `[HW]` | device HR cap (BLE_GATT §4.2) → HR staleness threshold |
| `kScanTimeoutS` | `10` | `[EXPERIMENTAL]` | bounded scan for bench use; tune with bench experience |
| `kConnectTimeoutS` | `15` | `[EXPERIMENTAL]` | bounded connect; generous for first Windows pairing |
| `kSyncStallTimeoutMs` | `5000` | `[DERIVED]` | mirrors device `kAckTimeoutMs` (SYNC §4.2): no progress ⇒ show stalled, re-request |
| `kStaleLabelMs` | `5000` | `[EXPERIMENTAL]` | beyond this age after disconnect, values are labeled "stale" (always shown with age) |
| `kDefaultTimelineLimit` | `50` | `[USER]` | timeline page size; overridable by flag |
| `kSyncRequestBatch` | `128` | `[EXPERIMENTAL]` | CURSOR `max` per round (u16-bounded); caps one transfer |
| `kDbFileName` | `companion.sqlite3` | `[DERIVED]` | single local-first DB under the OS user data dir; override by env/flag |
| `kSetTimeOnConnect` | `true` | `[DERIVED]` | device clock is unknown after boot (DEC-009); F17's resync tolerance makes repeats harmless |
| `EVENT_SCHEMA_VERSION` | `1` | `[HW]` | `Event::kSchemaVersion` (data_decision §4.5); unknown schema stored raw, flagged unsupported (D5) |
| `EVENT_TYPE_TIME_SYNC` | `10` | `[HW]` | `EventType` index of TimeSync (data_decision §4.5, append-only); feeds SYNC §4.5 reconstruction |
| `COMMAND_TYPE_SET_TIME` | `11` | `[HW]` | `CommandType` index of SetTime (data_decision §4.6, append-only); CR-021 pins the args, not the index |

## 9. Test plan (pytest + fake transport)

**Strategy:** everything except `bleak_transport.py` is pure or I/O-injectable and runs on the host with `FakeTransport` and a temp SQLite file — no BLE hardware in tests (bench = developer action after W5). Golden bytes come from the contracts' **own** vectors (BLE_GATT §6, SYNC §6), so drift in either direction fails. `bleak_transport.py` is kept to a thin adapter whose MTU/connect behavior is asserted through the `GattTransport` seam with the fake.

**Commands (run at IMPLEMENT+TEST, results recorded in the status log):**
- `python -m pytest tests` (cwd `companion/`)
- `ruff check companion/` — integrator action item: add `companion/` to `ci/check.py` `RUFF_PATHS` (F00-owned file, not edited by F21).

**IMPLEMENT+TEST result (2026-10-17):** `python -m pytest tests -q` (cwd `companion/`) → **92 passed** (baseline 0, §11); `python -m ruff check .` (cwd `companion/`, covers `companion/` incl. `tests/`) → **All checks passed**. Extra evidence: `pip wheel --no-build-isolation --no-deps` builds a wheel laying `lib/` out as package `companion` (console scripts `anarolepsy-app`/`anarolepsy-bench` resolve).

**Tests:**
1. `test_codec_event48` — BLE_GATT V6 first-16 bytes + SYNC V4 full 52 B chunk fixture → exact field asserts; encode(fixture record) == fixture bytes (round-trip); wrong length → error; `schema_version != 1` → stored raw, flagged unsupported (never guessed).
2. `test_codec_live_status` — V1 golden 14 B decode; `ver != 1` → rejected; 0-byte read → `None` ("no snapshot yet", BLE_GATT §5).
3. `test_codec_hr_battery` — V2 both widths (uint8/uint16); battery `0xFF` → unknown, never a percentage.
4. `test_codec_command` — `build_set_time` golden 9 B: `0B` + unix LE u32 + `00 00 00 00` (D7; CR-021 accepted + applied).
5. `test_time_reconstruct` — SYNC §4.5: direct `t_start_unix`; nearest `TimeSync` of same `boot_id`; tie → earlier; event before its TimeSync (signed math); no TimeSync → 0 flagged; other `boot_id` ignored.
6. `test_event_repository` — same `(device_id, seq)` twice → 1 row (idempotent); backward/duplicate seq no-op; `high_water` monotonic and survives reopen (resume, §4.6); timeline order `seq DESC`; `upsert` count return.
7. `test_sync_port` — `NullSyncPort` reports "not linked" (app/CLI degrade cleanly before F20's half); `FakeSyncPort` drives a stall scenario: no progress for `kSyncStallTimeoutMs` → status "stalled" + re-request; duplicate records upsert as no-op.
8. `test_viewmodel_live` — scripted bytes → battery 87, state Drowsy, HR 72, link/sync status; disconnect → all values retained **with age**; battery `0xFF` → "unknown".
9. `test_viewmodel_timeline` — mixed `t_start_unix` 0/non-zero → wall time or flagged "unknown"; detection rows render **"possible/suspected" + confidence** (spec DoD §14); unknown `ReasonCode` → `reason #N`, not hidden.
10. `test_viewmodel_sync_control` — starts from `hw + 1`; progress counters; STATUS flags (`streaming`, `mtu_too_low`, `transfer_aborted`) surfaced, never crash.
11. `test_cli` — `scan` filters by service UUID + local name; `status` prints device_id/battery/MTU/`SyncStatus`; `connect` prints negotiated MTU.
12. `test_cli_mtu_gate` — fake MTU 23 → `sync` refuses with `mtu_too_low` text and non-zero exit (D8); MTU ≥ 55 → `request_from` called (backlog 37 guard).
13. `test_wording` — "possible"/"suspected" + confidence formatting; no diagnostic/medical wording anywhere in rendered output.

## 10. Failure behavior

| Situation | Behavior |
|---|---|
| Device disconnected | show last synced data **with age** (stale past `kStaleLabelMs`); banner "not live"; never imply live data |
| ATT MTU < 55 | sync refused before any CURSOR write; status/CLI show `mtu_too_low` (mirrors SYNC §3.2 flag) |
| ATT `0x05` / `0x0F` (unbonded/unencrypted, incl. CCCD) | actionable "bonding required" message + retry path; no silent retry loop |
| Chunk CRC failure / garbage frame | drop (F20 client side); phone shows no progress, re-requests after `kSyncStallTimeoutMs`; never acks |
| Sync client not linked (F20 half absent) | `NullSyncPort`: timeline/status/timesync work from the local DB; `sync` says "sync client not linked" |
| Device clock never synced / no TimeSync | wall time = "unknown" (0), flagged; never fabricated |
| Unknown enum / version byte | render `unknown (#n)` / reject the frame; never coerce to a known value |
| SQLite locked/corrupt | report the error, keep the app responsive; device-side data is never at risk (it only reads) |

## 11. Budget

## 12. Out of scope
- Multi-user cloud, any server, third-party analytics (DEC-010).
- Clinical reporting, diagnosis wording, trend analytics.
- Settings/config UI, user labels/confirm-reject flows, diagnostics screens, export (F28), nap/history/explain features (excluded dirs).
- In-app pairing UI; BLE bench testing (developer action after W5).

---
