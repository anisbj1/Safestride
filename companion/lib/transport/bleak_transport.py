"""bleak implementation of GattTransport — a thin adapter (spec §7, D8, D9).

Only this module imports bleak, lazily, so view-models, UI and every host test run
without a BLE stack or hardware. MTU (D8): request `PREFERRED_PEER_MTU`, then
read back `mtu_size`; on backends where the OS negotiates on connect the request is
a no-op `[ASSUMED WinRT]` and the read-back check still runs — identical behavior.
"""

from __future__ import annotations

from collections.abc import Callable

from companion.data.models import DeviceInfo
from companion.transport import ports
from companion.transport.codec import decode_revision, decode_serial

# [EXPERIMENTAL] Bounded connect; generous for a first Windows pairing (spec §8).
# Tune with bench experience (bench runs are a developer action, spec §12).
CONNECT_TIMEOUT_S = 15.0

# [EXPERIMENTAL] Bounded scan for bench use; tune with bench experience (spec §8).
SCAN_TIMEOUT_S = 10.0

# [EXPERIMENTAL] Text markers for ATT 0x05 / 0x0F refusals across bleak backends
# (BLE_GATT §5: unbonded/unencrypted access, including every CCCD write — D9/§10).
# Backends phrase these differently; verified on the bench, extend as seen there.
_BONDING_ERROR_MARKERS = (
    "insufficient authentication",
    "insufficient encryption",
    "authentication failure",
    "pairing failed",
    "not paired",
    "unencrypted",
    "0x05",
    "0x0f",
)


class BondingRequiredError(PermissionError):
    """ATT 0x05/0x0F: the link is not bonded/encrypted — actionable message, no
    silent retry loop (D9, spec §10)."""


def is_bonding_required(message: str) -> bool:
    """True when a backend error text means 'pair in the OS settings first' (D9)."""
    text = message.lower()
    return any(marker in text for marker in _BONDING_ERROR_MARKERS)


def _translate(exc: Exception) -> Exception:
    """Map bonding-related backend errors to the actionable BondingRequiredError."""
    if isinstance(exc, BondingRequiredError):
        return exc
    if is_bonding_required(str(exc)):
        return BondingRequiredError(
            "bonding required — pair in the OS Bluetooth settings, then retry "
            "(ATT 0x05/0x0F on an unbonded or unencrypted link; every CCCD needs "
            "encryption, BLE_GATT §3/§5)"
        )
    return exc


def _normalize_address(address: str) -> str:
    """device_id fallback: the BLE address as lowercase 12-hex without separators
    (BLE_GATT §2). Returns '' when the address is not a 48-bit MAC."""
    hex_only = "".join(c for c in address.lower() if c in "0123456789abcdef")
    return hex_only if len(hex_only) == 12 else ""


class BleakGattTransport:
    """GattTransport over bleak. Kept thin: connect/MTU/notify dispatch only (spec §9)."""

    def __init__(self, *, timeout_s: float = CONNECT_TIMEOUT_S) -> None:
        self._timeout_s = timeout_s
        self._client = None
        self._mtu = 0

    @property
    def mtu_size(self) -> int:
        """Negotiated ATT MTU (0 before connect). Verified, never assumed (D8)."""
        return self._mtu

    async def connect(self, address: str | None) -> DeviceInfo:
        from bleak import BleakClient

        if address is None:
            raise ValueError("no address — run `anarolepsy-bench scan` first")
        try:
            client = BleakClient(address, timeout=self._timeout_s, mtu=ports.PREFERRED_PEER_MTU)
        except TypeError:
            # Backend/version without an mtu kwarg: the stack negotiates on connect.
            client = BleakClient(address, timeout=self._timeout_s)
        try:
            await client.connect()
        except Exception as exc:  # noqa: BLE001 - backend errors are untyped; translated below
            raise _translate(exc) from exc
        self._client = client
        # Read-back verification (D8): the check runs no matter who negotiated.
        self._mtu = int(getattr(client, "mtu_size", 0) or 0)
        return await self._read_identity(address)

    async def _read_identity(self, address: str) -> DeviceInfo:
        """DIS reads (unencrypted rows, BLE_GATT §3): serial is the device_id;
        a failed DIS read degrades to empty strings, never to fabricated values."""
        device_id = ""
        try:
            device_id = decode_serial(await self.read(ports.SERIAL_NUMBER_UUID))
        except Exception:  # noqa: BLE001 - fall back to the address (§2), then give up
            device_id = _normalize_address(address)
            if not device_id:
                raise
        fw = hw = sw = ""
        for char, sink in (
            (ports.FIRMWARE_REVISION_UUID, "fw"),
            (ports.HARDWARE_REVISION_UUID, "hw"),
            (ports.SOFTWARE_REVISION_UUID, "sw"),
        ):
            try:
                value = decode_revision(await self.read(char))
            except Exception:  # noqa: BLE001 - DIS extras are optional
                value = ""
            if sink == "fw":
                fw = value
            elif sink == "hw":
                hw = value
            else:
                sw = value
        return DeviceInfo(device_id=device_id, fw=fw, hw=hw, sw=sw)

    async def read(self, char: str) -> bytes:
        self._require_client()
        try:
            return bytes(await self._client.read_gatt_char(char))
        except Exception as exc:  # noqa: BLE001 - translated
            raise _translate(exc) from exc

    async def write(self, char: str, data: bytes) -> None:
        self._require_client()
        try:
            await self._client.write_gatt_char(char, data, response=True)
        except Exception as exc:  # noqa: BLE001 - translated
            raise _translate(exc) from exc

    async def start_notify(self, char: str, cb: Callable[[bytes], None]) -> None:
        self._require_client()

        def _handler(_sender: object, data: bytearray) -> None:
            cb(bytes(data))

        try:
            await self._client.start_notify(char, _handler)
        except Exception as exc:  # noqa: BLE001 - translated (CCCD writes need encryption)
            raise _translate(exc) from exc

    async def stop_notify(self, char: str) -> None:
        self._require_client()
        await self._client.stop_notify(char)

    async def disconnect(self) -> None:
        """Nothing is kept across a dropped link (BLE_GATT §5)."""
        client, self._client = self._client, None
        self._mtu = 0
        if client is not None and client.is_connected:
            await client.disconnect()

    def _require_client(self) -> None:
        if self._client is None:
            raise AssertionError("transport used before connect()")


async def bleak_scan(timeout_s: float = SCAN_TIMEOUT_S) -> list[ports.ScanResult]:
    """Default scanner: discover around the service UUID, map to ScanResult.

    The `service_uuids` filter is a macOS-backend hint elsewhere ignored `[ASSUMED]`;
    the app-level filter (`matches_scan` in bench) always runs as well.
    """
    from bleak import BleakScanner

    try:
        devices = await BleakScanner.discover(
            timeout=timeout_s, service_uuids=[ports.GATT_SERVICE_UUID]
        )
    except TypeError:
        devices = await BleakScanner.discover(timeout=timeout_s)
    results: list[ports.ScanResult] = []
    for device in devices:
        raw_uuids = (device.metadata or {}).get("uuids") or []
        uuids = tuple(str(u).lower() for u in raw_uuids)
        results.append(
            ports.ScanResult(name=device.name or "", address=str(device.address), uuids=uuids)
        )
    return results
