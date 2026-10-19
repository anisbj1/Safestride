#include <bw/ui/screens.h>
#include <cstring>

#include <bw/ui/icons.h>
#include <bw/ui/text_layout.h>

namespace bw::ui {

namespace {

const char* alertnessText(bw::AlertnessState state) {
    switch (state) {
    case bw::AlertnessState::Awake:
        return "Awake";
    case bw::AlertnessState::Drowsy:
        return "Drowsy";
    case bw::AlertnessState::HighDrowsinessRisk:
        return "High drowsy";  // truncated honestly if wider than the cap (D7)
    case bw::AlertnessState::PossibleSleep:
        return "Sleep?";
    case bw::AlertnessState::NotWorn:
        return "Not worn";
    case bw::AlertnessState::Unknown:
        break;
    }
    return "Unknown";
}

const char* wearText(bw::WearState state) {
    switch (state) {
    case bw::WearState::Worn:
        return "Worn";
    case bw::WearState::NotWorn:
        return "Not worn";
    case bw::WearState::Uncertain:
        break;
    }
    return "Uncertain";
}

// §10: an unknown source renders an honest token, never a guessed value.
// Tokens track the DataQuality vocabulary ("ok" / "degraded" /
// "insufficient"); "insufficient" may truncate on the combined line — the
// same honest truncation D7 applies to "High drowsy" (audit observation).
const char* qualityText(bw::DataQuality quality) {
    switch (quality) {
    case bw::DataQuality::Good:
        return "ok";
    case bw::DataQuality::Degraded:
        return "degraded";
    case bw::DataQuality::Insufficient:
        break;
    }
    return "insufficient";
}

const char* bannerLevelText(bw::AlertLevel level) {
    switch (level) {
    case bw::AlertLevel::Warning:
        return "Check?";
    case bw::AlertLevel::Active:
        return "Drowsy?";
    case bw::AlertLevel::Persistent:
        return "Persistent";
    case bw::AlertLevel::Normal:
        break;
    }
    return "";
}

}  // namespace

void renderHome(const UiFrame& frame, bw::IDisplayCanvas& canvas) {
    const HomeData& home = frame.home;
    // Every row below draws in the Medium font: the buffer is sized to the
    // Medium cap, so a future long string truncates at 16 chars (128 px),
    // never beyond the panel (audit F-2).
    MediumLineBuffer line{};

    // Row 1 (Medium, y=0): the alertness state, or an honest "No data" at
    // startup when no StateSnapshot was ever published (§9 edge case).
    if (home.snapshot_valid) {
        copyTruncated(line, alertnessText(home.alertness));
    } else {
        copyTruncated(line, "No data");
    }
    canvas.drawText(0, 0, line.data(), bw::FontId::Medium);

    // Row 2 (Medium, y=16): wear state (independent axis, DEC-004).
    copyTruncated(line, home.snapshot_valid ? wearText(home.wear) : "--");
    canvas.drawText(0, kLinePitchMedium, line.data(), bw::FontId::Medium);

    // Row 3 (Medium, y=32): heart rate. Two honest states (§10, 2026-10-15
    // developer reversal of the same-day `~ … ?` marker): "HR 72" for ANY
    // fresh plausible rhythm — quality gates stay inside F02 (alerts,
    // cataplexy, hr_confidence) and the live number self-corrects within
    // seconds — and "HR --" when there are no plausible beats or the data
    // is stale (§8 kHrStaleMs).
    if (home.hr_valid) {
        formatLine(line, "HR %u", static_cast<unsigned>(home.hr_bpm));
    } else {
        copyTruncated(line, "HR --");
    }
    canvas.drawText(0, 2 * kLinePitchMedium, line.data(), bw::FontId::Medium);

    // Row 4 (Medium, y=48): battery icon + percentage; "--" when the divider
    // is absent (A-04 / bench: valid == false). The text starts at x=16 after
    // the icon; its width is bounded by construction ("%u%%" of a uint8_t is
    // at most 4 chars), well inside the offset budget.
    canvas.drawBitmap(0, 3 * kLinePitchMedium, kBatteryIconXbm, kBatteryIconW, kBatteryIconH);
    if (home.battery_valid) {
        formatLine(line, "%u%%", static_cast<unsigned>(home.battery_percent));
    } else {
        copyTruncated(line, "--");
    }
    canvas.drawText(16, 3 * kLinePitchMedium, line.data(), bw::FontId::Medium);
}

void renderPrompt(const UiFrame& frame, bw::IDisplayCanvas& canvas) {
    const PromptData& prompt = frame.prompt;
    // D6 degraded placeholder: no provider, no state yet, or the session ended
    // (phase Idle). Kept as the honest "unknown" output (§10) — the screen
    // never invents a prompt that F14 is not offering.
    if (!prompt.available) {
        MediumLineBuffer line{};
        copyTruncated(line, "No prompts");
        canvas.drawText(0, 0, line.data(), bw::FontId::Medium);
        return;
    }

    // Row 1 (Medium): what is being asked. The phase IS the question, so it
    // gets the readable font; "Offered" is the actionable one, "Deferred" tells
    // the user the prompt will come back rather than vanish.
    MediumLineBuffer headline{};
    copyTruncated(headline,
                  prompt.phase == bw::ConfirmPromptPhase::Deferred ? "Deferred" : "Confirm?");
    canvas.drawText(0, 0, headline.data(), bw::FontId::Medium);

    // Rows 2-4 (Small): the facts behind the ask. Each buffer is sized to the
    // Small cap (audit F-2).
    SmallLineBuffer line{};
    formatLine(line, "event %lu", static_cast<unsigned long>(prompt.seq));
    canvas.drawText(0, kLinePitchSmall, line.data(), bw::FontId::Small);

    // The reply countdown. "--" when it is unknown (no clock, or the phase has
    // no deadline) — an honest gap beats a fabricated number (§10).
    if (prompt.deadline_valid) {
        formatLine(line, "reply in %us", static_cast<unsigned>(prompt.seconds_left));
    } else {
        copyTruncated(line, "reply --");
    }
    canvas.drawText(0, 2 * kLinePitchSmall, line.data(), bw::FontId::Small);

    formatLine(line, "queued %u", static_cast<unsigned>(prompt.queue_depth));
    canvas.drawText(0, 3 * kLinePitchSmall, line.data(), bw::FontId::Small);
}

void renderDiagnostics(const UiFrame& frame, bw::IDisplayCanvas& canvas) {
    const DiagData& diag = frame.diag;
    // Every diagnostics row draws in the Small font: the buffer is sized to
    // the Small cap (audit F-2).
    SmallLineBuffer line{};

    if (!diag.available) {
        // W2 default: no F23 health mailbox yet — every line says so.
        copyTruncated(line, "imu:n/a ppg:n/a");
        canvas.drawText(0, 0, line.data(), bw::FontId::Small);
        copyTruncated(line, "mflt:n/a");
        canvas.drawText(0, kLinePitchSmall, line.data(), bw::FontId::Small);
        copyTruncated(line, "heap:n/a");
        canvas.drawText(0, 2 * kLinePitchSmall, line.data(), bw::FontId::Small);
        copyTruncated(line, "ble:n/a mode:n/a");
        canvas.drawText(0, 3 * kLinePitchSmall, line.data(), bw::FontId::Small);
        return;
    }

    formatLine(line, "imu:%s ppg:%s", qualityText(diag.imu_quality), qualityText(diag.ppg_quality));
    canvas.drawText(0, 0, line.data(), bw::FontId::Small);
    // Backlog 24: "mflt" = MONITOR-scoped faults, on purpose. This number is
    // HealthSnapshot.fault_bits — only what bw_diag's own monitor diagnosed
    // (diagnostics.h D8) — while the app-level ctx.faults registry is a
    // separate ledger that also holds F01's boot reports. The label says which
    // one it is, so a 0 here next to a non-empty registry reads as "the
    // monitor saw nothing", not as a bug (the bench confusion that opened
    // backlog 24). One line, no data change: fusing the two ledgers needs a
    // composition-level fan-out and is deliberately NOT this fix.
    formatLine(line, "mflt:%lu", static_cast<unsigned long>(diag.fault_bits));
    canvas.drawText(0, kLinePitchSmall, line.data(), bw::FontId::Small);
    formatLine(line, "heap %lu", static_cast<unsigned long>(diag.min_free_heap));
    canvas.drawText(0, 2 * kLinePitchSmall, line.data(), bw::FontId::Small);
    formatLine(line, "ble:%s mode:%s", diag.ble_connected ? "yes" : "no",
               diag.degraded_mode ? "degraded" : "normal");
    canvas.drawText(0, 3 * kLinePitchSmall, line.data(), bw::FontId::Small);
}

void renderMenu(const UiFrame& frame, bw::IDisplayCanvas& canvas) {
    static_assert(sizeof(kMenuEntryNames) / sizeof(kMenuEntryNames[0]) == kMenuEntryCount,
                  "the menu name table must match kMenuEntryCount (screen_manager.h)");
    // Title in Medium, entries in Small right of the cursor: each buffer is
    // sized to its font AND its x-origin cap (audit F-2).
    MediumLineBuffer title{};
    MenuTextLineBuffer line{};
    copyTruncated(title, "Menu");
    canvas.drawText(0, 0, title.data(), bw::FontId::Medium);

    for (uint8_t i = 0; i < kMenuEntryCount; ++i) {
        const int16_t y = static_cast<int16_t>(kLinePitchMedium + i * kLinePitchSmall);
        // Cursor column: pointer + fixed x offset, never inside the text.
        copyTruncated(line, i == frame.menu.selected ? ">" : " ");
        canvas.drawText(0, y, line.data(), bw::FontId::Small);
        copyTruncated(line, kMenuEntryNames[i]);
        canvas.drawText(kMenuTextXPx, y, line.data(), bw::FontId::Small);
    }
}

void renderBanner(const UiFrame& frame, bw::IDisplayCanvas& canvas) {
    // The banner text starts right of the warning icon (kBannerTextXPx), so
    // it is formatted into the banner-sized buffer: "Persistent ACK" (14
    // chars) truncates to the 13-char offset cap rather than drawing past
    // 128 px (audit F-2).
    BannerLineBuffer line{};
    // D3: filled band over the top of whatever screen is active — redrawn
    // per backlog 21 + CR-013: the band is MASKED by erasing it (fillRect
    // off), not by painting it white. The old white fill + white draw-color
    // rendered white-on-white (the bench's solid white bar) and its text sat
    // at a clipped baseline. Icon and level text now draw normally over the
    // erased band, and a 1 px rule closes it at the bottom so it still reads
    // as an overlay band; inversion stays out of IDisplayCanvas (no new op).
    canvas.fillRect(0, 0, kScreenWidthPx, kBannerHeightPx, false);
    canvas.drawBitmap(1, 1, kWarningIconXbm, kWarningIconW, kWarningIconH);
    formatLine(line, "%s%s", bannerLevelText(frame.banner.level),
               frame.banner.acknowledged ? " ACK" : "");
    canvas.drawText(kBannerTextXPx, (kBannerHeightPx - kLinePitchMedium) / 2, line.data(),
                    bw::FontId::Medium);
    canvas.fillRect(0, kBannerHeightPx - 1, kScreenWidthPx, 1, true);
}

ScreenRenderer::ScreenRenderer(const UiConfig& config)
    : flush_fail_threshold_(config.flush_fail_threshold) {}

void ScreenRenderer::render(const UiFrame& frame, bw::IDisplayCanvas& canvas, bw::TimeMs now) {
    // D8: only a real transition touches setPower, so a timed-out display is
    // completely quiet (no draw, no flush) instead of re-asserting power.
    if (frame.power != power_on_) {
        canvas.setPower(frame.power);
        power_on_ = frame.power;
    }
    if (!frame.power) {
        return;
    }

    canvas.clear();
    if (frame.banner.visible && frame.banner.fullscreen) {
        // Never draw the base screen, even on the blank half of the blink.
        if (frame.banner.ink_visible) {
            const char* text =
                frame.banner.level == bw::AlertLevel::Warning ? "Drowsy?" : "Sleeping?";
            const int16_t width = static_cast<int16_t>(std::strlen(text)) * kGlyphWidthLargePx;
            canvas.drawText((kScreenWidthPx - width) / 2,
                            (kScreenHeightPx - kDemoBannerGlyphHeightPx) / 2, text,
                            bw::FontId::Large);
        }
    } else {
        switch (frame.screen) {
        case ScreenId::Home:
            renderHome(frame, canvas);
            break;
        case ScreenId::Prompt:
            renderPrompt(frame, canvas);
            break;
        case ScreenId::Diagnostics:
            renderDiagnostics(frame, canvas);
            break;
        case ScreenId::Menu:
            renderMenu(frame, canvas);
            break;
        }
        if (frame.banner.visible) {
            renderBanner(frame, canvas);
        }
    }

    const bw::Result<void> flushed = canvas.flush();
    if (flushed.ok()) {
        flush_failures_ = 0;
        return;
    }
    // §10: retry next frame; count consecutive failures; exactly ONE report
    // per failure episode (== threshold, not >=) so a dead panel logs one
    // line per episode instead of one per frame.
    if (flush_failures_ < 255) {
        ++flush_failures_;
    }
    if (flush_failures_ == flush_fail_threshold_ && faults_ != nullptr) {
        faults_->report(bw::FaultCode::DisplayFailed, bw::FaultSeverity::Error, now);
    }
}

}  // namespace bw::ui
