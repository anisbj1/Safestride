// CSV row splitting for bw_testkit's replay fakes (F00 spec §7: this is the one
// non-header file in the library). Host-only — guarded by bw/testkit/host_only.h
// through the header.

#include <bw/testkit/csv_row.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace bw::testkit {
namespace {

bool fieldEquals(const CsvField& field, const char* literal) {
    const size_t literal_size = std::strlen(literal);
    return field.size == literal_size && std::memcmp(field.data, literal, literal_size) == 0;
}

// Whole field must be consumed by strtof (endptr lands exactly on the field's
// end — splitCsvRow guarantees that position is ',' or end-of-line, so parsing
// cannot run into the next field). The charset scan rejects whitespace, the
// letters of "nan"/"inf" and hex floats ("0x10" — strtof would accept those but
// the capture format never emits them); the isfinite check rejects overflow like
// "1e400". Any deviation is damaged data -> false (Err::Corrupt).
// Locale: tests run under the default C locale, where '.' is the decimal point.
bool fieldToFloat(const CsvField& field, float& out) {
    if (field.size == 0) {
        return false;
    }
    for (size_t i = 0; i < field.size; ++i) {
        const char c = field.data[i];
        const bool allowed =
            (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E';
        if (!allowed) {
            return false;
        }
    }
    char* end = nullptr;
    const float value = std::strtof(field.data, &end);
    if (end != field.data + field.size) {
        return false;
    }
    if (!std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

// Manual digit accumulation instead of strtoull: strtoull accepts "-1" and wraps
// it to 2^64-1, which would turn damaged data into a plausible timestamp.
// Overflow of uint64 is reported as false (Err::Corrupt) instead of wrapping.
bool fieldToU64(const CsvField& field, uint64_t& out) {
    if (field.size == 0) {
        return false;
    }
    uint64_t value = 0;
    for (size_t i = 0; i < field.size; ++i) {
        const char c = field.data[i];
        if (c < '0' || c > '9') {
            return false;
        }
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10) {
            return false;  // would overflow
        }
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

// PPG raw counts are 18-bit ADC readings carried in uint32 (02 §3): anything
// beyond UINT32_MAX cannot be a real count -> Err::Corrupt, never truncated.
bool fieldToU32(const CsvField& field, uint32_t& out) {
    uint64_t value = 0;
    if (!fieldToU64(field, value) || value > UINT32_MAX) {
        return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

}  // namespace

bw::Result<size_t> splitCsvRow(const char* line, size_t size, CsvField* out, size_t max_fields) {
    if (line == nullptr || (out == nullptr && max_fields > 0)) {
        return {bw::Err::InvalidArg, 0};
    }
    size_t field_count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= size; ++i) {
        if (i == size || line[i] == ',') {
            if (field_count < max_fields) {
                out[field_count] = CsvField{line + start, i - start};
            }
            ++field_count;
            start = i + 1;
        }
    }
    return {bw::Err::Ok, field_count};
}

bw::Result<bw::ImuSample> parseImuCsvRow(const char* line, size_t size) {
    bw::ImuSample sample{};  // zeroed: the error path never returns uninitialized bytes
    if (line == nullptr) {
        return {bw::Err::InvalidArg, sample};
    }
    if (size > kMaxCsvLineBytes) {
        return {bw::Err::Overflow, sample};
    }
    if (size == 0) {
        return {bw::Err::InvalidArg, sample};
    }

    CsvField fields[kImuCsvColumns];
    const bw::Result<size_t> split = splitCsvRow(line, size, fields, kImuCsvColumns);
    if (!split.ok()) {
        return {split.err, sample};
    }
    if (split.value < kImuCsvColumns) {
        return {bw::Err::InvalidArg, sample};  // missing columns (F00 §9)
    }
    if (split.value > kImuCsvColumns) {
        return {bw::Err::Corrupt, sample};  // extra columns: malformed row
    }
    if (fields[0].size != 1 || fields[0].data[0] != 'I') {
        return {bw::Err::InvalidArg, sample};  // wrong row tag (a P row, or garbage)
    }

    uint64_t t_ms = 0;
    float accel_x = 0.0f, accel_y = 0.0f, accel_z = 0.0f;
    float gyro_x = 0.0f, gyro_y = 0.0f, gyro_z = 0.0f;
    if (!fieldToU64(fields[1], t_ms)) {
        return {bw::Err::Corrupt, sample};
    }
    if (!fieldToFloat(fields[2], accel_x) || !fieldToFloat(fields[3], accel_y) ||
        !fieldToFloat(fields[4], accel_z) || !fieldToFloat(fields[5], gyro_x) ||
        !fieldToFloat(fields[6], gyro_y) || !fieldToFloat(fields[7], gyro_z)) {
        return {bw::Err::Corrupt, sample};
    }

    sample.t_ms = t_ms;
    sample.accel_g[0] = accel_x;
    sample.accel_g[1] = accel_y;
    sample.accel_g[2] = accel_z;
    sample.gyro_dps[0] = gyro_x;
    sample.gyro_dps[1] = gyro_y;
    sample.gyro_dps[2] = gyro_z;
    return {bw::Err::Ok, sample};
}

bw::Result<bw::PpgSample> parsePpgCsvRow(const char* line, size_t size) {
    bw::PpgSample sample{};  // zeroed: the error path never returns uninitialized bytes
    if (line == nullptr) {
        return {bw::Err::InvalidArg, sample};
    }
    if (size > kMaxCsvLineBytes) {
        return {bw::Err::Overflow, sample};
    }
    if (size == 0) {
        return {bw::Err::InvalidArg, sample};
    }

    CsvField fields[kPpgCsvColumns];
    const bw::Result<size_t> split = splitCsvRow(line, size, fields, kPpgCsvColumns);
    if (!split.ok()) {
        return {split.err, sample};
    }
    if (split.value < kPpgCsvColumns) {
        return {bw::Err::InvalidArg, sample};  // missing columns
    }
    if (split.value > kPpgCsvColumns) {
        return {bw::Err::Corrupt, sample};  // extra columns: malformed row
    }
    if (fields[0].size != 1 || fields[0].data[0] != 'P') {
        return {bw::Err::InvalidArg, sample};  // wrong row tag (an I row, or garbage)
    }

    uint64_t t_ms = 0;
    uint32_t red = 0;
    uint32_t ir = 0;
    if (!fieldToU64(fields[1], t_ms)) {
        return {bw::Err::Corrupt, sample};
    }
    if (!fieldToU32(fields[2], red) || !fieldToU32(fields[3], ir)) {
        return {bw::Err::Corrupt, sample};
    }

    sample.t_ms = t_ms;
    sample.red = red;
    sample.ir = ir;
    // The 02 §6.4 capture row "P,t,red,ir" carries no saturation column, so a
    // replayed sample starts unsaturated; tests enqueue explicit samples to
    // simulate saturation (F23 cares, F01 sets the flag on the wire).
    sample.saturated = false;
    return {bw::Err::Ok, sample};
}

bool isImuCsvHeader(const char* line, size_t size) {
    if (line == nullptr || size == 0) {
        return false;
    }
    CsvField fields[2];
    const bw::Result<size_t> split = splitCsvRow(line, size, fields, 2);
    return split.ok() && split.value >= 2 && fields[0].size == 1 && fields[0].data[0] == 'I' &&
           fieldEquals(fields[1], "t");
}

bool isPpgCsvHeader(const char* line, size_t size) {
    if (line == nullptr || size == 0) {
        return false;
    }
    CsvField fields[2];
    const bw::Result<size_t> split = splitCsvRow(line, size, fields, 2);
    return split.ok() && split.value >= 2 && fields[0].size == 1 && fields[0].data[0] == 'P' &&
           fieldEquals(fields[1], "t");
}

bool CsvLineReader::next(const char*& line, size_t& size) {
    if (cursor_ == nullptr || *cursor_ == '\0') {
        return false;
    }
    line = cursor_;
    const char* p = cursor_;
    while (*p != '\0' && *p != '\n') {
        ++p;
    }
    size = static_cast<size_t>(p - line);
    if (size > 0 && line[size - 1] == '\r') {
        --size;  // tolerate CRLF fixtures (captured on Windows)
    }
    cursor_ = (*p == '\n') ? p + 1 : p;
    return true;
}

}  // namespace bw::testkit
