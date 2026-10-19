# Safestride

Safestride is an academic embedded-systems prototype for a wearable narcolepsy-assistance bracelet. It combines motion and pulse sensing, local wakefulness estimation, configurable alerts, on-device event storage, and an optional desktop companion.

> **Research prototype only.** Safestride is not a medical device, does not diagnose narcolepsy or sleep, and is not a substitute for professional care. Detection outputs are experimental and should be interpreted as possible events, not clinical findings.

## Project overview

The primary device is an ESP32-WROOM development board running C++17 firmware. Its processing pipeline is designed to continue locally without a phone or internet connection:

```text
Sensors → signal processing → feature estimation → state logic → alerts and display
                                           ↘ event storage → optional BLE sync
```

The firmware includes modular drivers and processing for a GY-87/MPU6050 IMU and MAX30102 PPG sensor; wear and activity estimates; wakefulness/state logic; LED, buzzer, button, and OLED interaction; local event storage; BLE communication and synchronization; and health/fault reporting. Research capture, replay/export tools, a Python companion, and an optional ESP32-CAM experiment are also included.

Wakefulness estimation currently uses a transparent, rule-based heuristic that emits probabilities. The inference interfaces support a replaceable model, but this repository does not include a trained narcolepsy-detection model or labeled clinical dataset. A probability output is not a clinical confidence or diagnosis.

## Hardware

The main prototype configuration uses:

- ESP32-WROOM development board
- GY-87 module (MPU6050 accelerometer/gyroscope)
- MAX30102 pulse-oximeter/PPG module
- 128×64 SH1106 OLED
- Buttons, RGB LED, and passive buzzer
- Li-Po battery and TP4056 charger module

Pin assignments and board-specific options are configuration, not assumptions in feature logic. Check `firmware/include/config/` before adapting the firmware to another board. Battery-voltage sensing is not available in the current two-wire battery setup; the battery reading is reported as unavailable rather than estimated.

## Repository layout

- `firmware/` — ESP32 firmware, reusable feature libraries, and Unity host tests
- `firmware_cam/` — optional camera experiment
- `companion/` — local Python desktop companion and CLI
- `ml/` — wakefulness schema, evaluation, and model-related tooling
- `tools/` — serial capture, replay, and dataset export utilities
- `docs/` — architecture, interfaces, feature specifications, design decisions, and protocol/data contracts
- `ci/check.py` — repository-wide verification entry point

## Build and test

On Windows, from the repository root, bootstrap the local Python environment and run the complete verification suite:

```powershell
.\scripts\setup.ps1
```

The script checks for Python 3.11+, creates `.venv`, installs the tools in
`requirements-dev.txt` and the companion package/dependencies, then runs
`ci/check.py`. If Python is missing, it offers to install Python 3.12 per-user
with `winget`. The first verification downloads PlatformIO's ESP32 toolchain
and libraries and can take several minutes. To install Python dependencies
without running the full suite yet:

```powershell
.\scripts\setup.ps1 -SkipVerification
.\.venv\Scripts\python.exe ci\check.py
```

Manual setup is also supported: install Python 3.11+, install
`requirements-dev.txt`, install `companion/` in editable mode, then run
`python ci/check.py`.

Firmware commands (run from `firmware/`):

```powershell
pio run -e esp32dev
pio test -e native
```

Upload to a connected ESP32 board (select the correct port for your system):

```powershell
pio run -e esp32dev -t upload --upload-port COM14
```

The companion and Python tooling can be installed/tested independently:

```powershell
cd companion
python -m pip install -e ".[dev]"
python -m pytest
```

## Design notes

The code is organized into small libraries with hardware access isolated behind interfaces. Pure processing and decision logic is host-testable. Shared data structures are fixed-size and suitable for task-to-task transfer. Missing, stale, or insufficient-quality sensor data is represented explicitly; the system should not invent a reliable reading when inputs are unavailable.

See `docs/00_PROJECT_CONTEXT.md`, `docs/01_ARCHITECTURE.md`, `docs/02_INTERFACES.md`, and the feature specifications under `docs/features/` for implementation details. Start with `docs/DEVELOPMENT.md` for repository setup and verification guidance.

## Limitations

This is a breadboard-scale academic prototype. Sensor quality, estimated states, alert thresholds, battery reporting, and experimental event detection are not clinically validated. Hardware configurations may require pin and polarity adjustments. A successful host test or firmware build does not establish real-world safety or medical effectiveness.
