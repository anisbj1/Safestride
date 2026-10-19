"""Input transports for the capture CLI: a serial port, or a file/stdin.

The serial transport imports `pyserial` lazily so the rest of the package
(and its tests) work without it; the CLI then reports a clear message
instead of an import traceback.
"""

from __future__ import annotations

import sys
from collections.abc import Iterator
from pathlib import Path

# kSerialBaud in firmware/lib/bw_research/.../research_config.h (F28 D2):
# 50 Hz IMU + 100 Hz PPG CSV is ~6-10 KB/s; 115200 cannot carry it sustained.
DEFAULT_BAUD = 921_600

# [DERIVED] pyserial read timeout: bounds one blocking readline() so the CLI
# loop wakes at least once per second to re-check --minutes and Ctrl+C.
# 1.0 s keeps the capture duration accurate to ~1 s without busy-polling a
# quiet port (rows arrive every ~10 ms under load, so the timeout is rarely
# reached during an actual capture).
SERIAL_READ_TIMEOUT_S = 1.0


class FileTransport:
    """Replays a saved capture (or stdin with path "-") — used by tests/replay."""

    def __init__(self, path: str) -> None:
        self._path = path

    def lines(self) -> Iterator[str]:
        if self._path == "-":
            yield from _decode_lines(sys.stdin.buffer.read())
            return
        data = Path(self._path).read_bytes()
        yield from _decode_lines(data)

    def close(self) -> None:  # nothing to release
        return


class SerialTransport:
    """Live device capture. Stops on Ctrl+C (KeyboardInterrupt in the CLI)."""

    def __init__(
        self, port: str, baudrate: int = DEFAULT_BAUD, timeout: float = SERIAL_READ_TIMEOUT_S
    ) -> None:
        try:
            import serial  # lazy: pyserial is optional (developer action A4)
        except ImportError as exc:  # pragma: no cover - environment dependent
            raise RuntimeError(
                "pyserial is not installed; `pip install pyserial` in the tools "
                "environment (developer action A4) or use --stdin FILE"
            ) from exc
        self._serial = serial.Serial(port=port, baudrate=baudrate, timeout=timeout)

    def lines(self) -> Iterator[str | None]:
        while True:
            data = self._serial.readline()
            if not data:
                yield None  # timeout tick: lets the CLI re-check its duration
                continue
            yield from _decode_lines(data)

    def close(self) -> None:
        self._serial.close()


def _decode_lines(data: bytes) -> list[str]:
    # errors="replace": garbled diagnostics are not research rows anyway, and
    # the parser counts anything research-prefixed but unreadable as malformed.
    return data.decode("utf-8", errors="replace").splitlines()
