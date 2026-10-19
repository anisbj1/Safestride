"""Scripted FakeTransport for pytest (D2 seam): canned reads, MTU knob, notify
injection, recorded writes and fault injection. No BLE hardware anywhere (spec §9).
"""

from __future__ import annotations

from collections.abc import Callable

from companion.data.models import DeviceInfo

# [EXPERIMENTAL] Default fake ATT MTU = the device's preferred value (BLE_GATT §2);
# tests override it (e.g. 23 or 55) to drive the D8 fail-closed gate.
DEFAULT_FAKE_MTU = 185

ReadValue = bytes | Callable[[], bytes] | Exception


class FakeTransport:
    """Implements GattTransport against plain Python scripts (spec §7 seam).

    - reads: char → bytes | callable | Exception instance (raised on read)
    - connect_error / write_error: injected faults (D9 / §10 failure rows)
    - emit(char, data): simulate a notification/indication from the "device"
    """

    def __init__(
        self,
        *,
        device_info: DeviceInfo | None = None,
        mtu: int = DEFAULT_FAKE_MTU,
        reads: dict[str, ReadValue] | None = None,
        connect_error: Exception | None = None,
        write_error: Exception | None = None,
    ) -> None:
        self.device_info = device_info or DeviceInfo(device_id="aabbccddeeff", fw="fw-test")
        self.mtu_size = mtu
        self.reads: dict[str, ReadValue] = dict(reads or {})
        self.connect_error = connect_error
        self.write_error = write_error
        self.connected = False
        self.writes: list[tuple[str, bytes]] = []
        self.notify_cbs: dict[str, Callable[[bytes], None]] = {}
        self.connect_calls: list[str | None] = []

    # --- test helpers --------------------------------------------------------

    def set_read(self, char: str, value: ReadValue) -> None:
        self.reads[char] = value

    def emit(self, char: str, data: bytes) -> None:
        """Simulate a device notification/indication on a subscribed characteristic."""
        cb = self.notify_cbs.get(char)
        if cb is None:
            raise AssertionError(f"no notify subscription for {char}")
        cb(data)

    # --- GattTransport -------------------------------------------------------

    async def connect(self, address: str | None) -> DeviceInfo:
        self.connect_calls.append(address)
        if self.connect_error is not None:
            raise self.connect_error
        self.connected = True
        return self.device_info

    async def read(self, char: str) -> bytes:
        if not self.connected:
            raise AssertionError("read while disconnected")
        value = self.reads.get(char, b"")
        if isinstance(value, Exception):
            raise value
        if callable(value):
            return value()
        return bytes(value)

    async def write(self, char: str, data: bytes) -> None:
        if not self.connected:
            raise AssertionError("write while disconnected")
        if self.write_error is not None:
            raise self.write_error
        self.writes.append((char, bytes(data)))

    async def start_notify(self, char: str, cb: Callable[[bytes], None]) -> None:
        if not self.connected:
            raise AssertionError("start_notify while disconnected")
        self.notify_cbs[char] = cb

    async def stop_notify(self, char: str) -> None:
        self.notify_cbs.pop(char, None)

    async def disconnect(self) -> None:
        self.connected = False
        self.notify_cbs.clear()
