# F27 — ESP32-CAM experimental module

> Track **E** · Milestone **M6** · Runs on **firmware_cam** · Contract changes only via change request.

## 1. Responsibility
Optional research add-on on a separate MCU/firmware: controlled recording (image/video) triggered by an explicit command, stored locally. The core wearable never requires it.

## 3. Inputs and outputs
**Inputs (consumed contracts)**
- Trigger/command from the main device (UART or GPIO) or manual button

**Outputs (produced contracts)**
- Local recordings with a session id matching F28 sessions
- Status line over the link

## 4. Dependencies
- Own project; shares only `docs/contracts/CAM_LINK.md` and the session-id convention.

Compile-time: `bw_core` only. Everything above is met through contracts, fakes or providers.

## 5. Must not
- Record without an explicit start command.
- Upload anywhere.
- Be a dependency of core firmware.

## 6. Design decisions (problem → alternatives → trade-offs → choice)

**D1. Link to the main device**
- Simplest: None (manual, timestamps aligned by hand).
- Professional/scalable: UART command link with session id + sync marker.
- Trade-offs: Extra wiring/software vs painful manual alignment.
- **Choice:** Start with manual/UART start-stop + sync marker; no data flows back.

## 7. Module structure
- `firmware_cam/src/{camera,storage,link}`

## 8. Constants (named, tagged, justified)
| Name | Value | Tag | Why |
|---|---|---|---|
| `kMaxRecordingMs` | 60000 | `EXPERIMENTAL` | Bounds storage and privacy exposure |

All values are starting points unless tagged `[HW]`; tune with data (F28/F29).

## 9. Test plan
- Host tests for link parser and recording state machine; bench tests for capture/storage.

## 10. Failure behavior
- No SD ⇒ refuse to record.

## 11. Budget
Separate MCU.

## 12. Out of scope
- Computer-vision inference in v1.

## 13. Open questions / assumptions
- A-10: the SD slot is a configuration flag; F27 refuses to record without storage.

## 14. Feature-specific Definition of Done (in addition to `03` §4)
- Start/stop/max-duration behavior documented and tested.

---
