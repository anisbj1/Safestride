# F30 — OTA firmware update

> Track **E** · Milestone **M6** · Runs on **firmware** · Contract changes only via change request.

## 1. Responsibility
Deploy new firmware without physical reprogramming: isolated update mechanism with validation, rollback and never during an active alert.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- Update image stream (Wi-Fi HTTP in research mode, or BLE chunked later)
- `AlertStatus` (refuse while alerting)
- `BatteryReading` (refuse if low)

**Outputs (produced contracts)**
- Update result events
- Rollback on failed self-test

## 4. Dependencies
- `bw_core` only; partition layout coordinated with F18 via CR.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Contain application logic.
- Update without integrity verification.
- Run without sufficient battery.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Transport**
- Simplest: Wi-Fi HTTP from a local machine (dev mode).
- Professional/scalable: Signed images over BLE via the companion.
- Trade-offs: BLE is slow (minutes for ~1 MB); Wi-Fi adds RAM/power and only in maintenance mode.
- **Choice:** Wi-Fi HTTP first; image hash check; dual-slot with rollback (`esp_ota` mark valid after self-test). Signing when the project needs it (CR).

## 7. Module structure
- `OtaManager`
- `ImageValidator`
- `UpdateGuard`

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kMinBatteryPercentForOta` | 40 | `EXPERIMENTAL` | A failed update from a dying battery is the worst case |
| `kSelfTestTimeoutMs` | 30000 | `EXPERIMENTAL` | New image must confirm itself or roll back |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
- Host: guard logic, hash verification with fixtures.
- Host: interrupted and bad-image paths via a fake partition API; an on-device check is optional.

## 10. Failure behavior
- Any failure ⇒ keep running current image.

## 11. Budget
Flash: two app slots must fit 4 MB (A-03) alongside LittleFS; verify with `partitions.csv`.

## 12. Out of scope
- Delta updates.

## 13. Open questions / assumptions
- Partition table co-owned with F18 (CR).

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Rollback logic covered by host tests with a fake partition API.

---
