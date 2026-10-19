#include <bw/baseline/baseline_serializer.h>

#include <bw/baseline/baseline_config.h>

#include <cstring>

namespace bw::baseline {

namespace {

// ---- blob layout (D3, §8: kBlobSchemaV1) ------------------------------------
//   0  u32 magic (kBlobMagic)   12 f32 hr_absdev_ewma
//   4  u16 schema               16 f32 enmo_ewma
//   6  u16 flags (0)            20 u32 accepted_samples
//   8  f32 hr_ewma              24 u32 reserved (0)
//                               28 u32 crc32 over bytes [0,28)
constexpr uint16_t kOffMagic = 0;
constexpr uint16_t kOffSchema = 4;
constexpr uint16_t kOffFlags = 6;
constexpr uint16_t kOffHrEwma = 8;
constexpr uint16_t kOffHrAbsdevEwma = 12;
constexpr uint16_t kOffEnmoEwma = 16;
constexpr uint16_t kOffSamples = 20;
constexpr uint16_t kOffReserved = 24;
constexpr uint16_t kOffCrc = 28;
// [DERIVED] CRC covers every byte before the CRC field itself (layout above).
constexpr uint16_t kCrcOffset = kOffCrc;

// CRC-32, reflected form, polynomial 0xEDB88320 — the same polynomial
// bw/storage's record_codec uses, reimplemented here as ~15 bitwise lines
// because 01 §1 rule 1 forbids a feature lib from including another feature's
// headers; same polynomial ⇒ values are comparable across the two (D3).
uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint32_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// Explicit little-endian field IO: the blob must decode identically on host
// (x86) and ESP32 regardless of native struct padding rules.
void putU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
}
void putU32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xFFu);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}
void putF32(uint8_t* p, float v) {
    uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    putU32(p, bits);
}
uint16_t getU16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}
uint32_t getU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
float getF32(const uint8_t* p) {
    const uint32_t bits = getU32(p);
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

}  // namespace

bw::Result<size_t> BaselineSerializer::serialize(const TrackerState& in, uint8_t* buf, size_t cap) {
    if (buf == nullptr) {
        return {bw::Err::InvalidArg, 0};
    }
    if (cap < kBlobBytes) {
        return {bw::Err::Full, 0};  // CR-004: refused, nothing written
    }
    putU32(buf + kOffMagic, kBlobMagic);
    putU16(buf + kOffSchema, kBlobSchemaV1);
    putU16(buf + kOffFlags, 0u);  // reserved for future feature flags
    putF32(buf + kOffHrEwma, in.hr_ewma);
    putF32(buf + kOffHrAbsdevEwma, in.hr_absdev_ewma);
    putF32(buf + kOffEnmoEwma, in.enmo_ewma);
    putU32(buf + kOffSamples, in.accepted_samples);
    putU32(buf + kOffReserved, 0u);  // v1 writes zero here (D3)
    putU32(buf + kOffCrc, crc32(buf, kCrcOffset));
    return {bw::Err::Ok, static_cast<size_t>(kBlobBytes)};
}

bw::Result<void> BaselineSerializer::deserialize(const uint8_t* data, size_t len,
                                                 TrackerState& out) {
    // Strict size: our blob is exactly kBlobBytes, so a truncated OR oversized
    // read means it is not the v1 layout (§9 test 12).
    if (data == nullptr || len != kBlobBytes) {
        return {bw::Err::Corrupt};
    }
    // Order matters: magic first (a flipped magic byte is corruption), then
    // schema (a well-formed header from a future version is Unsupported), then
    // layout-reserved fields, then CRC over everything (§9 test 11).
    if (getU32(data + kOffMagic) != kBlobMagic) {
        return {bw::Err::Corrupt};
    }
    if (getU16(data + kOffSchema) != kBlobSchemaV1) {
        return {bw::Err::Unsupported};
    }
    if (getU16(data + kOffFlags) != 0u || getU32(data + kOffReserved) != 0u) {
        return {bw::Err::Corrupt};  // nonzero "reserved" = not our layout
    }
    if (getU32(data + kOffCrc) != crc32(data, kCrcOffset)) {
        return {bw::Err::Corrupt};
    }
    // Decode into a local first: on any error `out` must stay untouched (§9 12).
    TrackerState decoded{};
    decoded.hr_ewma = getF32(data + kOffHrEwma);
    decoded.hr_absdev_ewma = getF32(data + kOffHrAbsdevEwma);
    decoded.enmo_ewma = getF32(data + kOffEnmoEwma);
    decoded.accepted_samples = getU32(data + kOffSamples);
    out = decoded;
    return {bw::Err::Ok};
}

bw::Result<void> BaselineSerializer::loadFromStore(bw::IKeyValueStore* store, TrackerState& out) {
    if (store == nullptr) {
        return {bw::Err::NotFound};  // no store yet = fresh boot (§10), not an error
    }
    uint8_t buf[kBlobBytes];
    const bw::Result<size_t> got = store->get(kStoreKey, buf, kBlobBytes);
    if (!got.ok()) {
        return {got.err};  // NotFound (first boot), Full (blob too big = not v1) or transient
    }
    if (got.value != kBlobBytes) {
        return {bw::Err::Corrupt};  // shorter than the v1 layout (store allows it, we don't)
    }
    return deserialize(buf, got.value, out);
}

bw::Result<void> BaselineSerializer::saveToStore(bw::IKeyValueStore* store,
                                                 const TrackerState& in) {
    if (store == nullptr) {
        return {bw::Err::NotFound};
    }
    uint8_t buf[kBlobBytes];
    const bw::Result<size_t> encoded = serialize(in, buf, kBlobBytes);
    if (!encoded.ok()) {
        return {encoded.err};
    }
    return store->put(kStoreKey, buf, encoded.value);
}

}  // namespace bw::baseline
