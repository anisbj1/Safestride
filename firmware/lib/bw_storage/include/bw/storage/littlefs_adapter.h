#pragma once

// bw/storage/littlefs_adapter.h — IFlashFs on Arduino LittleFS (device build
// only; the whole .cpp is behind #ifdef ARDUINO so the native suite links
// nothing platform-flavoured). LittleFS is mounted at wiring time (startup),
// which keeps mount-time heap use inside the "no heap AFTER startup" rule
// (00 §7.2).
//
// toVfsPath() lives OUTSIDE the guard on purpose: the VFS path mapping is
// pure logic and the host suite pins it directly (bench round 2: bare
// relative names made every LittleFS.open fail with "does not start with /").

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <bw/storage/constants.h>
#include <bw/storage/flash_fs.h>

namespace bw::storage {

// [DERIVED] Capacity of a VFS-form path buffer incl. NUL: IFlashFs paths are
// bounded by kMaxPathBytes (incl. NUL, callers validate) and the VFS form
// adds exactly one leading '/' (toVfsPath never adds more).
constexpr size_t kVfsPathBytes = kMaxPathBytes + 1;

// Maps an IFlashFs path (relative, e.g. "evt_000.bin") to the form the
// Arduino VFS demands (mountpoint-relative, starting with '/', e.g.
// "/evt_000.bin"). vfs_api.cpp requires fpath[0] == '/' and PREPENDS the
// registered mountpoint ("/littlefs") itself, so this mapping must add the
// single leading slash and must NOT repeat the mountpoint. Idempotent for
// already-prefixed input (never double-slashes); Err::InvalidArg on
// null/empty input or when slash+name+NUL does not fit out_cap — rejected,
// never truncated.
inline Result<void> toVfsPath(const char* path, char* out, size_t out_cap) {
    if (path == nullptr || out == nullptr || path[0] == '\0') {
        return {bw::Err::InvalidArg};
    }
    const bool needs_slash = path[0] != '/';
    const size_t len = std::strlen(path);
    const size_t total = len + (needs_slash ? 1u : 0u);
    if (total + 1 > out_cap) {  // +1 for the NUL
        return {bw::Err::InvalidArg};
    }
    size_t o = 0;
    if (needs_slash) {
        out[o++] = '/';
    }
    std::memcpy(out + o, path, len + 1);  // incl. NUL
    return {bw::Err::Ok};
}

class LittleFsAdapter final : public IFlashFs {
public:
    Result<void> mount(bool format_on_fail) override;
    Result<uint32_t> totalBytes() const override;
    Result<uint32_t> freeBytes() const override;

    bool exists(const char* path) override;
    Result<uint32_t> size(const char* path) override;
    Result<size_t> read(const char* path, size_t offset, uint8_t* buf, size_t len) override;
    Result<size_t> write(const char* path, size_t offset, const uint8_t* buf, size_t len) override;
    Result<void> truncate(const char* path, size_t len) override;
    Result<void> remove(const char* path) override;
    Result<void> rename(const char* from, const char* to) override;
};

}  // namespace bw::storage
