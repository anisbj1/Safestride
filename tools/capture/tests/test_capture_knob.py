"""P4 capture knob — build-time enable exists, repo default stays OFF (spec §8/D4).

The DoD §14 capture build must be able to turn research on WITHOUT editing the
repository (the 2026-10-11 bench procedure flipped the constant by hand and
reverted it — drift-prone). This pins the knob's *source shape*, the same
style as the SERIAL_READ_TIMEOUT_S pin (H10): revert `research_config.h` to a
hard-coded `= false` and the last assertion fails.
"""

from pathlib import Path

_CONFIG_H = (
    Path(__file__).resolve().parents[3]
    / "firmware"
    / "lib"
    / "bw_research"
    / "include"
    / "bw"
    / "research"
    / "research_config.h"
)


def _config_source() -> str:
    return _CONFIG_H.read_text(encoding="utf-8")


def test_capture_knob_is_a_01_build_flag() -> None:
    src = _config_source()
    assert "#ifndef F28_RESEARCH_CAPTURE_ENABLED" in src
    assert "#define F28_RESEARCH_CAPTURE_ENABLED 0" in src
    assert (
        "kResearchCaptureDefaultEnabled = (F28_RESEARCH_CAPTURE_ENABLED != 0)" in src
    ), "kResearchCaptureDefaultEnabled must resolve from the build flag"


def test_capture_knob_rejects_non_binary_values() -> None:
    # The static_assert is the compile-time guard for -DF28_...=yes typos.
    src = _config_source()
    assert "static_assert(F28_RESEARCH_CAPTURE_ENABLED == 0" in src
    assert "== 1," in src.split("static_assert(F28_RESEARCH_CAPTURE_ENABLED == 0", 1)[1]
