#include <bw/explain/explain_short.h>

#include <cstdio>
#include <cstring>

namespace bw {
namespace explain {
namespace {

// Appends src[0..len) under snprintf semantics: pos ALWAYS advances by len so
// the return value reports the full untruncated length, but a character is
// stored only while it fits below the NUL slot (writable = cap - 1, 0 when
// cap == 0), so the buffer can never overflow (F22 §6 D3).
void appendClamped(char* buf, size_t writable, size_t& pos, const char* src, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (pos < writable) {
            buf[pos] = src[i];
        }
        ++pos;
    }
}

}  // namespace

size_t explainShort(ReasonMask mask, char* buf, size_t cap) {
    if (buf == nullptr) {
        cap = 0;
    }
    const size_t writable = (cap > 0) ? cap - 1 : 0;

    size_t pos = 0;
    bool first = true;
    uint8_t shown = 0;

    for (uint8_t bit = 0; bit < kReasonCodeBitWidth && shown < kMaxShortReasons; ++bit) {
        if ((mask & (static_cast<ReasonMask>(1) << bit)) == 0) {
            continue;
        }
        const char* text = reasonText(static_cast<ReasonCode>(bit));
        size_t len = 0;
        char unknown[kMaxShortReasonChars + 1];
        if (text != nullptr) {
            len = std::strlen(text);
        } else {
            // Unknown code: render 'reason #N' inline (F22 §10). Bounded by
            // construction — bit < 32 means at most 'reason #31' + NUL, which
            // fits unknown[] — and clamped defensively if snprintf ever says
            // otherwise, so pos and the stored text stay consistent.
            const int written =
                std::snprintf(unknown, sizeof(unknown), "reason #%u", static_cast<unsigned>(bit));
            len = (written > 0) ? static_cast<size_t>(written) : 0;
            if (len >= sizeof(unknown)) {
                len = sizeof(unknown) - 1;
            }
            text = unknown;
        }
        if (!first) {
            appendClamped(buf, writable, pos, ", ", 2);
        }
        appendClamped(buf, writable, pos, text, len);
        first = false;
        ++shown;
    }

    if (cap > 0) {
        buf[(pos < writable) ? pos : writable] = '\0';
    }
    return pos;
}

}  // namespace explain
}  // namespace bw
