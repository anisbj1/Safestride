#pragma once

// RamFsFake — bw::storage::IFlashFs on the host (F18 spec §15.2, DoD §14).
// Host-only test infrastructure: it uses std::vector/string on purpose (the
// no-heap rule targets firmware paths, 00 §7.2; bw/testkit/host_only.h marks
// the same exception for shared fakes).
//
// Power-loss model: every write() call is one atomic flash programming step
// the fake can cut. armTornWrite(n) makes the NEXT write commit exactly n
// bytes of its payload and then fail — the byte-offset sweep of the DoD
// drives n across every offset of an append. "Reboot" = construct a fresh
// store over the SAME fake; contents survive, RAM state does not.
//
// Op-failure model: failOpsEveryN(n) fails every n-th mutating op
// (write/truncate/remove/rename) with no side effect — the host-testing
// "fail every N-th operation" pattern.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <bw/storage/flash_fs.h>

class RamFsFake final : public bw::storage::IFlashFs {
public:
    explicit RamFsFake(uint32_t capacity_bytes = 512u * 1024u) : capacity_(capacity_bytes) {}

    bw::Result<void> mount(bool format_on_fail) override {
        if (!formatted_) {
            if (!format_on_fail) {
                return {bw::Err::NotReady};
            }
            files_.clear();
            formatted_ = true;
        }
        mounted_ = true;
        return {bw::Err::Ok};
    }

    bw::Result<uint32_t> totalBytes() const override { return {bw::Err::Ok, capacity_}; }

    bw::Result<uint32_t> freeBytes() const override {
        const uint32_t used = usedBytes();
        return {bw::Err::Ok, used >= capacity_ ? 0 : capacity_ - used};
    }

    bool exists(const char* path) override { return files_.find(path) != files_.end(); }

    bw::Result<uint32_t> size(const char* path) override {
        countOp();
        if (shouldFailOp()) {
            return {bw::Err::HwFault, 0};
        }
        auto it = files_.find(path);
        if (it == files_.end()) {
            return {bw::Err::NotFound, 0};
        }
        return {bw::Err::Ok, static_cast<uint32_t>(it->second.size())};
    }

    bw::Result<size_t> read(const char* path, size_t offset, uint8_t* buf, size_t len) override {
        countOp();
        if (shouldFailOp()) {
            return {bw::Err::HwFault, 0};
        }
        if (!fail_read_path_.empty() && fail_read_path_ == path) {
            fail_read_path_.clear();  // one-shot transient IO (F-04)
            return {bw::Err::HwFault, 0};
        }
        auto it = files_.find(path);
        if (it == files_.end()) {
            return {bw::Err::NotFound, 0};
        }
        if (offset >= it->second.size()) {
            return {bw::Err::Ok, 0};  // EOF: short read, no error (flash_fs.h)
        }
        const size_t avail = it->second.size() - offset;
        const size_t n = len < avail ? len : avail;
        for (size_t i = 0; i < n; ++i) {
            buf[i] = it->second[offset + i];
        }
        return {bw::Err::Ok, n};
    }

    bw::Result<size_t> write(const char* path, size_t offset, const uint8_t* buf,
                             size_t len) override {
        countOp();
        if (shouldFailOp()) {
            return {bw::Err::HwFault, 0};
        }
        auto it = files_.find(path);
        if (it == files_.end()) {
            if (offset != 0) {
                return {bw::Err::NotFound, 0};  // matches LittleFsAdapter
            }
            it = files_.emplace(path, std::vector<uint8_t>{}).first;
        }
        std::vector<uint8_t>& data = it->second;
        if (offset > data.size()) {
            return {bw::Err::NotFound, 0};  // no hole punching
        }
        size_t commit = len;
        bool torned = false;
        if (torn_next_ >= 0) {
            if (torn_skip_ > 0) {
                --torn_skip_;
            } else {
                const size_t budget = static_cast<size_t>(torn_next_);
                torn_next_ = -1;  // one-shot
                if (budget < len) {
                    // Power cut: exactly `budget` bytes of this write reach
                    // the medium, the device dies (Err::HwFault).
                    commit = budget;
                    torned = true;
                }
            }
        }
        const size_t new_size = offset + commit > data.size() ? offset + commit : data.size();
        const size_t growth = new_size > data.size() ? new_size - data.size() : 0;
        if (usedBytes() + growth > capacity_) {
            return {bw::Err::Full, 0};  // flash full: nothing written (CR-004)
        }
        data.resize(new_size, 0);
        for (size_t i = 0; i < commit; ++i) {
            data[offset + i] = buf[i];
        }
        if (torned) {
            return {bw::Err::HwFault, commit};
        }
        return {bw::Err::Ok, commit};
    }

    bw::Result<void> truncate(const char* path, size_t len) override {
        countOp();
        if (shouldFailOp()) {
            return {bw::Err::HwFault};
        }
        auto it = files_.find(path);
        if (it == files_.end()) {
            return {bw::Err::NotFound};
        }
        if (it->second.size() > len) {
            it->second.resize(len);
        }
        return {bw::Err::Ok};
    }

    bw::Result<void> remove(const char* path) override {
        countOp();
        if (shouldFailOp()) {
            return {bw::Err::HwFault};
        }
        return files_.erase(path) > 0 ? bw::Result<void>{bw::Err::Ok}
                                      : bw::Result<void>{bw::Err::NotFound};
    }

    bw::Result<void> rename(const char* from, const char* to) override {
        countOp();
        if (shouldFailOp()) {
            return {bw::Err::HwFault};
        }
        auto it = files_.find(from);
        if (it == files_.end()) {
            return {bw::Err::NotFound};
        }
        files_[to] = std::move(it->second);  // overwrite semantics (flash_fs.h)
        files_.erase(it);
        return {bw::Err::Ok};
    }

    // --- fault injection ------------------------------------------------------
    // The next write (after `skip_writes` ordinary writes) commits exactly
    // `bytes` of its payload then fails; bytes >= len behaves as a normal
    // successful write. Lets a test tear a specific write inside a
    // multi-write operation (e.g. the payload after a header, D4).
    void armTornWrite(int64_t bytes, size_t skip_writes = 0) {
        torn_next_ = bytes;
        torn_skip_ = skip_writes;
    }
    // Every n-th mutating op fails with no side effect; 0 disables.
    void failOpsEveryN(size_t n) { fail_every_n_ = n; }
    size_t opCount() const { return ops_; }

    // F-03: model an UNREADABLE medium that still holds its files —
    // formatted_ = false makes mount(false) return NotReady while the bytes
    // stay put, so a test can prove the fallback format really wipes them.
    // (Real hardware cannot tell blank from corrupt through IFlashFs; the
    // fake stages the case the store must survive and REPORT, not inspect.)
    void forceUnformatted() { formatted_ = false; }

    // F-04: the next read() of exactly `path` fails with HwFault and no side
    // effect — a transient IO error on a probe. One-shot, like armTornWrite.
    void failNextReadFor(const std::string& path) { fail_read_path_ = path; }

    // --- test helpers ---------------------------------------------------------
    void corruptByte(const std::string& path, size_t offset) {
        auto it = files_.find(path);
        if (it != files_.end() && offset < it->second.size()) {
            it->second[offset] = static_cast<uint8_t>(it->second[offset] ^ 0xFF);
        }
    }
    void setBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
        files_[path] = bytes;
    }
    std::vector<uint8_t>* file(const std::string& path) {
        auto it = files_.find(path);
        return it == files_.end() ? nullptr : &it->second;
    }
    size_t fileCount() const { return files_.size(); }
    uint32_t usedBytes() const {
        uint32_t total = 0;
        for (const auto& kv : files_) {
            total += static_cast<uint32_t>(kv.second.size());
        }
        return total;
    }

private:
    void countOp() { ++ops_; }
    bool shouldFailOp() {
        if (fail_every_n_ == 0) {
            return false;
        }
        return ops_ % fail_every_n_ == 0;
    }

    uint32_t capacity_;
    bool formatted_ = false;
    bool mounted_ = false;
    std::map<std::string, std::vector<uint8_t>> files_;

    int64_t torn_next_ = -1;
    size_t torn_skip_ = 0;
    size_t fail_every_n_ = 0;
    size_t ops_ = 0;
    std::string fail_read_path_;  // F-04 one-shot transient read failure
};
