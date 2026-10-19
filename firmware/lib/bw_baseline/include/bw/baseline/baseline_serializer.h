#pragma once

// bw/baseline/baseline_serializer.h — F13 D3: the versioned 32 B blob codec
// and its IKeyValueStore glue. Pure logic over bw_core contracts only.

#include <bw/baseline/baseline_types.h>
#include <bw/providers.h>
#include <bw/result.h>

#include <cstddef>
#include <cstdint>

namespace bw::baseline {

class BaselineSerializer {
public:
    // Encodes `in` as exactly kBlobBytes (little-endian field layout, CRC-32 of
    // the leading bytes). Err::Full with NOTHING written when cap < kBlobBytes
    // (CR-004 — the caller retries with a bigger buffer, input untouched).
    static bw::Result<size_t> serialize(const TrackerState& in, uint8_t* buf, size_t cap);

    // Decodes a blob produced by serialize(). Err::Corrupt for any wrong
    // size/magic/reserved/CRC, Err::Unsupported for an unknown schema — both
    // are discarded by the caller (D3, §10). `out` is only written on success.
    static bw::Result<void> deserialize(const uint8_t* data, size_t len, TrackerState& out);

    // Store glue (D3/§10): store == nullptr or a missing key => Err::NotFound
    // (fresh boot — an ordinary state, no fault). Every other store error is
    // propagated verbatim so wiring can distinguish an unusable blob (Corrupt/
    // Unsupported/Full => report + discard) from a transient failure (retry).
    static bw::Result<void> loadFromStore(bw::IKeyValueStore* store, TrackerState& out);
    static bw::Result<void> saveToStore(bw::IKeyValueStore* store, const TrackerState& in);
};

}  // namespace bw::baseline
