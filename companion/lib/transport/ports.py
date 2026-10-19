"""GattTransport port + GATT constants (BLE_GATT.md v1.1, spec §7 seam, D2).

Pure: Protocol, named constants and a scan-result value type — no bleak import here,
so view-models, UI and tests never pull in a BLE stack (bleak lives in
``bleak_transport`` only, imported lazily).
"""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from typing import Protocol, runtime_checkable

from companion.data.models import DeviceInfo

# --- spec §8 constants (name ↔ spec table; exactly one tag + why each) ---------

# [HW] Scan-filter service UUID in the advertising payload (BLE_GATT §2).
GATT_SERVICE_UUID = "4bb00319-8811-4387-95a6-f4bb78380000"
# [HW] Device local name from the scan response (BLE_GATT §2).
DEVICE_LOCAL_NAME = "Anarolepsy"
# [HW] Device's preferred ATT MTU (BLE_GATT §2 `kPreferredMtu`): request target (D8).
PREFERRED_PEER_MTU = 185
# [DERIVED] 3 ATT + 2 counter + 48 record + 2 CRC = 55 (SYNC §2 `kMinSyncMtuBytes`);
# the device streams only at ATT MTU >= 55, so sync fails closed below it (D8, backlog 37).
MIN_SYNC_MTU_BYTES = 55
# [DERIVED] 48 B Event record + 3 B ATT header = 51 (BLE_GATT §4.7): below it the
# device suppresses Event Notify entirely — explains a silent characteristic (never truncated).
EVENT_NOTIFY_MTU_FLOOR = 51
# [HW] First byte of every sync frame (SYNC §5 `kSyncProtocolVersion`); shown by `bench status`.
SYNC_PROTOCOL_VERSION = 1
# [HW] GATT contract version byte (BLE_GATT §1): readers reject unknown versions, never guess.
GATT_CONTRACT_VERSION = 1

# [HW] 128-bit UUID base + 16-bit append-only index (BLE_GATT §3, scheme D3);
# published indices are never renumbered.
GATT_UUID_BASE = "4bb00319-8811-4387-95a6-f4bb7838"

# [HW] Append-only characteristic indices (BLE_GATT §3 rows 5-11).
CHAR_LIVE_STATUS = 0x0001
CHAR_HEART_RATE = 0x0002
CHAR_EVENT_NOTIFY = 0x0003
CHAR_COMMAND = 0x0004
CHAR_CONFIG = 0x0005
CHAR_SYNC_CONTROL = 0x0006
CHAR_SYNC_DATA = 0x0007


def char_uuid(index: int) -> str:
    """SIG-style 128-bit UUID for a 16-bit characteristic index (BLE_GATT §3)."""
    return f"{GATT_UUID_BASE}{index:04x}"


LIVE_STATUS_UUID = char_uuid(CHAR_LIVE_STATUS)
HEART_RATE_UUID = char_uuid(CHAR_HEART_RATE)
EVENT_NOTIFY_UUID = char_uuid(CHAR_EVENT_NOTIFY)
COMMAND_UUID = char_uuid(CHAR_COMMAND)
CONFIG_UUID = char_uuid(CHAR_CONFIG)
SYNC_CONTROL_UUID = char_uuid(CHAR_SYNC_CONTROL)
SYNC_DATA_UUID = char_uuid(CHAR_SYNC_DATA)

# [HW] Standard 16-bit characteristic UUIDs in SIG base form (BLE_GATT §3 rows 0-4).
FIRMWARE_REVISION_UUID = "00002a26-0000-1000-8000-00805f9b34fb"
HARDWARE_REVISION_UUID = "00002a27-0000-1000-8000-00805f9b34fb"
SOFTWARE_REVISION_UUID = "00002a28-0000-1000-8000-00805f9b34fb"
SERIAL_NUMBER_UUID = "00002a25-0000-1000-8000-00805f9b34fb"
BATTERY_LEVEL_UUID = "00002a19-0000-1000-8000-00805f9b34fb"


@dataclass(frozen=True)
class ScanResult:
    """One device seen during a scan: name, address and any advertised 128-bit UUIDs
    (platform metadata — empty when the backend does not surface them)."""

    name: str
    address: str
    uuids: tuple[str, ...] = ()


@runtime_checkable
class GattTransport(Protocol):
    """Byte-pipe seam (spec §7, D2). UI/view-models never touch bleak directly; the
    fake implementation drives every host test. MTU behavior (D8) lives behind
    `mtu_size`: connect() requests the preferred MTU and verifies by read-back.
    """

    @property
    def mtu_size(self) -> int:
        """Negotiated ATT MTU; < MIN_SYNC_MTU_BYTES means sync must fail closed (D8)."""

    async def connect(self, address: str | None) -> DeviceInfo:
        """Connect (None = let the stack pick / auto-connect) and return identity."""

    async def read(self, char: str) -> bytes:
        """Read a characteristic value; 0 bytes is the honest "no snapshot yet" (BLE_GATT §5)."""

    async def write(self, char: str, data: bytes) -> None:
        """Write a characteristic value (Command, Config, Sync Control)."""

    async def start_notify(self, char: str, cb: Callable[[bytes], None]) -> None:
        """Subscribe to notifications/indications; cb receives each payload as bytes."""

    async def stop_notify(self, char: str) -> None:
        """Unsubscribe."""

    async def disconnect(self) -> None:
        """Drop the link; nothing is kept across a disconnect (BLE_GATT §5)."""
