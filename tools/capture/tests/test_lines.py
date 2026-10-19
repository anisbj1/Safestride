"""Golden vectors for the schema v1 line grammar.

These literals are the SAME as the C++ golden tests
(firmware/test/bw_research/test_bw_research/test_line_format.cpp) and as
DATASET_FORMAT.md §5, so a formatter/parser drift on either side fails here.
"""

from __future__ import annotations

import pytest

from capture import lines

GOLDEN = [
    (
        "#HDR,1,7,0.1.0,2,3,4,5",
        "header",
        {
            "schema_version": 1,
            "boot_id": 7,
            "firmware_version": "0.1.0",
            "imu_drops": 2,
            "ppg_drops": 3,
            "line_drops": 4,
            "feature_missed": 5,
        },
    ),
    (
        "I,1000,0.0012,-0.0034,0.9988,1.5,-2.3,0.0",
        "imu",
        {
            "t_ms": 1000,
            "ax_g": 0.0012,
            "ay_g": -0.0034,
            "az_g": 0.9988,
            "gx_dps": 1.5,
            "gy_dps": -2.3,
            "gz_dps": 0.0,
        },
    ),
    (
        "P,2000,123456,654321",
        "ppg",
        {"t_ms": 2000, "red": 123456, "ir": 654321},
    ),
    (
        "F,M,5000,Good,0.0123,0.0456,0.0789,12.3,45.6,1.250,700",
        "motion",
        {
            "t_ms": 5000,
            "quality": "Good",
            "enmo_g": 0.0123,
            "mag_std_g": 0.0456,
            "jerk_rms_g_per_s": 0.0789,
            "gyro_rms_dps": 12.3,
            "gravity_tilt_deg": 45.6,
            "dominant_freq_hz": 1.25,
            "stillness_ms": 700,
        },
    ),
    (
        "F,P,6000,Degraded,1,63.5,0.875,0.04567,12345.6,0.900,0,0,0.0",
        "ppg_features",
        {
            "t_ms": 6000,
            "quality": "Degraded",
            "hr_valid": True,
            "hr_bpm": 63.5,
            "hr_confidence": 0.875,
            "perfusion_index": 0.04567,
            "ir_dc_level": 12345.6,
            "sqi": 0.9,
            "motion_corrupted": False,
            "hrv_valid": False,
            "rmssd_ms": 0.0,
        },
    ),
    (
        "F,C,7000,Walking,0.750",
        "context",
        {"t_ms": 7000, "activity": "Walking", "confidence": 0.75},
    ),
    (
        "E,3,123456789,7,5000,1700000000,0.500,4100,deadbeef00010203,Good,"
        "Insufficient,UserMarked,Confirmed,Walking,Awake,5,1",
        "event",
        {
            "seq": 3,
            "t_start_ms": 123456789,
            "boot_id": 7,
            "duration_ms": 5000,
            "t_start_unix": 1700000000,
            "confidence": 0.5,
            "reasons": 4100,
            "payload_hex": "deadbeef00010203",
            "quality_imu": "Good",
            "quality_ppg": "Insufficient",
            "type": "UserMarked",
            "label": "Confirmed",
            "activity": "Walking",
            "alertness": "Awake",
            "detector_id": 5,
            "schema_version": 1,
        },
    ),
    (
        "M,8000,user_marked#42",
        "marker",
        {"t_ms": 8000, "marker": "user_marked#42"},
    ),
]


@pytest.mark.parametrize(("text", "kind", "row"), GOLDEN, ids=[g[0][:12] for g in GOLDEN])
def test_golden_lines_parse(text: str, kind: str, row: dict[str, object]) -> None:
    parsed_kind, parsed_row = lines.parse_line(text)
    assert parsed_kind == kind
    assert parsed_row == row


@pytest.mark.parametrize(
    "text",
    [
        "#HDR,1,7,0.1.0,2,3,4,5",
        "I,1000,0.0012,-0.0034,0.9988,1.5,-2.3,0.0",
        "P,2000,123456,654321",
        "F,M,5000,Good,0.0123,0.0456,0.0789,12.3,45.6,1.250,700",
        "F,C,7000,Walking,0.750",
        "M,8000,user_marked#42",
    ],
)
def test_research_lines_are_classified_as_research(text: str) -> None:
    assert lines.is_research_line(text)


@pytest.mark.parametrize(
    "text",
    [
        "",  # empty line
        "[PIPE] window t=1000 motion=Stationary",  # bench diagnostic
        "[BTN] press 1",
        "BOOT v0.1.0",  # boot banner
        "Email,5",  # looks like E-row but is not ("Em,")
        "F,X,0,Good",  # F row with unknown subtype: diagnostics, not research
        "F02 says hi",
        "IMU warmup",  # "I" not followed by ','
    ],
)
def test_non_research_lines_are_diverted(text: str) -> None:
    assert not lines.is_research_line(text)


@pytest.mark.parametrize(
    "text",
    [
        "I,abc,1,2,3,4,5,6",  # t_ms not an integer
        "I,0,1,2,3,4,5",  # wrong field count
        "P,-1,2,3",  # negative t_ms
        "F,M,0,Bogus,0.0,0.0,0.0,0.0,0.0,0.0,0",  # unknown enum name = schema drift
        "F,P,0,Good,2,63.5,0.875,0.05,100.0,0.9,0,0,0.0",  # hr_valid not 0/1
        "F,C,0,Walking,high",  # confidence not a number
        "E,1,2,3,4,5,0.5,0,zz,Good,Good,Boot,Unlabeled,Unknown,Unknown,1,1",  # payload
        "M,5,",  # empty marker
        "M,x,user_marked#1",  # t_ms not an integer
    ],
)
def test_malformed_research_rows_raise(text: str) -> None:
    with pytest.raises(lines.MalformedRow):
        lines.parse_line(text)


def test_unknown_schema_raises_schema_mismatch() -> None:
    with pytest.raises(lines.SchemaMismatch):
        lines.parse_line("#HDR,2,0,unknown,0,0,0,0")


def test_line_endings_are_tolerated() -> None:
    kind, row = lines.parse_line("F,C,7000,Walking,0.750\r\n")
    assert kind == "context"
    assert row["confidence"] == 0.75
