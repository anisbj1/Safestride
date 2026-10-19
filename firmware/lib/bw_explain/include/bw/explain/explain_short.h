// bw_explain — compact multi-reason line for the OLED (F22 §3 output 1,
// §6 D3/D4).
#pragma once

#include <cstddef>

#include <bw/explain/reason_catalog.h>
#include <bw/state.h>

namespace bw {
namespace explain {

// [EXPERIMENTAL] Max reasons rendered in one line: OLED space (F22 §8). When
// more bits are set, the tail is dropped by ascending bit index; a caller can
// show the remainder as '+N' from popcount(mask) — that wording is the UI's
// decision, not this formatter's.
constexpr uint8_t kMaxShortReasons = 3;

// [DERIVED] Longest single rendering: the worst case is the unknown-code form
// 'reason #31' (10 chars) inside a 14-char budget, with margin for wording
// edits. Enforced by test_reason_catalog.cpp, not by convention.
constexpr uint8_t kMaxShortReasonChars = 14;

// [DERIVED] Worst-case line = 3 * 14 chars + 2 * 2-char separators + NUL = 47,
// rounded up. Callers may pass any smaller cap: explainShort truncates safely.
constexpr size_t kExplainShortBufSize = 48;

// [DERIVED] ReasonMask is uint32_t (docs/interfaces/data_decision.md §4.5):
// bits 0..31 are walkable; a set bit with no catalog entry renders as
// 'reason #N' (F22 §10) and stays testable before such a code exists.
constexpr uint8_t kReasonCodeBitWidth = 32;

static_assert(kMaxShortReasons * kMaxShortReasonChars + (kMaxShortReasons - 1) * 2 + 1 <=
                  kExplainShortBufSize,
              "kExplainShortBufSize must hold the worst-case rendered line");

// Renders up to kMaxShortReasons set bits of mask, ascending by bit index,
// joined with ", "; a set bit with no catalog entry renders inline at its own
// position as 'reason #N' (never silent, F22 §10). snprintf semantics: the
// return value is the length that WOULD have been written, excluding the NUL.
// buf is NUL-terminated whenever cap > 0 and is never written at index >= cap;
// cap == 0 (or buf == nullptr) writes nothing but still reports the length.
// Empty mask => empty string, 0 (§6 D4). No heap, no static buffer: storage
// belongs to the caller (F22 §11).
size_t explainShort(ReasonMask mask, char* buf, size_t cap);

}  // namespace explain
}  // namespace bw
