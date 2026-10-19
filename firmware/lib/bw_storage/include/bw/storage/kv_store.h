#pragma once

// bw/storage/kv_store.h — KvStore: F18's bw::IKeyValueStore implementation
// (F18 spec §15.2, §15.3 D7).
//
// One file per key under kv/: payload = {magic, schema version, len} +
// CRC32-last. put() writes kv/<key>.tmp (header+payload, then the CRC in a
// second write so any tear leaves an invalid file), then atomically renames
// over the destination (littlefs overwrite-rename) — a power cut yields the
// OLD value or the NEW value, never a torn mix. get(): missing key ->
// Err::NotFound; CRC/version failure -> Err::Corrupt + kv_discards++ + fault
// (the caller discards and restarts from defaults, per the storage design).
// No RAM index: get/put resolve lazily — KV traffic is small and infrequent.

#include <cstddef>
#include <cstdint>

#include <bw/fault.h>
#include <bw/providers.h>

#include <bw/storage/constants.h>
#include <bw/storage/counters.h>
#include <bw/storage/flash_fs.h>

namespace bw::storage {

class KvStore : public bw::IKeyValueStore {
public:
    // Stored envelope: 'F18K' + schema version + uint16 len + payload + CRC32.
    static constexpr uint32_t kKvMagic = 0x4B384631;  // 'F18K'
    static constexpr uint8_t kKvSchemaVersion = 1;
    static constexpr size_t kKvHeaderBytes = 8;
    static constexpr size_t kKvCrcBytes = 4;

    KvStore(IFlashFs& fs, StorageCounters& counters);

    void attachFaultReporter(bw::IFaultReporter* reporter) { faults_ = reporter; }

    // --- bw::IKeyValueStore ---------------------------------------------------
    Result<size_t> get(const char* key, void* buf, size_t cap) override;
    Result<void> put(const char* key, const void* data, size_t len) override;

private:
    // Path-safe key check: [A-Za-z0-9_.-], non-empty, <= kKvKeyMaxLen — a key
    // can never escape kv/ or produce an unbuildable path.
    static bool validKey(const char* key);
    void keyPath(const char* key, char* out, size_t cap) const;
    void tmpPath(const char* key, char* out, size_t cap) const;

    IFlashFs& fs_;
    StorageCounters& counters_;
    bw::IFaultReporter* faults_ = nullptr;
};

}  // namespace bw::storage
