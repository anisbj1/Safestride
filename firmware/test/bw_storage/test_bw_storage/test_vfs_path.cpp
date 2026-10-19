// bench round-2 device fix: the Arduino VFS rejects paths that do not start
// with '/' (vfs_api.cpp: "does not start with /") and prepends the registered
// mountpoint ("/littlefs") itself — bw_storage builds relative names
// ("evt_000.bin"), so the adapter maps them through toVfsPath() before every
// LittleFS call. That code path is ARDUINO-only and unreachable on the host,
// so the mapping is pure logic in the header and these tests pin it directly
// (H10: dropping the prefix fails the bare-name test below).

#include <unity.h>

#include <bw/storage/littlefs_adapter.h>

#include <cstring>

namespace {

// Longest legal IFlashFs path: kMaxPathBytes incl. NUL.
constexpr size_t kLongestPathChars = bw::storage::kMaxPathBytes - 1;

}  // namespace

void test_vfs_path_prefixes_bare_relative_names(void) {
    // The exact bench failure: "evt_000.bin does not start with /".
    char out[bw::storage::kVfsPathBytes];
    TEST_ASSERT_TRUE(bw::storage::toVfsPath("evt_000.bin", out, sizeof(out)).ok());
    TEST_ASSERT_EQUAL_STRING("/evt_000.bin", out);
    TEST_ASSERT_TRUE(bw::storage::toVfsPath("kv_motion.tmp", out, sizeof(out)).ok());
    TEST_ASSERT_EQUAL_STRING("/kv_motion.tmp", out);
    TEST_ASSERT_TRUE(bw::storage::toVfsPath("evt_015.bin.cut", out, sizeof(out)).ok());
    TEST_ASSERT_EQUAL_STRING("/evt_015.bin.cut", out);
}

void test_vfs_path_is_idempotent_for_prefixed_input(void) {
    // Double-prefixing would hand VFS "//evt_000.bin" — pass-through instead.
    char out[bw::storage::kVfsPathBytes];
    TEST_ASSERT_TRUE(bw::storage::toVfsPath("/evt_000.bin", out, sizeof(out)).ok());
    TEST_ASSERT_EQUAL_STRING("/evt_000.bin", out);
    TEST_ASSERT_TRUE(bw::storage::toVfsPath(out, out, sizeof(out)).ok());  // no self-corruption
    TEST_ASSERT_EQUAL_STRING("/evt_000.bin", out);
}

void test_vfs_path_cut_name_agrees_across_builders(void) {
    // Adapter truncate builds its temp as (mapped path) + ".cut"; the mount
    // sweep probes "evt_xxx.bin.cut" through the same mapping. Both must name
    // the ONE lfs file or stale-cut cleanup would miss it.
    char mapped[bw::storage::kVfsPathBytes];
    TEST_ASSERT_TRUE(bw::storage::toVfsPath("evt_007.bin", mapped, sizeof(mapped)).ok());
    char via_mapped[bw::storage::kVfsPathBytes + sizeof(".cut")];
    std::strcpy(via_mapped, mapped);
    std::strcat(via_mapped, ".cut");
    char direct[bw::storage::kVfsPathBytes + sizeof(".cut")];
    TEST_ASSERT_TRUE(bw::storage::toVfsPath("evt_007.bin.cut", direct, sizeof(direct)).ok());
    TEST_ASSERT_EQUAL_STRING(direct, via_mapped);
}

void test_vfs_path_bounds_and_bad_args_rejected(void) {
    char out[bw::storage::kVfsPathBytes];
    // Longest legal bare path (47 chars) + slash + NUL == exactly kVfsPathBytes.
    char longest[bw::storage::kVfsPathBytes];
    std::memset(longest, 'a', kLongestPathChars);
    longest[kLongestPathChars] = '\0';
    TEST_ASSERT_TRUE(bw::storage::toVfsPath(longest, out, sizeof(out)).ok());
    TEST_ASSERT_EQUAL_UINT(kLongestPathChars + 1, std::strlen(out));
    // One char beyond the caller contract: rejected, never truncated.
    char toolong[bw::storage::kVfsPathBytes + 1];
    std::memset(toolong, 'a', bw::storage::kMaxPathBytes);
    toolong[bw::storage::kMaxPathBytes] = '\0';
    TEST_ASSERT_FALSE(bw::storage::toVfsPath(toolong, out, sizeof(out)).ok());
    // Prefixed max-length input still fits (no extra slash to add).
    char prefixed_in[bw::storage::kVfsPathBytes];
    prefixed_in[0] = '/';
    std::memcpy(prefixed_in + 1, longest, kLongestPathChars + 1);
    TEST_ASSERT_TRUE(bw::storage::toVfsPath(prefixed_in, out, sizeof(out)).ok());
    // Bad arguments.
    TEST_ASSERT_FALSE(bw::storage::toVfsPath("", out, sizeof(out)).ok());
    TEST_ASSERT_FALSE(bw::storage::toVfsPath(nullptr, out, sizeof(out)).ok());
    TEST_ASSERT_FALSE(bw::storage::toVfsPath("evt_000.bin", out, 5).ok());  // too small
}
