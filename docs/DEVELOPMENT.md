# Development and verification

This page describes how to build and test Safestride as a software project.

## Toolchain

- PlatformIO Core with the ESP32 platform and Arduino framework
- Python 3.11 or later
- `ruff`, `pytest`, and `clang-format` for the repository-wide check

The firmware's PlatformIO environments are defined in `firmware/platformio.ini`.

## Verification

Run all configured repository checks from the root:

```powershell
python ci/check.py
```

This runs native Unity tests, the ESP32 firmware build, Python linting/format checks, and the companion/tools test suites.

For a focused firmware build or host-test run, change to `firmware/`:

```powershell
pio run -e esp32dev
pio test -e native
```

Keep pure processing and decision logic independent of Arduino/FreeRTOS APIs so it remains testable in the native environment. Add deterministic host tests for behavior, missing/stale inputs, and timing boundaries when changing such logic.

## Device upload

Connect the target ESP32 and select its serial port:

```powershell
pio run -e esp32dev -t upload --upload-port COM14
```

Replace `COM14` with the port assigned by the host operating system. The default serial monitor rate is 115200 baud. Confirm the board and pin configuration in `firmware/include/config/` before connecting different hardware.

## Python components

Each Python component has its own `pyproject.toml`. For example, from `companion/`:

```powershell
python -m pip install -e ".[dev]"
python -m pytest
```

The `ml/` and `tools/` packages have their own test and lint configuration.
