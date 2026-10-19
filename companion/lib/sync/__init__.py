"""F20 sync package — stable entrypoints for the composition root (D12).

Owned by F20; `companion/lib/app.py`, `bench.py`, `README.md` and
`pyproject.toml` are the integrator's composition root and are wired at P4'
(never by this feature):

1. ``build_sync_port(transport)`` — primary builder; takes the **live**
   `GattTransport` (F21's zero-arg `PortFactory` cannot reach the transport a
   bench/app created later, so a bare closure is not enough).
2. ``bound_port_factory(transport_factory)`` — adapter pair for F21's
   `bench.main(port_factory=...)` seam with zero F21 edits: the wrapped
   transport factory captures the instance, the port factory binds the client
   to it (honest failure if the ordering ever changes).
3. ``companion.sync.runner.main(argv)`` / ``run_sync(...)`` — the real
   long-transfer driver (`bench.py` line 31 defers to it); it reuses F21's
   `SyncControlViewModel` as the single orchestration logic (no second loop).
"""

from __future__ import annotations

import pathlib
import sys
import types
from collections.abc import Callable

# Source-layout bootstrap - the same guarded `companion` -> ./lib mapping as
# tests/conftest.py and F21's tests/conftest.py. pytest's prepend import mode
# walks up into this package as `lib.sync` (tests/ became a package to fix
# audit F20-AUD-3, same defect as F16-AUD-1), executing this __init__ *before*
# any conftest body could map the package, so the package maps it itself. The
# installed layout already has `companion` imported with a matching __path__ -
# the guard keeps it.
_LIB = pathlib.Path(__file__).resolve().parents[1]
_existing = sys.modules.get("companion")
if _existing is None or list(getattr(_existing, "__path__", [])) != [str(_LIB)]:
    _pkg = types.ModuleType("companion")
    _pkg.__path__ = [str(_LIB)]  # namespace-style package rooted at the source tree
    _pkg.__doc__ = "anarolepsy companion package (source layout, used by sync)"
    sys.modules["companion"] = _pkg

from companion.sync.client import SyncClient, SyncMtuError  # noqa: E402
from companion.transport.ports import GattTransport  # noqa: E402

__all__ = ["SyncClient", "SyncMtuError", "bound_port_factory", "build_sync_port"]

TransportFactory = Callable[[], GattTransport]
PortFactory = Callable[[], SyncClient]


def build_sync_port(transport: GattTransport) -> SyncClient:
    """D12(1): wrap a connected-or-connecting transport in the real SyncPort."""
    return SyncClient(transport)


def bound_port_factory(transport_factory: TransportFactory) -> tuple[TransportFactory, PortFactory]:
    """D12(2): adapt F21's zero-arg `PortFactory` to a transport-aware builder.

    Returns `(transport_factory, port_factory)`: pass the first wherever
    `bench.main`/`app.main` expects a transport factory and the second as its
    `port_factory`. The port factory raises (honest failure, D12) if no
    transport has been created yet — composition roots always connect first.
    """
    holder: list[GattTransport | None] = [None]

    def make_transport() -> GattTransport:
        transport = transport_factory()
        holder[0] = transport
        return transport

    def make_port() -> SyncClient:
        transport = holder[0]
        if transport is None:
            raise RuntimeError(
                "bound_port_factory: port requested before the transport was created "
                "(the wrapped transport_factory must run first — bench/app ordering, D12)"
            )
        return SyncClient(transport)

    return make_transport, make_port
