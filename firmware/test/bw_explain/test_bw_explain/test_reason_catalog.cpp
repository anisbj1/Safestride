// bw_explain — F22 §9 items 1-6: the catalog is pinned to bw_core's REAL
// ReasonCode enum, never to a copy inside this test (§6 D2):
//   1. parse the 'enum class ReasonCode' block out of bw/state.h (fail-loud if
//      the header cannot be found — a skipped check is a failed check, §10),
//      compare enumerator count + per-code text against the catalog, so
//      APPENDING a code without text fails here;
//   2. every enumerator name must appear symbolically as 'ReasonCode::<name>'
//      in the bw_explain sources (no numeric-cast entries);
//   3. every current enumerator's numeric value is pinned (append-only contract
//      guard, docs/interfaces/core.md §6.6): renumber/reorder fails at compile
//      time; a plain append does NOT fail here — test 1 catches it;
//   4. wording: lowercase printable ASCII, no diagnosis/certainty vocabulary
//      (F22 §5, docs/00_PROJECT_CONTEXT.md: the device never diagnoses);
//   5. catalog version pinned.

#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <unity.h>

#include <bw/explain/explain_short.h>
#include <bw/explain/reason_catalog.h>

namespace {

// F22 §8 kReasonHeaderCandidates: tried in order relative to the test CWD.
const char* const kStateHeaderCandidates[] = {
    "firmware/lib/bw_core/include/bw/state.h",
    "lib/bw_core/include/bw/state.h",
    "../lib/bw_core/include/bw/state.h",
};
const char* const kStateHeaderSuffix = "lib/bw_core/include/bw/state.h";

// The catalog TU is REQUIRED (if it cannot be read, nothing is verified);
// the other sources are skipped when unreadable because their relative path
// depends on which candidate above matched.
const char* const kCatalogSource = "lib/bw_explain/src/reason_catalog.cpp";
const char* const kOptionalSources[] = {
    "lib/bw_explain/src/explain_short.cpp",
    "lib/bw_explain/include/bw/explain/reason_catalog.h",
    "lib/bw_explain/include/bw/explain/explain_short.h",
    "src/app/wiring/wire_explain.h",
    "src/app/wiring/wire_explain.cpp",
};

bool readFile(const std::string& path, std::string& out) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

// Prefix of a candidate that reaches the repository ('', 'firmware/', '../'):
// shared so the enum header and the bw_explain sources resolve from one root.
std::string rootOf(const std::string& candidate) {
    const size_t pos = candidate.rfind(kStateHeaderSuffix);
    return (pos == std::string::npos) ? std::string() : candidate.substr(0, pos);
}

bool loadStateHeader(std::string& content, std::string& root) {
    for (const char* candidate : kStateHeaderCandidates) {
        if (readFile(candidate, content)) {
            root = rootOf(candidate);
            return true;
        }
    }
    return false;
}

// Extracts the enumerator identifiers of 'enum class ReasonCode {...}' in
// SOURCE ORDER — which is the numeric order of this append-only enum with its
// implicit values (docs/interfaces/data_decision.md §4.5). Returns false on
// any line it cannot classify: the test then FAILS instead of silently
// checking fewer codes than exist.
bool parseReasonCodeBlock(const std::string& src, std::vector<std::string>& out) {
    out.clear();
    const size_t keyword = src.find("enum class ReasonCode");
    if (keyword == std::string::npos) {
        return false;
    }
    const size_t open = src.find('{', keyword);
    if (open == std::string::npos) {
        return false;
    }
    const size_t close = src.find('}', open);
    if (close == std::string::npos) {
        return false;
    }
    const std::string block = src.substr(open + 1, close - open - 1);

    size_t line_start = 0;
    while (line_start < block.size()) {
        size_t line_end = block.find('\n', line_start);
        if (line_end == std::string::npos) {
            line_end = block.size();
        }
        std::string line = block.substr(line_start, line_end - line_start);
        line_start = line_end + 1;

        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos) {
            continue;  // blank line
        }
        line = line.substr(first);
        if (line[0] == '/' || line[0] == '*') {
            continue;  // comment (full-line or block-comment continuation)
        }
        if (std::isdigit(static_cast<unsigned char>(line[0])) != 0) {
            return false;  // enumerator names never start with a digit
        }
        size_t end = 0;
        while (end < line.size()) {
            const unsigned char c = static_cast<unsigned char>(line[end]);
            const bool ident_char =
                std::isalpha(c) != 0 || std::isdigit(c) != 0 || line[end] == '_';
            if (!ident_char) {
                break;
            }
            ++end;
        }
        if (end == 0) {
            return false;  // unparsable line inside the enum block
        }
        if (end < line.size()) {
            const char delimiter = line[end];
            if (delimiter != ',' && delimiter != '/' && delimiter != ' ' && delimiter != '\t' &&
                delimiter != '\r') {
                return false;  // name must end at ',' , comment or EOL
            }
        }
        out.push_back(line.substr(0, end));
    }
    return !out.empty();
}

}  // namespace

// --- D2 test 3: append-only value pins (compile time) -----------------------
// A plain APPEND does not fail these (earlier values are unchanged) — the
// completeness test below parses bw/state.h and catches that case.
static_assert(static_cast<uint8_t>(bw::ReasonCode::SustainedLowMotion) == 0, "");
static_assert(static_cast<uint8_t>(bw::ReasonCode::ProlongedInactivity) == 1, "");
static_assert(static_cast<uint8_t>(bw::ReasonCode::HrBelowBaseline) == 2, "");
static_assert(static_cast<uint8_t>(bw::ReasonCode::HrTrendChange) == 3, "");
static_assert(static_cast<uint8_t>(bw::ReasonCode::AbruptMotionDrop) == 4, "");
static_assert(static_cast<uint8_t>(bw::ReasonCode::TimeOfDayTypicalDrowsy) == 5, "");
static_assert(static_cast<uint8_t>(bw::ReasonCode::MatchesPastConfirmedEvent) == 6, "");
static_assert(static_cast<uint8_t>(bw::ReasonCode::UserMarked) == 7, "");
static_assert(static_cast<uint8_t>(bw::ReasonCode::PlannedNap) == 8, "");
static_assert(static_cast<uint8_t>(bw::ReasonCode::LowSignalQuality) == 9, "");
static_assert(static_cast<uint8_t>(bw::ReasonCode::SensorUnavailable) == 10, "");
static_assert(static_cast<uint8_t>(bw::ReasonCode::DeviceNotWorn) == 11, "");
static_assert(static_cast<uint8_t>(bw::ReasonCode::SustainedLowAlertnessProbability) == 12, "");
static_assert(static_cast<uint8_t>(bw::ReasonCode::SustainedWakefulnessProbability) == 13, "");
static_assert(static_cast<uint8_t>(bw::ReasonCode::InsufficientEvidence) == 14, "");
static_assert(static_cast<uint8_t>(bw::ReasonCode::PossibleFall) == 15, "");
static_assert(static_cast<uint8_t>(bw::ReasonCode::PostureChange) == 16, "");
static_assert(static_cast<uint8_t>(bw::ReasonCode::ShortMotionBurst) == 17, "");

void test_catalog_completeness_pinned_to_enum(void) {
    std::string content;
    std::string root;
    TEST_ASSERT_TRUE_MESSAGE(
        loadStateHeader(content, root),
        "bw/state.h not found via kStateHeaderCandidates - FAIL, never skip (F22 S10)");
    std::vector<std::string> names;
    TEST_ASSERT_TRUE_MESSAGE(parseReasonCodeBlock(content, names),
                             "could not parse the enum class ReasonCode block from bw/state.h");
    // THE spec-9 check: a ReasonCode appended without a catalog entry fails here.
    TEST_ASSERT_EQUAL_UINT(bw::explain::kReasonCatalogCount, static_cast<unsigned>(names.size()));
    for (size_t i = 0; i < names.size(); ++i) {
        const char* text = bw::explain::reasonText(static_cast<bw::ReasonCode>(i));
        TEST_ASSERT_NOT_NULL_MESSAGE(text, names[i].c_str());
        const size_t len = std::strlen(text);
        TEST_ASSERT_TRUE_MESSAGE(len > 0, names[i].c_str());
        TEST_ASSERT_TRUE_MESSAGE(len <= bw::explain::kMaxShortReasonChars, names[i].c_str());
    }
}

void test_catalog_entries_reference_enumerators_symbolically(void) {
    std::string content;
    std::string root;
    TEST_ASSERT_TRUE_MESSAGE(loadStateHeader(content, root),
                             "bw/state.h not found via kStateHeaderCandidates");
    std::vector<std::string> names;
    TEST_ASSERT_TRUE_MESSAGE(parseReasonCodeBlock(content, names),
                             "could not parse the enum class ReasonCode block");

    std::string catalog_source;
    TEST_ASSERT_TRUE_MESSAGE(readFile(root + kCatalogSource, catalog_source),
                             "reason_catalog.cpp not found - cannot verify symbolic entries");
    std::vector<std::string> optional_sources;
    for (const char* relative : kOptionalSources) {
        std::string source;
        if (readFile(root + relative, source)) {
            optional_sources.push_back(source);
        }
    }

    for (const std::string& name : names) {
        const std::string needle = "ReasonCode::" + name;
        bool found = catalog_source.find(needle) != std::string::npos;
        for (const std::string& source : optional_sources) {
            found = found || source.find(needle) != std::string::npos;
        }
        TEST_ASSERT_TRUE_MESSAGE(found, name.c_str());
    }
}

// --- D2 test 3: value pins, mirrored at runtime so the pin is visible in the
// suite output (the static_asserts above fail the build on their own). -------
void test_catalog_code_values_pinned(void) {
    TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(bw::ReasonCode::SustainedLowMotion));
    TEST_ASSERT_EQUAL_UINT8(1, static_cast<uint8_t>(bw::ReasonCode::ProlongedInactivity));
    TEST_ASSERT_EQUAL_UINT8(2, static_cast<uint8_t>(bw::ReasonCode::HrBelowBaseline));
    TEST_ASSERT_EQUAL_UINT8(3, static_cast<uint8_t>(bw::ReasonCode::HrTrendChange));
    TEST_ASSERT_EQUAL_UINT8(4, static_cast<uint8_t>(bw::ReasonCode::AbruptMotionDrop));
    TEST_ASSERT_EQUAL_UINT8(5, static_cast<uint8_t>(bw::ReasonCode::TimeOfDayTypicalDrowsy));
    TEST_ASSERT_EQUAL_UINT8(6, static_cast<uint8_t>(bw::ReasonCode::MatchesPastConfirmedEvent));
    TEST_ASSERT_EQUAL_UINT8(7, static_cast<uint8_t>(bw::ReasonCode::UserMarked));
    TEST_ASSERT_EQUAL_UINT8(8, static_cast<uint8_t>(bw::ReasonCode::PlannedNap));
    TEST_ASSERT_EQUAL_UINT8(9, static_cast<uint8_t>(bw::ReasonCode::LowSignalQuality));
    TEST_ASSERT_EQUAL_UINT8(10, static_cast<uint8_t>(bw::ReasonCode::SensorUnavailable));
    TEST_ASSERT_EQUAL_UINT8(11, static_cast<uint8_t>(bw::ReasonCode::DeviceNotWorn));
    TEST_ASSERT_EQUAL_UINT8(12,
                            static_cast<uint8_t>(bw::ReasonCode::SustainedLowAlertnessProbability));
    TEST_ASSERT_EQUAL_UINT8(13,
                            static_cast<uint8_t>(bw::ReasonCode::SustainedWakefulnessProbability));
    TEST_ASSERT_EQUAL_UINT8(14, static_cast<uint8_t>(bw::ReasonCode::InsufficientEvidence));
    TEST_ASSERT_EQUAL_UINT8(15, static_cast<uint8_t>(bw::ReasonCode::PossibleFall));
    TEST_ASSERT_EQUAL_UINT8(16, static_cast<uint8_t>(bw::ReasonCode::PostureChange));
    TEST_ASSERT_EQUAL_UINT8(17, static_cast<uint8_t>(bw::ReasonCode::ShortMotionBurst));
}

void test_catalog_wording_has_no_diagnosis_language(void) {
    // docs/00_PROJECT_CONTEXT.md wording rule: the device never diagnoses, never claims
    // certainty. Detections read 'possible'/'suspected'; reason text only
    // names observable factors. Banned stems come from that rule.
    const char* const kBannedStems[] = {"diagnos", "narcolep", "disease",
                                        "certain", "confirm",  "definite"};
    for (uint8_t i = 0; i < bw::explain::kReasonCatalogCount; ++i) {
        const char* text = bw::explain::reasonText(static_cast<bw::ReasonCode>(i));
        TEST_ASSERT_NOT_NULL(text);
        for (const char* stem : kBannedStems) {
            TEST_ASSERT_NULL_MESSAGE(std::strstr(text, stem), text);
        }
        TEST_ASSERT_TRUE_MESSAGE(std::islower(static_cast<unsigned char>(text[0])) != 0, text);
        for (const char* c = text; *c != '\0'; ++c) {
            // isprint rejects non-ASCII (locale "C") — OLED-safe plain ASCII.
            TEST_ASSERT_TRUE_MESSAGE(std::isprint(static_cast<unsigned char>(*c)) != 0, text);
        }
    }
}

void test_catalog_completeness_detects_appended_code(void) {
    // Fault-injection for the completeness check ITSELF: a doctored IN-MEMORY
    // copy of the enum block gains one enumerator, exactly as a future
    // append-only CR would add it. No bw_core file is touched. The parser must
    // SEE the new name — so the parsed==kReasonCatalogCount equality in
    // test_catalog_completeness_pinned_to_enum fails until bw_explain gains
    // the matching entry (the mechanism behind spec §9).
    std::string content;
    std::string root;
    TEST_ASSERT_TRUE_MESSAGE(loadStateHeader(content, root),
                             "bw/state.h not found via kStateHeaderCandidates");
    std::vector<std::string> names;
    TEST_ASSERT_TRUE_MESSAGE(parseReasonCodeBlock(content, names),
                             "could not parse the enum class ReasonCode block");

    const size_t keyword = content.find("enum class ReasonCode");
    const size_t open = content.find('{', keyword);
    const size_t close = content.find('}', open);
    TEST_ASSERT_TRUE(keyword != std::string::npos && open != std::string::npos &&
                     close != std::string::npos);
    content.insert(close, "\n    BrandNewReason,");

    std::vector<std::string> doctored_names;
    TEST_ASSERT_TRUE(parseReasonCodeBlock(content, doctored_names));
    TEST_ASSERT_EQUAL_UINT(bw::explain::kReasonCatalogCount + 1, doctored_names.size());
    TEST_ASSERT_EQUAL_STRING("BrandNewReason", doctored_names.back().c_str());

    // Defensive rendering path for that future code (§10): no entry yet, so
    // reasonText says 'no entry' and explainShort still shows 'reason #N'.
    const bw::ReasonCode future = static_cast<bw::ReasonCode>(bw::explain::kReasonCatalogCount);
    TEST_ASSERT_NULL(bw::explain::reasonText(future));
    char buf[32];
    bw::explain::explainShort(static_cast<bw::ReasonMask>(1) << bw::explain::kReasonCatalogCount,
                              buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("reason #18", buf);
}

void test_catalog_version_is_pinned(void) {
    TEST_ASSERT_EQUAL_UINT16(1, bw::explain::kReasonCatalogVersion);
}
