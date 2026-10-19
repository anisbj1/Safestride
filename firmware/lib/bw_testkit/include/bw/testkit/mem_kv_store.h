#pragma once

#include <bw/testkit/host_only.h>

#include <bw/providers.h>
#include <bw/result.h>

#include <cstring>

namespace bw::testkit {

// [EXPERIMENTAL] Store geometry. Why 16 x 32 x 128: M0–M3 settings and feature
// keys number in the tens (each key < 32 bytes, each value < 128 bytes — a
// timestamp, a threshold, a short name); tests that need more instantiate a
// bigger InMemoryKeyValueStore<...>. Exceeding a per-entry limit is an argument
// error (Err::InvalidArg — it can never be stored, not a transient condition);
// a full table is Err::Full (CR-004: refused, nothing written).
constexpr size_t kDefaultKvEntries = 16;
constexpr size_t kDefaultKvKeyBytes = 32;
constexpr size_t kDefaultKvValueBytes = 128;

// InMemoryKeyValueStore — bw::IKeyValueStore fake (02 §5): a fixed table of
// key/value entries, no heap, no file system. F18's real KvStore (LittleFS) must
// satisfy the same get/put semantics this fake documents:
//  - put() on an existing key overwrites it (upsert);
//  - get() of an unknown key -> Err::NotFound;
//  - get() with a buffer smaller than the value -> Err::Full, nothing written
//    (CR-004 rule of thumb: the caller retries with a bigger buffer, nothing was
//    consumed).
template <size_t MaxEntries = kDefaultKvEntries, size_t MaxKeyBytes = kDefaultKvKeyBytes,
          size_t MaxValueBytes = kDefaultKvValueBytes>
class InMemoryKeyValueStore final : public bw::IKeyValueStore {
    static_assert(MaxEntries > 0, "store needs at least one entry");
    static_assert(MaxKeyBytes > 0, "key must fit at least one byte plus NUL");
    static_assert(MaxValueBytes > 0, "value slot must hold at least one byte");

public:
    bw::Result<size_t> get(const char* key, void* buf, size_t cap) override {
        if (key == nullptr || key[0] == '\0') {
            return {bw::Err::InvalidArg, 0};
        }
        const Entry* entry = find(key);
        if (entry == nullptr) {
            return {bw::Err::NotFound, 0};
        }
        if (cap < entry->value_len && buf != nullptr) {
            return {bw::Err::Full, 0};  // CR-004: destination too small, nothing written
        }
        if (entry->value_len > 0) {
            if (buf == nullptr) {
                return {bw::Err::InvalidArg, 0};
            }
            std::memcpy(buf, entry->value, entry->value_len);
        }
        return {bw::Err::Ok, entry->value_len};
    }

    bw::Result<void> put(const char* key, const void* data, size_t len) override {
        if (key == nullptr || key[0] == '\0') {
            return {bw::Err::InvalidArg};
        }
        const size_t key_len = std::strlen(key);
        if (key_len >= MaxKeyBytes || len > MaxValueBytes) {
            // Too large for this store's geometry — no amount of retrying helps,
            // so this is an argument error, not Err::Full.
            return {bw::Err::InvalidArg};
        }
        if (len > 0 && data == nullptr) {
            return {bw::Err::InvalidArg};
        }

        Entry* entry = findMutable(key);
        if (entry == nullptr) {
            entry = freeEntry();
            if (entry == nullptr) {
                return {bw::Err::Full};  // CR-004: table full, refused, nothing written
            }
            std::memcpy(entry->key, key, key_len);
            entry->key[key_len] = '\0';
            entry->key_len = key_len;
            entry->used = true;
        }
        if (len > 0) {
            std::memcpy(entry->value, data, len);
        }
        entry->value_len = len;
        return {bw::Err::Ok};
    }

    size_t count() const {
        size_t n = 0;
        for (const Entry& e : entries_) {
            n += e.used ? 1 : 0;
        }
        return n;
    }

private:
    struct Entry {
        bool used = false;
        char key[MaxKeyBytes] = {'\0'};
        size_t key_len = 0;
        uint8_t value[MaxValueBytes] = {0};
        size_t value_len = 0;
    };

    const Entry* find(const char* key) const {
        const size_t key_len = std::strlen(key);
        for (const Entry& e : entries_) {
            if (e.used && e.key_len == key_len && std::memcmp(e.key, key, key_len) == 0) {
                return &e;
            }
        }
        return nullptr;
    }

    Entry* findMutable(const char* key) { return const_cast<Entry*>(find(key)); }

    Entry* freeEntry() {
        for (Entry& e : entries_) {
            if (!e.used) {
                return &e;
            }
        }
        return nullptr;
    }

    Entry entries_[MaxEntries];
};

}  // namespace bw::testkit
