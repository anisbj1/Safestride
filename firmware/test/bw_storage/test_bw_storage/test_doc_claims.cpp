// F18 audit FIX tests for findings that live in COMMENTS and DOCS only
// (F-05: a false wiring claim; F-09: spec numbers drifted from the code).
// Behavior cannot fail when a comment is reverted, so these tests read the
// sources as text and gate the claims directly — the only revert evidence
// available for comment/doc findings (deviation recorded in the status log).
//
// Sources are located via __FILE__ (works from any worktree layout) with
// CWD-relative fallbacks (pio test may run the binary from the project dir).
// Failing to FIND a source is a test failure, never a silent pass.

#include <unity.h>

#include <bw/storage/constants.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string readTextFile(const char* path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string readBesideSource(const char* rel_from_file,
                             const std::vector<const char*>& cwd_candidates) {
    std::string self = __FILE__;
    const size_t slash = self.find_last_of("/\\");
    const std::string dir = (slash == std::string::npos) ? std::string(".") : self.substr(0, slash);
    std::string content = readTextFile((dir + "/" + rel_from_file).c_str());
    if (!content.empty()) {
        return content;
    }
    for (const char* candidate : cwd_candidates) {
        content = readTextFile(candidate);
        if (!content.empty()) {
            return content;
        }
    }
    return {};
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

void test_doc_wire_storage_write_claim_is_true(void) {
    const std::string header =
        readBesideSource("../../../src/app/wiring/wire_storage.h",
                         {"src/app/wiring/wire_storage.h", "firmware/src/app/wiring/wire_storage.h",
                          "../firmware/src/app/wiring/wire_storage.h"});
    TEST_ASSERT_FALSE_MESSAGE(header.empty(),
                              "wire_storage.h not found - doc-claim test cannot gate F-05");
    // F-05: the old comment claimed "All flash writes happen HERE, never in
    // a producer task" — false: KV (put) and window (putWindow) writes run
    // synchronously in the CALLER's task (they have no queue). Reverting the
    // comment fix brings the false claim back and this assertion fails.
    TEST_ASSERT_FALSE_MESSAGE(contains(header, "never in a producer task"),
                              "wire_storage.h reverted to the false write claim (F-05)");
    TEST_ASSERT_TRUE_MESSAGE(contains(header, "synchronous in the caller's task"),
                             "wire_storage.h must state the real KV/window precondition (F-05)");
}

void test_doc_spec_numbers_match_code(void) {
    const std::string spec = readBesideSource("../../../../docs/features/F18_local_data_storage.md",
                                              {"docs/features/F18_local_data_storage.md",
                                               "../docs/features/F18_local_data_storage.md",
                                               "../../docs/features/F18_local_data_storage.md"});
    TEST_ASSERT_FALSE_MESSAGE(spec.empty(), "F18 spec not found - doc-claim test cannot gate F-09");

    // F-09: spec numbers must match the code. Op is pinned at 64 B by the
    // static_assert in event_store.cpp, SegmentManager's state by
    // kSegmentManagerStateBudgetBytes in segment_manager.h.
    TEST_ASSERT_FALSE_MESSAGE(contains(spec, "896 B"),
                              "stale queue width '896 B' back in the spec (F-09)");
    TEST_ASSERT_FALSE_MESSAGE(contains(spec, "56 B)"),
                              "stale Op width '56 B' back in the spec (F-09)");
    TEST_ASSERT_FALSE_MESSAGE(contains(spec, "index 256 B"),
                              "stale segment index size 'index 256 B' back in the spec (F-09)");
    TEST_ASSERT_TRUE_MESSAGE(contains(spec, "Op = 64 B"), "spec must state 'Op = 64 B' (F-09)");
    TEST_ASSERT_TRUE_MESSAGE(contains(spec, "1024 B"),
                             "spec must state the queue as '1024 B' (F-09)");
    TEST_ASSERT_TRUE_MESSAGE(contains(spec, "index 320 B"),
                             "spec must state 'index 320 B' (16 x 20 B, F-09)");

    // F-05 mirror: the spec carried the same false write claim as the header
    // and must be corrected with the same wording.
    TEST_ASSERT_FALSE_MESSAGE(contains(spec, "All flash writes happen inside"),
                              "spec reverted to the false write claim (F-05)");
    TEST_ASSERT_TRUE_MESSAGE(contains(spec, "synchronous in the caller's task"),
                             "spec must carry the real KV/window precondition (F-05)");
}

void test_doc_thread_model_lists_shared_state(void) {
    // Audit NEW-L1 (comment-only finding, gated as text like F-05/F-09): the
    // thread-model comment must list ALL cross-task shared state —
    // resync_budget_used_ is written by read-only scanFrom walks, and the
    // producer/pump marker flag needs its note at the member.
    const std::string seg_header =
        readBesideSource("../../../lib/bw_storage/include/bw/storage/segment_manager.h",
                         {"firmware/lib/bw_storage/include/bw/storage/segment_manager.h",
                          "../firmware/lib/bw_storage/include/bw/storage/segment_manager.h"});
    TEST_ASSERT_FALSE_MESSAGE(seg_header.empty(),
                              "segment_manager.h not found - doc-claim test cannot gate NEW-L1");
    TEST_ASSERT_TRUE_MESSAGE(
        contains(seg_header, "READ-ONLY scanFrom walk bumps it"),
        "thread-model comment must list resync_budget_used_ as shared (NEW-L1)");
    const std::string store_header =
        readBesideSource("../../../lib/bw_storage/include/bw/storage/event_store.h",
                         {"firmware/lib/bw_storage/include/bw/storage/event_store.h",
                          "../firmware/lib/bw_storage/include/bw/storage/event_store.h"});
    TEST_ASSERT_FALSE_MESSAGE(store_header.empty(),
                              "event_store.h not found - doc-claim test cannot gate NEW-L1");
    TEST_ASSERT_TRUE_MESSAGE(contains(store_header, "Shared with the producer side (audit NEW-L1)"),
                             "marker_dirty_ needs its producer/pump shared-state note (NEW-L1)");
}

void test_doc_counters_header_names_format_recoveries(void) {
    // Audit NEW-L2 (comment-only finding): the counters header must not claim
    // every field is a CR-015 field — format_recoveries is fault-only because
    // CR-015's StorageHealth is a locked contract.
    const std::string counters =
        readBesideSource("../../../lib/bw_storage/include/bw/storage/counters.h",
                         {"firmware/lib/bw_storage/include/bw/storage/counters.h",
                          "../firmware/lib/bw_storage/include/bw/storage/counters.h"});
    TEST_ASSERT_FALSE_MESSAGE(counters.empty(),
                              "counters.h not found - doc-claim test cannot gate NEW-L2");
    TEST_ASSERT_FALSE_MESSAGE(contains(counters, "health counters (CR-015 fields)"),
                              "counters.h reverted to the false CR-015-only header (NEW-L2)");
    TEST_ASSERT_TRUE_MESSAGE(contains(counters, "fault-only"),
                             "counters.h must name format_recoveries as fault-only (NEW-L2)");
}

// Bench device bug (COM14, master @ 2a57192): LittleFS.begin() used the
// Arduino default label "spiffs" while firmware/partitions.csv names the data
// partition `storage` — every boot failed to mount AND the format fallback
// found no partition, so events never survived a reboot. Host tests run on
// RamFsFake and cannot see it, so pin BOTH halves here as text: the constant
// must name the CSV's data row, and the adapter must actually pass it.
namespace {

std::string trimField(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) {
        return {};
    }
    const size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

}  // namespace

void test_doc_partition_label_matches_csv(void) {
    const std::string csv =
        readBesideSource("../../../partitions.csv", {"firmware/partitions.csv", "partitions.csv",
                                                     "../firmware/partitions.csv"});
    TEST_ASSERT_FALSE_MESSAGE(csv.empty(),
                              "partitions.csv not found - doc-claim test cannot gate the label");

    std::istringstream rows(csv);
    std::string line;
    bool row_found = false;
    while (std::getline(rows, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const size_t c1 = line.find(',');
        const size_t c2 = (c1 == std::string::npos) ? std::string::npos : line.find(',', c1 + 1);
        if (c1 == std::string::npos || c2 == std::string::npos) {
            continue;
        }
        if (trimField(line.substr(c1 + 1, c2 - c1 - 1)) != "data") {
            continue;  // only the Type column selects data rows (nvs, storage)
        }
        if (trimField(line.substr(0, c1)) == bw::storage::kLittleFsPartitionLabel) {
            row_found = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(
        row_found, "a data row's Name in partitions.csv must equal kLittleFsPartitionLabel");

    const std::string adapter =
        readBesideSource("../../../lib/bw_storage/src/littlefs_adapter.cpp",
                         {"firmware/lib/bw_storage/src/littlefs_adapter.cpp",
                          "lib/bw_storage/src/littlefs_adapter.cpp",
                          "../firmware/lib/bw_storage/src/littlefs_adapter.cpp"});
    TEST_ASSERT_FALSE_MESSAGE(
        adapter.empty(), "littlefs_adapter.cpp not found - doc-claim test cannot gate the label");
    TEST_ASSERT_TRUE_MESSAGE(contains(adapter, "kLittleFsPartitionLabel"),
                             "LittleFS.begin() must receive kLittleFsPartitionLabel, not the "
                             "Arduino default (partition not found on device)");
}
