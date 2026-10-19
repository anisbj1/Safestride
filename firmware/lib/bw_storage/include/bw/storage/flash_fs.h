#pragma once

// bw/storage/flash_fs.h — F18's internal flash-filesystem abstraction
// (F18 spec §4: "flash access via a small IFlashFs abstraction internal to the
// lib"). NOT a bw_core contract: only F18 code sees it. Its purpose is
// testability — the whole storage stack runs on the host against RamFsFake
// (spec §15.5, DoD §14 byte-offset fault injection) — and replaceability
// (LittleFsAdapter today, another fs later).
//
// Conventions:
//  - Paths are NUL-terminated, relative, never contain ".." (callers validate).
//  - read/write report the byte count transferred; short reads mean EOF.
//  - write at offset > size is an error (no implicit hole punching).
//  - rename OVERWRITES an existing destination atomically (littlefs rename
//    semantics), which KvStore relies on for torn-safe puts (D7).

#include <cstddef>
#include <cstdint>

#include <bw/result.h>

namespace bw::storage {

class IFlashFs {
public:
    // Mounts the filesystem; when `mount_failed` is true the adapter formats
    // once and remounts (fresh device or unreadable fs — spec §10: never brick
    // boot).
    virtual Result<void> mount(bool format_on_fail) = 0;

    virtual Result<uint32_t> totalBytes() const = 0;
    virtual Result<uint32_t> freeBytes() const = 0;

    virtual bool exists(const char* path) = 0;
    virtual Result<uint32_t> size(const char* path) = 0;

    // Reads/writes [offset, offset+len). write() is the fault-injection seam:
    // RamFsFake can commit a torn prefix of exactly one write call.
    virtual Result<size_t> read(const char* path, size_t offset, uint8_t* buf, size_t len) = 0;
    virtual Result<size_t> write(const char* path, size_t offset, const uint8_t* buf,
                                 size_t len) = 0;

    virtual Result<void> truncate(const char* path, size_t len) = 0;
    virtual Result<void> remove(const char* path) = 0;
    // Atomic overwrite: destination is replaced or the call fails; never a
    // remove-then-rename window (D7/D4 power-loss safety).
    virtual Result<void> rename(const char* from, const char* to) = 0;

    // No virtual destructor on purpose (same rule as bw::IClock, bw/clock.h):
    // instances live in the composition root and are never deleted through a
    // base pointer; RTTI is disabled (DEC-002).

protected:
    ~IFlashFs() = default;
};

}  // namespace bw::storage
