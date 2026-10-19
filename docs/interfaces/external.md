# 02 — INTERFACES · external contracts (time sync, BLE, sync, dataset, feature schema, change policy)

> Part of the shared contracts. **Locked: change only via change request.** Section numbers match the original `02` numbering; index in `docs/02_INTERFACES.md`.

## 6. External contracts (draft; detailed files under `docs/contracts/`)

### 6.1 Time sync
Phone sends `SetTime(unix_seconds)`; device records `TimeSync{ boot_id, t_mono_ms, unix_s }` as an `Event`. Companion reconstructs wall time of any earlier event from the nearest `TimeSync` of the same `boot_id`. Drift over hours is `[ASSUMED]` acceptable for this project. Owner: F19 (transport) + F17 (record) + F20 (reconstruction rule).

### 6.2 BLE GATT (owner F19 → `contracts/BLE_GATT.md`)
Custom 128-bit service UUIDs (generated once by F19, recorded in the contract) plus standard Battery (0x180F) and Device Information (0x180A).
| Characteristic | Props | Payload |
|---|---|---|
| Device Info | R | fw version, hw rev, `schema versions`, `device_id` |
| Live Status | R, N | `StateSnapshot` compact (≤ 20 B) at ≤ 1 Hz while subscribed |
| Heart Rate | N | standard HR Measurement format when valid |
| Event Notify | N | newest `Event` (compact) — convenience only, not the sync path |
| Command | W (encrypted) | `Command` compact |
| Config | R/W (encrypted) | key/value settings |
| Sync Control | W, I | `{from_seq, max, ack_seq}` requests / status indications |
| Sync Data | N | chunked `Event` records with chunk counter + CRC |
Characteristics that carry state, events or commands require an encrypted, bonded link (standard NimBLE bonding, see F19 D1).

### 6.3 Sync protocol (owner F20 → `contracts/SYNC_PROTOCOL.md`)
Cursor-based, idempotent. `(device_id, seq)` uniquely identifies an event. Phone: `RequestFrom(seq)` → device streams chunks → phone validates CRC and stores → phone writes `Ack(seq_high)` → device calls `markSyncedThrough(seq_high)`. Partial transfer ⇒ no ack ⇒ resend; duplicates dropped by key. Device never waits for sync to alert or store. Transport seam: `bw::ISyncTransport` (services §5, CR-019).

### 6.4 Dataset & capture format (owner F28 → `contracts/DATASET_FORMAT.md`)
Session directory: `session.json` (session id, device id, firmware version, feature schema versions, free-text notes) + `raw_imu.csv`, `raw_ppg.csv`, `features.csv`, `events.csv`, `markers.csv` (+ `labels.csv` after review). All rows carry `t_ms` and `boot_id`. Serial capture line format: `#HDR,<schema>` then `I,t,ax,ay,az,gx,gy,gz` / `P,t,red,ir` / `F,...` / `E,...` / `M,t,marker`.

### 6.5 Feature schema (owner F05 → `contracts/FEATURE_SCHEMA.md`)
`feature_schema_v<N>.json`: ordered list `{name, unit, source, normalization}` + `schema_id`. **The same file** drives Python training and generates the C++ constants; the firmware refuses a model whose `schema_id/version` does not match the assembler's.

### 6.6 Contract change policy
Additive changes (new enum value appended, new field at end with version bump) → CR reviewed at the next integration. Breaking changes → CR + all owners notified + migration note. Enums are **append-only**.
