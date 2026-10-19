"""Transport wiring checks (F28 audit A5, loop 2): the named constants are
the single source of truth for the serial defaults — no literal drift."""

from __future__ import annotations

import inspect

from capture import transport


def test_serial_default_timeout_is_the_named_constant() -> None:
    # The default must REFERENCE the named constant, not repeat its value.
    # An identity check cannot be the guard: CPython reuses the cached 1.0
    # float object even when the signature holds a fresh literal, so this
    # pins the constant's name in the __init__ source instead (reverting to
    # `timeout: float = 1.0` removes it and fails here).
    init_source = inspect.getsource(transport.SerialTransport.__init__)
    assert "SERIAL_READ_TIMEOUT_S" in init_source
    default = inspect.signature(transport.SerialTransport.__init__).parameters["timeout"].default
    assert default == transport.SERIAL_READ_TIMEOUT_S == 1.0  # value also pinned
