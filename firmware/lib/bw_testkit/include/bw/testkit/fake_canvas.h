#pragma once

#include <bw/testkit/host_only.h>

#include <bw/hardware.h>
#include <bw/result.h>

#include <cstdarg>
#include <cstdio>

namespace bw::testkit {

// [EXPERIMENTAL] Golden-log capacity (bytes, NUL excluded). Why 4096: one
// 128x64 screen's draw sequence (a dozen ops at ~30 chars each) is well under
// 1 KB, so 4 KB holds several frames before content is dropped; overflow is
// counted in dropped(), never a crash (CR-004).
constexpr size_t kFakeCanvasLogBytes = 4096;

// FakeCanvas — bw::IDisplayCanvas fake (02 §3): appends every drawing operation
// tests" compare against (a human-readable log diffs better than a framebuffer).
//
// clear() is a canvas operation and only LOGS itself — it does not erase the log;
// use clearLog() to start a new golden. flush() returns a configurable result so
// tests can inject an I2C/display failure (DoD item 7: failure behaviour).
class FakeCanvas final : public bw::IDisplayCanvas {
public:
    void clear() override { append("clear\n"); }

    void drawText(int16_t x, int16_t y, const char* text, bw::FontId font) override {
        appendf("text %d,%d %s \"%s\"\n", static_cast<int>(x), static_cast<int>(y), fontName(font),
                text != nullptr ? text : "");
    }

    void drawBitmap(int16_t x, int16_t y, const uint8_t* /*xbm*/, uint8_t w, uint8_t h) override {
        appendf("bitmap %d,%d %u,%u\n", static_cast<int>(x), static_cast<int>(y),
                static_cast<unsigned>(w), static_cast<unsigned>(h));
    }

    void fillRect(int16_t x, int16_t y, uint8_t w, uint8_t h, bool on) override {
        appendf("rect %d,%d %u,%u %s\n", static_cast<int>(x), static_cast<int>(y),
                static_cast<unsigned>(w), static_cast<unsigned>(h), on ? "on" : "off");
    }

    void setPower(bool on) override {
        power_ = on;
        appendf("power %s\n", on ? "on" : "off");
    }

    void setContrast(uint8_t contrast) override {
        contrast_ = contrast;
        appendf("contrast %u\n", static_cast<unsigned>(contrast));
    }

    bw::Result<void> flush() override {
        ++flush_count_;
        appendf("flush %s\n", flush_err_ == bw::Err::Ok ? "ok" : "failed");
        return {flush_err_};
    }

    // Injects the result flush() returns from now on (default Err::Ok).
    void setFlushResult(bw::Err err) { flush_err_ = err; }

    const char* log() const { return log_; }
    void clearLog() {
        used_ = 0;
        log_[0] = '\0';
        dropped_ = 0;
    }

    size_t flushCount() const { return flush_count_; }
    // Ops that did not fit the fixed log (CR-004 Overflow sense).
    size_t dropped() const { return dropped_; }

    // Last values written, independent of the log.
    bool power() const { return power_; }
    // 0 until setContrast() is first called — read the log to distinguish "never
    // set" from "set to 0" in assertions that care.
    uint8_t contrast() const { return contrast_; }

private:
    static const char* fontName(bw::FontId font) {
        switch (font) {
        case bw::FontId::Small:
            return "small";
        case bw::FontId::Medium:
            return "medium";
        case bw::FontId::Large:
            return "large";
        }
        return "small";
    }

    // printf-style append into the fixed log; never reallocates, never crashes —
    // a truncated (or dropped) op is counted instead.
    void appendf(const char* fmt, ...) {
        if (used_ >= kFakeCanvasLogBytes) {
            ++dropped_;
            return;
        }
        va_list args;
        va_start(args, fmt);
        const int written = vsnprintf(log_ + used_, kFakeCanvasLogBytes - used_ + 1, fmt, args);
        va_end(args);
        if (written < 0 || static_cast<size_t>(written) >= kFakeCanvasLogBytes - used_) {
            // Truncated: keep what was written, count the op as lost.
            used_ = kFakeCanvasLogBytes;
            log_[kFakeCanvasLogBytes] = '\0';
            ++dropped_;
            return;
        }
        used_ += static_cast<size_t>(written);
    }

    void append(const char* text) { appendf("%s", text); }

    // +1 for the NUL so kFakeCanvasLogBytes is always usable log space.
    char log_[kFakeCanvasLogBytes + 1] = {'\0'};
    size_t used_ = 0;
    size_t dropped_ = 0;
    size_t flush_count_ = 0;
    bw::Err flush_err_ = bw::Err::Ok;
    bool power_ = false;  // starts off, like an unpowered panel (F12 turns it on)
    uint8_t contrast_ = 0;
};

}  // namespace bw::testkit
