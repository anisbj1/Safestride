#pragma once

#include <bw/testkit/host_only.h>

#include <bw/hardware.h>
#include <bw/result.h>
#include <bw/types.h>

namespace bw::testkit {

// kMaxCsvLineBytes — [EXPERIMENTAL] (F00 §8 holds the full derivation): fixed line
// bound for FakeImu/FakePpg CSV replay — no heap. The longest row is
// raw_imu.csv's "I,t,ax,ay,az,gx,gy,gz": 20-digit t_ms + 6 floats at ±4 g / ±500 dps
// ≈ 90 chars, so 128 leaves ~40 % headroom. Exceeding it returns Err::Overflow,
// never a truncated parse. Feature rows (up to 32 floats ≈ 420 chars) are out of
// scope — F28 owns that parser.
constexpr size_t kMaxCsvLineBytes = 128;

// Column counts of the serial-capture rows (02 §6.4):
// "I,t,ax,ay,az,gx,gy,gz" and "P,t,red,ir".
constexpr size_t kImuCsvColumns = 8;
constexpr size_t kPpgCsvColumns = 4;

// A view into the caller's own line buffer — no copy, no NUL, no heap.
struct CsvField {
    const char* data;
    size_t size;
};

// Splits one line at ',' into at most max_fields views stored in `out` and returns
// the TOTAL number of fields in the line — which may exceed max_fields, so the
// caller compares the count against what its row format expects. Fields are never
// modified in place (a row of a capture file stays byte-identical while parsing).
bw::Result<size_t> splitCsvRow(const char* line, size_t size, CsvField* out, size_t max_fields);

// Row parsers for the capture formats in 02 §6.4. Error mapping (F00 §9's plan):
//   size > kMaxCsvLineBytes            -> Err::Overflow  (line overran the fixed bound)
//   empty line, wrong row tag, missing columns -> Err::InvalidArg (wrong argument)
//   field present but not a parseable number  -> Err::Corrupt (data is damaged)
//   more columns than the format holds -> Err::Corrupt (malformed row)
// Never crashes: every path checks length and fields before reading them.
bw::Result<bw::ImuSample> parseImuCsvRow(const char* line, size_t size);
bw::Result<bw::PpgSample> parsePpgCsvRow(const char* line, size_t size);

// True for the header lines 02 §6.4 defines: first field is the row tag, second is
// the literal "t" (i.e. "I,t,ax,…" / "P,t,red,ir"). loadCsv() skips these.
bool isImuCsvHeader(const char* line, size_t size);
bool isPpgCsvHeader(const char* line, size_t size);

// Line iterator over NUL-terminated CSV text; yields views without copying.
// Handles "\n" and "\r\n" endings and yields empty lines too (callers skip them).
class CsvLineReader {
public:
    explicit CsvLineReader(const char* text) : cursor_(text) {}

    // Returns false at end of text; otherwise fills line/size with the next line
    // (without its newline). A trailing line without a newline is still yielded.
    bool next(const char*& line, size_t& size);

private:
    const char* cursor_;
};

}  // namespace bw::testkit
