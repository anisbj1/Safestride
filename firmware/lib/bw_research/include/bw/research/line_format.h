#pragma once

// bw/research/line_format.h — F28 serial line grammar (02 §6.4, docs/contracts/
// DATASET_FORMAT.md): one pure formatter per row type, no platform calls, no
// heap, fixed buffers only (embedded conventions).
//
// Contract (F28 §10): a formatter writes at most one '\n'-terminated,
// NUL-terminated line and returns the byte count including '\n'. Return 0 means
// "line does not fit in cap" — out is left untouched (Err::Full sense, CR-004)
// and the caller drops the row and counts it. Malformed input is never emitted
// as a partial line.

#include <cstddef>
#include <cstdint>

#include <bw/context.h>
#include <bw/events.h>
#include <bw/features.h>
#include <bw/hardware.h>
#include <bw/quality.h>

namespace bw::research {

// #HDR payload: schema + identity + cumulative drop counters (D10). Everything
// the host needs to interpret a capture that cannot be derived from rows.
struct HeaderInfo {
    uint32_t schema_version;
    uint32_t boot_id;              // 0 = unknown (kUnknownBootId)
    const char* firmware_version;  // null → "unknown"; sanitised, truncated
    uint32_t imu_drops;            // tee pushes refused while enabled
    uint32_t ppg_drops;
    uint32_t line_drops;      // rows that did not fit the line ring
    uint32_t feature_missed;  // mailbox values overwritten before we read
};

size_t formatHeader(char* out, size_t cap, const HeaderInfo& info);
size_t formatImu(char* out, size_t cap, const bw::ImuSample& sample);
size_t formatPpg(char* out, size_t cap, const bw::PpgSample& sample);
size_t formatMotionFeatures(char* out, size_t cap, const bw::MotionFeatures& features);
size_t formatPpgFeatures(char* out, size_t cap, const bw::PpgFeatures& features);
size_t formatContext(char* out, size_t cap, const bw::ActivityEstimate& estimate);
size_t formatEvent(char* out, size_t cap, const bw::Event& event);
size_t formatMarker(char* out, size_t cap, bw::TimeMs t_ms, uint32_t event_seq);

}  // namespace bw::research
