# Repository layout and engineering approach

Safestride separates hardware, signal processing, estimation, state/alert logic, storage, transport, and presentation. Feature libraries depend on shared value contracts; the application composition root constructs the concrete implementations and connects them.

```text
firmware/
  include/config/       board and pin configuration
  lib/bw_core/          shared value types, contracts, containers
  lib/bw_hal/           hardware interfaces and implementations
  lib/bw_<feature>/     modular firmware features
  src/app/              composition root and feature wiring
  test/                 Unity tests, including native-host suites
firmware_cam/           optional ESP32-CAM experiment
companion/              Python desktop companion (BLE, SQLite, CLI)
ml/                     wakefulness/event schemas and evaluation
 tools/                  capture, replay, decode, and export utilities
 docs/                   architecture, interfaces, protocols, and feature specs
 ci/                     repository verification entry point
```

## Technology stack

| Area | Technology |
|---|---|
| Firmware | C++17, PlatformIO, Arduino-ESP32, FreeRTOS |
| Firmware tests | Unity; pure-logic libraries also build for the native host |
| BLE | NimBLE-Arduino |
| Local event storage | LittleFS with a versioned record format and CRC |
| Companion | Python 3.11+, bleak, SQLite; local-first, no server |
| ML/research tooling | Python schemas, evaluation, capture, replay, and export tools |

## Test strategy

1. Deterministic unit tests for pure processing, state, and policy logic.
2. Replay tests that run recorded or synthetic sensor sequences through the processing pipeline.
3. Contract tests for value layout, binary encoding, and protocol compatibility.
4. Separate hardware bring-up and bench checks for drivers and physical behavior.
5. Event-level evaluation for exploratory classifiers; synthetic results are not clinical validation.

Use `python ci/check.py` at the repository root for the complete configured verification suite. See `DEVELOPMENT.md` for setup and commands.
