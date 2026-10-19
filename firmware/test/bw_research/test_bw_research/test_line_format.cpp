// F28 §9.1 — golden tests for the serial line grammar. The same literals are
// mirrored in docs/contracts/DATASET_FORMAT.md (test vectors) and parsed by
// tools/capture/tests/test_lines.py, so the device formatter, the contract doc
// and the host parser stay pinned to one grammar (D9/D10).

#include <cstdio>
#include <cstring>

#include <bw/research/line_format.h>
#include <bw/research/research_config.h>

#include <unity.h>

void test_header_golden(void) {
    bw::research::HeaderInfo info{};
    info.schema_version = 1;
    info.boot_id = 7;
    info.firmware_version = "0.1.0";
    info.imu_drops = 2;
    info.ppg_drops = 3;
    info.line_drops = 4;
    info.feature_missed = 5;

    char line[bw::research::kMaxLineLen];
    const size_t n = bw::research::formatHeader(line, sizeof(line), info);
    TEST_ASSERT_EQUAL_UINT(strlen("#HDR,1,7,0.1.0,2,3,4,5\n"), n);
    TEST_ASSERT_EQUAL_STRING("#HDR,1,7,0.1.0,2,3,4,5\n", line);
}

void test_header_sanitises_firmware_version(void) {
    // D6: ',' and newlines would change the field count the parser relies on.
    bw::research::HeaderInfo info{};
    info.schema_version = 1;
    info.firmware_version = "0.1.0,beta\n";

    char line[bw::research::kMaxLineLen];
    bw::research::formatHeader(line, sizeof(line), info);
    TEST_ASSERT_EQUAL_STRING("#HDR,1,0,0.1.0_beta_,0,0,0,0\n", line);
}

void test_header_null_version_is_unknown(void) {
    bw::research::HeaderInfo info{};
    info.schema_version = 1;
    info.firmware_version = nullptr;

    char line[bw::research::kMaxLineLen];
    bw::research::formatHeader(line, sizeof(line), info);
    TEST_ASSERT_EQUAL_STRING("#HDR,1,0,unknown,0,0,0,0\n", line);
}

void test_header_truncates_long_version(void) {
    // Fixed field length keeps the grammar stable (kMaxFirmwareVersionLen).
    char version[64];
    for (size_t i = 0; i < sizeof(version) - 1; ++i) {
        version[i] = 'x';
    }
    version[sizeof(version) - 1] = '\0';

    bw::research::HeaderInfo info{};
    info.schema_version = 1;
    info.firmware_version = version;

    char line[bw::research::kMaxLineLen];
    bw::research::formatHeader(line, sizeof(line), info);
    // "#HDR," + schema "1," + boot_id "0," + 32 x + ",0,0,0,0\n"
    TEST_ASSERT_EQUAL_STRING("#HDR,1,0,xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx,0,0,0,0\n", line);
}

void test_imu_line_golden(void) {
    bw::ImuSample sample{};
    sample.t_ms = 1000;
    sample.accel_g[0] = 0.0012f;
    sample.accel_g[1] = -0.0034f;
    sample.accel_g[2] = 0.9988f;
    sample.gyro_dps[0] = 1.5f;
    sample.gyro_dps[1] = -2.3f;
    sample.gyro_dps[2] = 0.0f;

    char line[bw::research::kMaxLineLen];
    bw::research::formatImu(line, sizeof(line), sample);
    TEST_ASSERT_EQUAL_STRING("I,1000,0.0012,-0.0034,0.9988,1.5,-2.3,0.0\n", line);
}

void test_ppg_line_golden(void) {
    bw::PpgSample sample{};
    sample.t_ms = 2000;
    sample.red = 123456;
    sample.ir = 654321;
    sample.saturated = false;

    char line[bw::research::kMaxLineLen];
    bw::research::formatPpg(line, sizeof(line), sample);
    TEST_ASSERT_EQUAL_STRING("P,2000,123456,654321\n", line);
}

void test_motion_feature_line_golden(void) {
    bw::MotionFeatures features{};
    features.t_ms = 5000;
    features.quality = bw::DataQuality::Good;
    features.enmo_g = 0.0123f;
    features.mag_std_g = 0.0456f;
    features.jerk_rms_g_per_s = 0.0789f;
    features.gyro_rms_dps = 12.3f;
    features.gravity_tilt_deg = 45.6f;
    features.dominant_freq_hz = 1.25f;
    features.stillness_ms = 700;

    char line[bw::research::kMaxLineLen];
    bw::research::formatMotionFeatures(line, sizeof(line), features);
    TEST_ASSERT_EQUAL_STRING("F,M,5000,Good,0.0123,0.0456,0.0789,12.3,45.6,1.250,700\n", line);
}

void test_ppg_feature_line_golden(void) {
    bw::PpgFeatures features{};
    features.t_ms = 6000;
    features.quality = bw::DataQuality::Degraded;
    features.hr_valid = true;
    features.hr_bpm = 63.5f;
    features.hr_confidence = 0.875f;
    features.perfusion_index = 0.04567f;
    features.ir_dc_level = 12345.6f;
    features.sqi = 0.9f;
    features.motion_corrupted = false;
    features.hrv_valid = false;
    features.rmssd_ms = 0.0f;

    char line[bw::research::kMaxLineLen];
    bw::research::formatPpgFeatures(line, sizeof(line), features);
    TEST_ASSERT_EQUAL_STRING("F,P,6000,Degraded,1,63.5,0.875,0.04567,12345.6,0.900,0,0,0.0\n",
                             line);
}

void test_context_line_golden(void) {
    bw::ActivityEstimate estimate{};
    estimate.t_ms = 7000;
    estimate.context = bw::ActivityContext::Walking;
    estimate.confidence = 0.75f;

    char line[bw::research::kMaxLineLen];
    bw::research::formatContext(line, sizeof(line), estimate);
    TEST_ASSERT_EQUAL_STRING("F,C,7000,Walking,0.750\n", line);
}

void test_marker_line_golden(void) {
    char line[bw::research::kMaxLineLen];
    const size_t n = bw::research::formatMarker(line, sizeof(line), 8000, 42);
    TEST_ASSERT_EQUAL_UINT(strlen("M,8000,user_marked#42\n"), n);
    TEST_ASSERT_EQUAL_STRING("M,8000,user_marked#42\n", line);
}

void test_event_line_golden(void) {
    bw::Event event{};
    event.t_start_ms = 123456789;
    event.seq = 3;
    event.boot_id = 7;
    event.duration_ms = 5000;
    event.t_start_unix = 1700000000;
    event.confidence = 0.5f;
    event.reasons = 4100;
    const uint8_t payload[bw::Event::kMaxPayloadBytes] = {0xDE, 0xAD, 0xBE, 0xEF,
                                                          0x00, 0x01, 0x02, 0x03};
    for (size_t i = 0; i < bw::Event::kMaxPayloadBytes; ++i) {
        event.payload[i] = payload[i];
    }
    event.quality = {bw::DataQuality::Good, bw::DataQuality::Insufficient};
    event.type = bw::EventType::UserMarked;
    event.label = bw::UserLabel::Confirmed;
    event.context = bw::ActivityContext::Walking;
    event.alertness_at_start = bw::AlertnessState::Awake;
    event.detector_id = 5;
    event.schema_version = bw::Event::kSchemaVersion;

    char line[bw::research::kMaxLineLen];
    bw::research::formatEvent(line, sizeof(line), event);
    TEST_ASSERT_EQUAL_STRING(
        "E,3,123456789,7,5000,1700000000,0.500,4100,deadbeef00010203,Good,Insufficient,"
        "UserMarked,Confirmed,Walking,Awake,5,1\n",
        line);
}

void test_cap_too_small_writes_nothing(void) {
    // F28 §10 / CR-004: refusal, nothing written, caller counts the drop.
    char buffer[8];
    std::memset(buffer, 'x', sizeof(buffer));
    const size_t n = bw::research::formatMarker(buffer, sizeof(buffer), 8000, 42);
    TEST_ASSERT_EQUAL_UINT(0, n);
    for (size_t i = 0; i < sizeof(buffer); ++i) {
        TEST_ASSERT_EQUAL_CHAR('x', buffer[i]);
    }
}

void test_cap_boundary_is_exact(void) {
    // Line is 22 bytes + NUL: cap 22 refuses, cap 23 fits (no off-by-one).
    char exact[22];
    TEST_ASSERT_EQUAL_UINT(0, bw::research::formatMarker(exact, sizeof(exact), 8000, 42));

    char fitting[23];
    TEST_ASSERT_EQUAL_UINT(22, bw::research::formatMarker(fitting, sizeof(fitting), 8000, 42));
    TEST_ASSERT_EQUAL_STRING("M,8000,user_marked#42\n", fitting);
}

void test_event_worst_case_line_fits_max_line_len(void) {
    // kMaxLineLen's arithmetic (research_config.h): this is the longest row the
    // grammar can produce — if it stops fitting, every E row would be dropped.
    bw::Event event{};
    event.t_start_ms = 18446744073709551615ULL;
    event.seq = 4294967295u;
    event.boot_id = 4294967295u;
    event.duration_ms = 4294967295u;
    event.t_start_unix = 4294967295u;
    event.confidence = 1.0f;
    event.reasons = 4294967295u;
    for (size_t i = 0; i < bw::Event::kMaxPayloadBytes; ++i) {
        event.payload[i] = 0xFF;
    }
    event.quality = {bw::DataQuality::Insufficient, bw::DataQuality::Insufficient};
    event.type = bw::EventType::SuspectedCataplexy;
    event.label = bw::UserLabel::Unlabeled;
    event.context = bw::ActivityContext::InactiveProlonged;
    event.alertness_at_start = bw::AlertnessState::HighDrowsinessRisk;
    event.detector_id = 255;
    event.schema_version = 255;

    char line[bw::research::kMaxLineLen];
    const size_t n = bw::research::formatEvent(line, sizeof(line), event);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(n < bw::research::kMaxLineLen);
}

void test_activity_names_table(void) {
    // Append-only enums (02 §6.6): every value needs a stable spelling, else the
    // parser's activity column silently changes meaning. Full-line comparison
    // with a fixed t/conf keeps the whole row shape pinned too.
    struct Case {
        bw::ActivityContext value;
        const char* name;
    };
    static const Case kCases[] = {
        {bw::ActivityContext::Unknown, "Unknown"},
        {bw::ActivityContext::Stationary, "Stationary"},
        {bw::ActivityContext::Sitting, "Sitting"},
        {bw::ActivityContext::Standing, "Standing"},
        {bw::ActivityContext::Walking, "Walking"},
        {bw::ActivityContext::Running, "Running"},
        {bw::ActivityContext::LyingResting, "LyingResting"},
        {bw::ActivityContext::InactiveProlonged, "InactiveProlonged"},
    };

    char line[bw::research::kMaxLineLen];
    char expected[bw::research::kMaxLineLen];
    for (const Case& c : kCases) {
        bw::ActivityEstimate estimate{};
        estimate.t_ms = 0;
        estimate.context = c.value;
        estimate.confidence = 0.0f;
        bw::research::formatContext(line, sizeof(line), estimate);
        std::snprintf(expected, sizeof(expected), "F,C,0,%s,0.000\n", c.name);
        TEST_ASSERT_EQUAL_STRING(expected, line);
    }
}

void test_event_type_names_table(void) {
    // Every EventType must be spelled identically in E rows (and in the Python
    // decoder, DATASET_FORMAT.md "Enum spellings").
    struct Case {
        bw::EventType value;
        const char* name;
    };
    static const Case kCases[] = {
        {bw::EventType::SleepEpisode, "SleepEpisode"},
        {bw::EventType::DrowsinessEvent, "DrowsinessEvent"},
        {bw::EventType::SuspectedCataplexy, "SuspectedCataplexy"},
        {bw::EventType::UserMarked, "UserMarked"},
        {bw::EventType::AlertRaised, "AlertRaised"},
        {bw::EventType::SensorFault, "SensorFault"},
        {bw::EventType::DeviceNotWorn, "DeviceNotWorn"},
        {bw::EventType::BatteryWarning, "BatteryWarning"},
        {bw::EventType::NapPlanned, "NapPlanned"},
        {bw::EventType::NapCompleted, "NapCompleted"},
        {bw::EventType::TimeSync, "TimeSync"},
        {bw::EventType::Boot, "Boot"},
    };

    char line[bw::research::kMaxLineLen];
    char needle[48];
    for (const Case& c : kCases) {
        bw::Event event{};
        event.type = c.value;
        bw::research::formatEvent(line, sizeof(line), event);
        std::snprintf(needle, sizeof(needle), ",%s,", c.name);
        TEST_ASSERT_NOT_NULL(std::strstr(line, needle));
    }
}

void test_quality_names_table(void) {
    struct Case {
        bw::DataQuality value;
        const char* name;
    };
    static const Case kCases[] = {
        {bw::DataQuality::Good, "Good"},
        {bw::DataQuality::Degraded, "Degraded"},
        {bw::DataQuality::Insufficient, "Insufficient"},
    };

    char line[bw::research::kMaxLineLen];
    for (const Case& c : kCases) {
        bw::MotionFeatures features{};
        features.quality = c.value;
        bw::research::formatMotionFeatures(line, sizeof(line), features);
        // "F,M,0,<quality>,..." — quality is the 4th field.
        char expected[bw::research::kMaxLineLen];
        std::snprintf(expected, sizeof(expected), "F,M,0,%s,", c.name);
        TEST_ASSERT_EQUAL_INT(0, std::strncmp(line, expected, std::strlen(expected)));
    }
}
