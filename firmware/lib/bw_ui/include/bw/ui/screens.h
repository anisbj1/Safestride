#pragma once

// bw/ui/screens.h — F12 D1/§7: the render layer.
//
// Free functions draw exactly one screen (or the banner overlay) from a
// UiFrame; they are stateless and golden-testable. ScreenRenderer adds the
// only state the frame path needs: the power transition (D8) and the
// consecutive-flush-failure counter with its single fault report per episode
// (§10). No canvas lives here — F01 owns the framebuffer (§11).

#include <cstdint>

#include <bw/fault.h>
#include <bw/hardware.h>
#include <bw/types.h>

#include <bw/ui/ui_config.h>
#include <bw/ui/ui_model.h>

namespace bw::ui {

// One-screen draw ops; each is pure (frame in, canvas ops out).
void renderHome(const UiFrame& frame, bw::IDisplayCanvas& canvas);
void renderPrompt(const UiFrame& frame, bw::IDisplayCanvas& canvas);
void renderDiagnostics(const UiFrame& frame, bw::IDisplayCanvas& canvas);
void renderMenu(const UiFrame& frame, bw::IDisplayCanvas& canvas);
// D3 overlay: drawn AFTER the active screen, over its top band.
void renderBanner(const UiFrame& frame, bw::IDisplayCanvas& canvas);

class ScreenRenderer {
public:
    explicit ScreenRenderer(const UiConfig& config = defaultUiConfig());

    // nullptr-safe (F01 D7 fallback): while unattached, flush failures are
    // only counted, never reported (§10).
    void attachFaultReporter(bw::IFaultReporter* reporter) { faults_ = reporter; }

    // Presents one frame: applies the power transition, and while powered on
    // draws the screen (+ banner overlay) and flushes. A failed flush is
    // retried next frame; kFlushFailFaultThreshold consecutive failures
    // produce exactly ONE DisplayFailed report until a success resets the
    // episode (§10).
    void render(const UiFrame& frame, bw::IDisplayCanvas& canvas, bw::TimeMs now);

    // Observable for tests/diagnostics.
    uint8_t consecutiveFlushFailures() const { return flush_failures_; }
    bool powerOn() const { return power_on_; }

private:
    bw::IFaultReporter* faults_ = nullptr;
    uint8_t flush_fail_threshold_;
    bool power_on_ = false;  // FakeCanvas/F01 panel starts unpowered (D8)
    uint8_t flush_failures_ = 0;
};

}  // namespace bw::ui
