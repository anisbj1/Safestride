#include <bw/storage/littlefs_adapter.h>

#if defined(ARDUINO)

#include <LittleFS.h>

#include <cstring>

#include <bw/storage/constants.h>

namespace bw::storage {

namespace {

// Truncate fallback for cores without File::truncate: copy the valid prefix
// to a temporary and rename it over the original (littlefs rename is an
// atomic overwrite, so the file is always old-or-new, never a mix). A power
// cut during the copy leaves the original intact plus a stale ".cut" file —
// SegmentManager::mount removes stale cut files for the fixed segment slot
// names; accepted limitation for other families (status log).
constexpr const char* kCutSuffix = ".cut";

}  // namespace

Result<void> LittleFsAdapter::mount(bool format_on_fail) {
    // Label passed explicitly — NOT the Arduino default "spiffs": it must
    // match the data row of firmware/partitions.csv, see
    // kLittleFsPartitionLabel (bench bug: default label found no partition,
    // mount AND format fallback failed, events never persisted). basePath/
    // maxOpenFiles are the core defaults, passed through only because the
    // label is the 4th begin() parameter.
    if (LittleFS.begin(/*formatOnFail=*/false, /*basePath=*/"/littlefs",
                       /*maxOpenFiles=*/10, kLittleFsPartitionLabel)) {
        return {bw::Err::Ok};
    }
    if (!format_on_fail) {
        return {bw::Err::NotReady};
    }
    // Unreadable/absent fs: format once — the store restarts empty rather
    // than bricking boot (F18 §10). The adapter holds no counters and no
    // fault sink: the CALLER (LittleFsEventStore::mount, two-step mount F-03)
    // reports StorageDegraded + counts format_recoveries around this format
    // call, so the wipe never passes silently.
    if (!LittleFS.format()) {
        return {bw::Err::HwFault};
    }
    return LittleFS.begin(/*formatOnFail=*/false, /*basePath=*/"/littlefs",
                          /*maxOpenFiles=*/10, kLittleFsPartitionLabel)
               ? bw::Result<void>{bw::Err::Ok}
               : bw::Result<void>{bw::Err::NotReady};
}

Result<uint32_t> LittleFsAdapter::totalBytes() const {
    return {bw::Err::Ok, static_cast<uint32_t>(LittleFS.totalBytes())};
}

Result<uint32_t> LittleFsAdapter::freeBytes() const {
    // This core exposes totalBytes/usedBytes only; free is the difference —
    // an approximation that ignores fs metadata overhead, documented for F23.
    const size_t total = LittleFS.totalBytes();
    const size_t used = LittleFS.usedBytes();
    return {bw::Err::Ok, static_cast<uint32_t>(total >= used ? total - used : 0)};
}

// Every path-taking entry point maps its argument(s) to the VFS form first
// (bench round 2: bare relative names were rejected by vfs_api.cpp with
// "does not start with /" — open failed for all 16 segments, writes dropped
// silently, nothing survived a reboot). mount()/totalBytes()/freeBytes() take
// no file path and are unaffected.

bool LittleFsAdapter::exists(const char* path) {
    char vfs[kVfsPathBytes];
    if (!toVfsPath(path, vfs, sizeof(vfs)).ok()) {
        return false;
    }
    return LittleFS.exists(vfs);
}

Result<uint32_t> LittleFsAdapter::size(const char* path) {
    char vfs[kVfsPathBytes];
    if (!toVfsPath(path, vfs, sizeof(vfs)).ok()) {
        return {bw::Err::InvalidArg, 0};
    }
    File f = LittleFS.open(vfs, "r");
    if (!f) {
        return {bw::Err::NotFound, 0};
    }
    const uint32_t s = f.size();
    f.close();
    return {bw::Err::Ok, s};
}

Result<size_t> LittleFsAdapter::read(const char* path, size_t offset, uint8_t* buf, size_t len) {
    char vfs[kVfsPathBytes];
    if (!toVfsPath(path, vfs, sizeof(vfs)).ok()) {
        return {bw::Err::InvalidArg, 0};
    }
    File f = LittleFS.open(vfs, "r");
    if (!f) {
        return {bw::Err::NotFound, 0};
    }
    if (offset > 0 && !f.seek(static_cast<uint32_t>(offset), SeekSet)) {
        f.close();
        return {bw::Err::HwFault, 0};
    }
    const size_t n = f.read(buf, len);
    f.close();
    return {bw::Err::Ok, n};  // short read at EOF is part of the contract
}

Result<size_t> LittleFsAdapter::write(const char* path, size_t offset, const uint8_t* buf,
                                      size_t len) {
    char vfs[kVfsPathBytes];
    if (!toVfsPath(path, vfs, sizeof(vfs)).ok()) {
        return {bw::Err::InvalidArg, 0};
    }
    if (!LittleFS.exists(vfs)) {
        if (offset != 0) {
            return {bw::Err::NotFound, 0};  // no hole punching (flash_fs.h)
        }
        File created = LittleFS.open(vfs, "w");
        if (!created) {
            return {bw::Err::HwFault, 0};
        }
        created.close();
    }
    File f = LittleFS.open(vfs, "r+");
    if (!f) {
        // Full partition (or unreadable): map to Err::Full so the D6
        // eviction ladder runs instead of a generic fault.
        return {bw::Err::Full, 0};
    }
    if (offset > 0 && !f.seek(static_cast<uint32_t>(offset), SeekSet)) {
        f.close();
        return {bw::Err::HwFault, 0};
    }
    size_t written = 0;
    while (written < len) {
        const size_t n = f.write(buf + written, len - written);
        if (n == 0) {
            f.close();
            return {bw::Err::Full, written};
        }
        written += n;
    }
    f.flush();
    f.close();
    return {bw::Err::Ok, written};
}

Result<void> LittleFsAdapter::truncate(const char* path, size_t len) {
    char vfs[kVfsPathBytes];
    if (!toVfsPath(path, vfs, sizeof(vfs)).ok()) {
        return {bw::Err::InvalidArg};
    }
    File f = LittleFS.open(vfs, "r");
    if (!f) {
        return {bw::Err::NotFound};
    }
    const size_t current = f.size();
    if (current <= len) {
        f.close();
        return {bw::Err::Ok};  // growing truncate is a no-op contract-wise here
    }
    // Copy-prefix + atomic rename-over (see kCutSuffix note above). The cut
    // name is built from the ALREADY-MAPPED path, so it is byte-identical to
    // what the stale-cut sweep sees when it maps "evt_xxx.bin.cut" itself —
    // one lfs target, no orphan split (pinned by test_vfs_path). vfs is
    // bounded by kVfsPathBytes, so vfs + ".cut" always fits the buffer.
    char cut[kVfsPathBytes + sizeof(kCutSuffix)];
    const size_t plen = std::strlen(vfs);
    std::memcpy(cut, vfs, plen);
    std::memcpy(cut + plen, kCutSuffix, sizeof(kCutSuffix));
    File out = LittleFS.open(cut, "w");
    if (!out) {
        f.close();
        return {bw::Err::HwFault};
    }
    uint8_t chunk[128];
    size_t remaining = len;
    bool failed = false;
    while (remaining > 0) {
        const size_t want = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
        if (f.read(chunk, want) != want) {
            failed = true;
            break;
        }
        if (out.write(chunk, want) != want) {
            failed = true;
            break;
        }
        remaining -= want;
    }
    f.close();
    out.flush();
    out.close();
    if (failed) {
        (void)LittleFS.remove(cut);
        return {bw::Err::HwFault};
    }
    if (!LittleFS.rename(cut, vfs)) {
        (void)LittleFS.remove(cut);
        return {bw::Err::HwFault};
    }
    return {bw::Err::Ok};
}

Result<void> LittleFsAdapter::remove(const char* path) {
    char vfs[kVfsPathBytes];
    if (!toVfsPath(path, vfs, sizeof(vfs)).ok()) {
        return {bw::Err::InvalidArg};
    }
    return LittleFS.remove(vfs) ? bw::Result<void>{bw::Err::Ok}
                                : bw::Result<void>{bw::Err::NotFound};
}

Result<void> LittleFsAdapter::rename(const char* from, const char* to) {
    char vfs_from[kVfsPathBytes];
    char vfs_to[kVfsPathBytes];
    if (!toVfsPath(from, vfs_from, sizeof(vfs_from)).ok() ||
        !toVfsPath(to, vfs_to, sizeof(vfs_to)).ok()) {
        return {bw::Err::InvalidArg};
    }
    return LittleFS.rename(vfs_from, vfs_to) ? bw::Result<void>{bw::Err::Ok}
                                             : bw::Result<void>{bw::Err::HwFault};
}

}  // namespace bw::storage

#endif  // ARDUINO
