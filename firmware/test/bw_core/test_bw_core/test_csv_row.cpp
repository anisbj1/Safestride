#include <bw/testkit/fake_imu.h>
#include <bw/testkit/fake_ppg.h>

#include <unity.h>

#include <cstring>

// CSV replay contract (F00 §9, row "test_csv_row.cpp (bw_testkit)"):
//  - FakeImu replays a committed fixture CSV deterministically — same timestamps,
//    same sample count on every run;
//  - malformed row -> Err::Corrupt;
//  - over-long row -> Err::Overflow (kMaxCsvLineBytes);
//  - missing columns -> Err::InvalidArg;
//  - never a crash, whatever the input.
//
// The fixture lives in this source (not a data file) so a fresh clone replays the
// exact same bytes with no file I/O — see the [ASSUMED] placement note in the
// spec's status log: §9 lists this file under bw_testkit, but Owns grants
// firmware/test/bw_core/** only.

namespace {

// Header + 5 IMU rows, 02 §6.4 capture format. t_ms steps of 20 ms = 50 Hz (01 §3.3).
// Array (not pointer) so sizeof() in tests is the real text length including NUL.
const char kImuFixture[] = "I,t,ax,ay,az,gx,gy,gz\n"
                           "I,1000,0.00,-0.01,0.99,0.50,-0.50,0.00\n"
                           "I,1020,0.01,0.00,1.00,-0.20,0.10,0.30\n"
                           "I,1040,0.02,-0.02,0.98,1.00,0.00,-0.10\n"
                           "I,1060,0.00,0.01,1.01,0.00,0.20,0.00\n"
                           "I,1080,-0.01,0.00,0.99,-0.30,0.00,0.10\n";

const size_t kFixtureRows = 5;

// Header + 2 PPG rows (02 §6.4 "P,t,red,ir").
const char kPpgFixture[] = "P,t,red,ir\n"
                           "P,1000,12000,150000\n"
                           "P,1010,12500,151000\n";

const size_t kPpgFixtureRows = 2;

}  // namespace

void test_csv_split_yields_fields_without_copying(void) {
    char line[] = "I,1000,0.5";
    bw::testkit::CsvField fields[4];

    const bw::Result<size_t> split = bw::testkit::splitCsvRow(line, std::strlen(line), fields, 4);
    TEST_ASSERT_TRUE(split.ok());
    TEST_ASSERT_EQUAL_UINT32(3, split.value);  // total count may exceed the output array
    TEST_ASSERT_EQUAL_UINT(1, fields[0].size);
    TEST_ASSERT_EQUAL_INT('I', fields[0].data[0]);
    TEST_ASSERT_EQUAL_UINT(4, fields[1].size);
    TEST_ASSERT_TRUE(std::memcmp(fields[1].data, "1000", 4) == 0);
    // Views point into the caller's buffer (no copy): mutating the line moves the field.
    line[2] = 'X';
    TEST_ASSERT_EQUAL_INT('X', fields[1].data[0]);

    // Fewer output slots than fields: the extra fields are counted, not written.
    bw::testkit::CsvField one[1];
    const bw::Result<size_t> capped = bw::testkit::splitCsvRow(line, std::strlen(line), one, 1);
    TEST_ASSERT_TRUE(capped.ok());
    TEST_ASSERT_EQUAL_UINT32(3, capped.value);

    // Null line is a caller mistake, not data damage.
    const bw::Result<size_t> null_line = bw::testkit::splitCsvRow(nullptr, 5, fields, 4);
    TEST_ASSERT_FALSE(null_line.ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg), static_cast<int>(null_line.err));
}

void test_csv_fixture_replays_deterministically(void) {
    bw::testkit::FakeImu<16> imu;
    bw::ImuSample first[kFixtureRows];
    bw::ImuSample second[kFixtureRows];

    const bw::Result<size_t> load1 = imu.loadCsv(kImuFixture);
    TEST_ASSERT_TRUE(load1.ok());
    TEST_ASSERT_EQUAL_UINT32(kFixtureRows, load1.value);  // header skipped
    TEST_ASSERT_EQUAL_UINT32(kFixtureRows, imu.remaining());

    bw::Result<size_t> read = imu.read(first, kFixtureRows);
    TEST_ASSERT_TRUE(read.ok());
    TEST_ASSERT_EQUAL_UINT32(kFixtureRows, read.value);
    TEST_ASSERT_EQUAL_UINT32(0, imu.remaining());

    // Reload the SAME fixture: identical count, timestamps and values.
    const bw::Result<size_t> load2 = imu.loadCsv(kImuFixture);
    TEST_ASSERT_TRUE(load2.ok());
    TEST_ASSERT_EQUAL_UINT32(kFixtureRows, load2.value);
    read = imu.read(second, kFixtureRows);
    TEST_ASSERT_TRUE(read.ok());
    TEST_ASSERT_EQUAL_UINT32(kFixtureRows, read.value);

    // Pinned expectations — not just self-equality, so a parser drift is caught.
    const uint64_t expected_t[kFixtureRows] = {1000, 1020, 1040, 1060, 1080};
    for (size_t i = 0; i < kFixtureRows; ++i) {
        TEST_ASSERT_EQUAL_UINT64(expected_t[i], first[i].t_ms);
        TEST_ASSERT_EQUAL_UINT64(expected_t[i], second[i].t_ms);
        for (size_t axis = 0; axis < 3; ++axis) {
            TEST_ASSERT_FLOAT_WITHIN(1e-6f, first[i].accel_g[axis], second[i].accel_g[axis]);
            TEST_ASSERT_FLOAT_WITHIN(1e-6f, first[i].gyro_dps[axis], second[i].gyro_dps[axis]);
        }
    }
    // Spot-check parsed values against the fixture text itself.
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -0.01f, first[0].accel_g[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.99f, first[0].accel_g[2]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -0.30f, first[4].gyro_dps[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.01f, first[3].accel_g[2]);
}

void test_csv_malformed_row_returns_corrupt(void) {
    bw::testkit::FakeImu<16> imu;

    // Right column count, damaged data: non-numeric, trailing junk, extra column,
    // a timestamp that would wrap, an empty field.
    const char* const corrupt_rows[] = {
        "I,abc,0.1,0.2,1.0,0,0,0",      // t_ms is not a number
        "I,1000,0.1,0.2,1.0,0,0,0xyz",  // field does not parse fully
        "I,1000,0.1,0.2,1.0,0,0,0,9",   // 9 columns (format has 8)
        "I,-5,0.1,0.2,1.0,0,0,0",       // negative t_ms would wrap
        "I,1000,,0.2,1.0,0,0,0",        // empty field
        "I,1000,0.1,0.2,1.0,0,0,nan",   // not finite sensor data
    };
    for (const char* row : corrupt_rows) {
        const bw::Result<bw::ImuSample> parsed = bw::testkit::parseImuCsvRow(row, std::strlen(row));
        TEST_ASSERT_FALSE(parsed.ok());
        TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::Corrupt), static_cast<int>(parsed.err));
    }

    // Through loadCsv the whole file must fail AND stay unloaded (all-or-nothing).
    static const char kBadRow[] = "I,1100,oops,0,1,0,0,0\n";
    char bad_file[sizeof(kImuFixture) + sizeof(kBadRow)];
    std::memcpy(bad_file, kImuFixture, sizeof(kImuFixture) - 1);
    std::memcpy(bad_file + sizeof(kImuFixture) - 1, kBadRow, sizeof(kBadRow));
    const bw::Result<size_t> load = imu.loadCsv(bad_file);
    TEST_ASSERT_FALSE(load.ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::Corrupt), static_cast<int>(load.err));
    TEST_ASSERT_EQUAL_UINT32(0, imu.remaining());
}

void test_csv_overlong_row_respects_max_line_bytes(void) {
    // Boundary (host-testing: values exactly at the limit): a valid row padded with
    // trailing zeros inside the float field to EXACTLY kMaxCsvLineBytes parses;
    // one byte more is Err::Overflow, never a truncated parse.
    static const char kBase[] = "I,1000,0.0,0.0,1.0,0.0,0.0,0.0";
    const size_t base_len = sizeof(kBase) - 1;  // 31
    char line[bw::testkit::kMaxCsvLineBytes + 2];
    std::memcpy(line, kBase, base_len);
    const size_t pad = bw::testkit::kMaxCsvLineBytes - base_len;
    std::memset(line + base_len, '0', pad);
    const size_t exact_len = base_len + pad;
    line[exact_len] = '\0';
    TEST_ASSERT_EQUAL_UINT32(bw::testkit::kMaxCsvLineBytes, exact_len);

    const bw::Result<bw::ImuSample> at_limit = bw::testkit::parseImuCsvRow(line, exact_len);
    TEST_ASSERT_TRUE(at_limit.ok());
    TEST_ASSERT_EQUAL_UINT64(1000, at_limit.value.t_ms);

    line[exact_len] = '0';  // one byte over the limit
    line[exact_len + 1] = '\0';
    const bw::Result<bw::ImuSample> over_limit = bw::testkit::parseImuCsvRow(line, exact_len + 1);
    TEST_ASSERT_FALSE(over_limit.ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::Overflow), static_cast<int>(over_limit.err));

    // Same contract through the fake: an over-long row fails the whole load
    // atomically, leaving the script untouched.
    bw::testkit::FakeImu<16> imu;
    const bw::Result<size_t> ok_load = imu.loadCsv(kImuFixture);
    TEST_ASSERT_TRUE(ok_load.ok());
    char long_file[sizeof(kImuFixture) + bw::testkit::kMaxCsvLineBytes + 4];
    std::memcpy(long_file, kImuFixture, sizeof(kImuFixture) - 1);
    size_t offset = sizeof(kImuFixture) - 1;
    long_file[offset++] = 'I';
    long_file[offset++] = ',';
    std::memset(long_file + offset, '9', bw::testkit::kMaxCsvLineBytes);
    offset += bw::testkit::kMaxCsvLineBytes;
    long_file[offset] = '\0';
    const bw::Result<size_t> bad_load = imu.loadCsv(long_file);
    TEST_ASSERT_FALSE(bad_load.ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::Overflow), static_cast<int>(bad_load.err));
    TEST_ASSERT_EQUAL_UINT32(kFixtureRows, imu.remaining());  // nothing added, nothing lost
}

void test_csv_missing_columns_returns_invalid_arg(void) {
    // Too few columns, wrong row tag and empty input are ARGUMENT problems — the
    // caller passed a row this parser cannot take — distinct from Err::Corrupt
    // (damaged data in a structurally valid row).
    const char* const invalid_rows[] = {
        "I,1000,0.1,0.2",          // 4 of 8 columns
        "I,1000,0.1,0.2,1.0,0,0",  // 7 of 8 columns
        "P,1000,12000,150000",     // P row passed to the IMU parser
        "hello",                   // not a row at all
        "",                        // empty line
    };
    for (const char* row : invalid_rows) {
        const bw::Result<bw::ImuSample> parsed = bw::testkit::parseImuCsvRow(row, std::strlen(row));
        TEST_ASSERT_FALSE(parsed.ok());
        TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg), static_cast<int>(parsed.err));
    }

    // The header itself is not a data row (it has the right shape but must be
    // skipped by loadCsv, not parsed by parseImuCsvRow — "t" is not a number, so
    // parsing it as data is Corrupt; loadCsv uses isImuCsvHeader and never does).
    bw::testkit::FakeImu<16> imu;
    TEST_ASSERT_EQUAL_UINT32(kFixtureRows, imu.loadCsv(kImuFixture).value);

    // PPG parser mirrors the mapping.
    const char* const ppg_short_row = "P,1000,12000";
    const bw::Result<bw::PpgSample> ppg_short =
        bw::testkit::parsePpgCsvRow(ppg_short_row, std::strlen(ppg_short_row));
    TEST_ASSERT_FALSE(ppg_short.ok());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg), static_cast<int>(ppg_short.err));
}

void test_csv_garbage_never_crashes(void) {
    // Fuzz-ish sweep: every input must return a Result — never crash, never hang.
    const char* const junk[] = {
        "",
        ",",
        ",,,,,,,,",                            // 9 empty fields
        "I",                                   // bare tag
        "II,1000,0,0,1,0,0,0",                 // two-char tag
        "\x01\x02\x03",                        // control bytes
        "I,18446744073709551616,0,0,1,0,0,0",  // t_ms one past UINT64_MAX
        "I,1000,1e400,0,1,0,0,0",              // float overflow -> inf
        "I,1000,0x10,0,1,0,0,0",               // hex field the format never emits
    };
    for (const char* row : junk) {
        const bw::Result<bw::ImuSample> parsed = bw::testkit::parseImuCsvRow(row, std::strlen(row));
        TEST_ASSERT_FALSE(parsed.ok());
        const int err = static_cast<int>(parsed.err);
        const bool expected_family = err == static_cast<int>(bw::Err::InvalidArg) ||
                                     err == static_cast<int>(bw::Err::Corrupt) ||
                                     err == static_cast<int>(bw::Err::Overflow);
        TEST_ASSERT_TRUE(expected_family);
    }

    // Garbage at file level: empty text is an ordinary empty file (Ok, 0 rows),
    // null text is a caller mistake.
    bw::testkit::FakeImu<16> imu;
    const bw::Result<size_t> empty_file = imu.loadCsv("");
    TEST_ASSERT_TRUE(empty_file.ok());
    TEST_ASSERT_EQUAL_UINT32(0, empty_file.value);
    const bw::Result<size_t> null_text = imu.loadCsv(nullptr);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(bw::Err::InvalidArg), static_cast<int>(null_text.err));
    TEST_ASSERT_EQUAL_UINT32(0, imu.remaining());
}

void test_csv_ppg_fixture_replays_deterministically(void) {
    bw::testkit::FakePpg<8> ppg;
    bw::PpgSample samples[kPpgFixtureRows];

    const bw::Result<size_t> load = ppg.loadCsv(kPpgFixture);
    TEST_ASSERT_TRUE(load.ok());
    TEST_ASSERT_EQUAL_UINT32(kPpgFixtureRows, load.value);  // header skipped
    TEST_ASSERT_EQUAL_UINT32(kPpgFixtureRows, ppg.remaining());

    const bw::Result<size_t> read = ppg.read(samples, kPpgFixtureRows);
    TEST_ASSERT_TRUE(read.ok());
    TEST_ASSERT_EQUAL_UINT32(kPpgFixtureRows, read.value);

    TEST_ASSERT_EQUAL_UINT64(1000, samples[0].t_ms);
    TEST_ASSERT_EQUAL_UINT32(12000, samples[0].red);
    TEST_ASSERT_EQUAL_UINT32(150000, samples[0].ir);
    TEST_ASSERT_EQUAL_UINT64(1010, samples[1].t_ms);
    TEST_ASSERT_EQUAL_UINT32(12500, samples[1].red);
    TEST_ASSERT_EQUAL_UINT32(151000, samples[1].ir);
    // 02 §6.4's row carries no saturation column: replayed samples start clean.
    TEST_ASSERT_FALSE(samples[0].saturated);
    TEST_ASSERT_FALSE(samples[1].saturated);
}
