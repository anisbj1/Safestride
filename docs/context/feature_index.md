# 00 — CONTEXT · feature index and glossary (§10–11)

> Part of `00_PROJECT_CONTEXT.md`; locked.

## 10. Feature index

Full specs in `docs/features/`. Track: A sensing/processing · B intelligence · C decision/UX · D data/comms · E platform. Milestones are summarized in the feature specifications.

| ID | Feature | Track | Milestone | Runs on |
|---|---|---|---|---|
| F00 | Foundation: repo, contracts-as-code, core library, test kit *(new)* | E | M0 | firmware + tools |
| F01 | Sensor acquisition & hardware abstraction | A | M1 | firmware |
| F02 | Signal processing | A | M1 | firmware |
| F03 | Activity classification | A | M2 | firmware |
| F04 | Wear detection | A | M2 | firmware |
| F05 | ML inference engine | B | M2 | firmware + ml |
| F06 | Wakefulness / drowsiness estimation | B | M2 (heuristic) → M5 (trained) | firmware + ml |
| F07 | State engine | C | M2 | firmware |
| F08 | Sleep episode detection | C | M3 | firmware |
| F09 | Suspected cataplexy / event detection (experimental) | B | M5 | firmware + ml |
| F10 | Alert engine | C | M2 | firmware |
| F11 | User interaction (buttons) | C | M2 | firmware |
| F12 | OLED user interface | C | M2 | firmware |
| F13 | Personalized baseline | B | M5 | firmware |
| F14 | User event confirmation | C | M3 | firmware |
| F15 | Scheduled nap management | C | M5 | firmware + companion |
| F16 | Sleep/nap history | D | M4 | firmware + companion |
| F17 | Event logging | D | M3 | firmware |
| F18 | Local data storage | D | M3 | firmware |
| F19 | BLE communication | D | M4 | firmware |
| F20 | Data synchronization | D | M4 | firmware + companion |
| F21 | Companion application | D | M4 | companion |
| F22 | Explainable event information | C | M3 (codes) → M5 | firmware + companion |
| F23 | Sensor quality & diagnostics | A | M2 (basic) → M5 | firmware |
| F24 | Operating modes | E | M5 | firmware |
| F25 | Power management | E | M6 | firmware |
| F26 | Fault handling & reliability | E | M5 | firmware |
| F27 | ESP32-CAM experimental module | E | M6 | firmware_cam |
| F28 | Data export, research mode & replay tooling | D | M1 (serial capture) → M4 | firmware + tools |
| F29 | Model evaluation & validation | B | M2 (skeleton) → M5 | ml |
| F30 | OTA firmware update | E | M6 | firmware |

---

## 11. Glossary
**Window**: fixed-length span of samples processed together. **Epoch**: longer aggregation span (e.g. 30 s) used for state estimation. **Feature vector**: fixed-schema array of floats fed to a model. **Schema**: versioned definition of feature order/meaning shared by firmware and ML training. **Reason code**: compact enum explaining why an event/alert happened. **Composition root**: the single place (`firmware/src/app`) that instantiates concrete classes and wires them. **Host test**: unit test running on the PC (`pio test -e native`).
