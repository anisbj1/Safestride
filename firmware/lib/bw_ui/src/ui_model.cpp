#include <bw/ui/ui_model.h>

#include <cmath>

namespace bw::ui {

namespace {

// Rounds a positive float to uint16 without printf %f (newlib's float printf
// is heavy and may allocate — §11: no heap in the frame path).
uint16_t roundToU16(float value, uint16_t max_value) {
    if (!(value > 0.0f)) {  // also catches NaN: compare false, honest 0
        return 0;
    }
    const float rounded = value + 0.5f;
    return static_cast<uint16_t>(rounded > static_cast<float>(max_value) ? max_value : rounded);
}

// Audit F-4 (defense in depth): a non-finite or negative measurement
// (NaN/±inf from a glitching sensor or ADC) is INVALID data, not a value to
// render — mapping it to 0 or 999 would fabricate a reading from bad input
// ("unknown" is the honest output, §10). Finite non-negative values still go
// through roundToU16: 0 stays 0, so an honest empty battery reads "0%", not
// "--".
bool isRenderableMeasurement(float value) {
    return std::isfinite(value) && value >= 0.0f;
}

}  // namespace

UiModel::UiModel(const UiConfig& config) : config_(config), screens_(config.alert_min_show_ms) {}

void UiModel::handleCommand(const bw::Command& cmd, bw::TimeMs now) {
    screens_.handleCommand(cmd);
    // D8: any received command is user activity — reset the timeout.
    last_activity_ms_ = now;
}

UiFrame UiModel::step(const UiInputs& inputs, bw::TimeMs now) {
    UiFrame frame{};

    // --- alert overlay (D3) ------------------------------------------------
    bw::AlertStatus alert{};
    const bool has_alert = inputs.alert != nullptr && inputs.alert->get(alert);
    const bool alert_active = has_alert && alert.level >= bw::AlertLevel::Warning;
    const ScreenManager::AlertView view = screens_.updateAlert(alert_active, alert.level, now);
    if (view.raised) {
        // D8: an alert raise wakes the display from any timeout.
        last_activity_ms_ = now;
    }

    // --- power / timeout (D8) ----------------------------------------------
    // Backward time cannot happen with one monotonic clock; guard anyway so a
    // bogus `now` keeps the screen on rather than off forever. A configured
    // timeout of 0 disables the power policy entirely (screen always on).
    const bool timed_out = config_.display_timeout_ms > 0 && now >= last_activity_ms_ &&
                           (now - last_activity_ms_) >= config_.display_timeout_ms;
    frame.power = view.banner_visible || !timed_out;

    // --- data (D5: every pointer may be null ⇒ field renders unknown) -------
    frame.screen = screens_.screen();
    frame.banner.visible = view.banner_visible;
    frame.banner.level = view.banner_level;
    frame.banner.acknowledged = has_alert && alert.acknowledged;
    if (config_.demo_wake_protocol) {
        // Demo prompts disappear immediately on response/gate closure, unlike
        // the standard min-show band. Preserve the user's underlying screen.
        frame.banner.visible = alert_active;
        frame.banner.fullscreen = alert_active;
        const bw::TimeMs age = now >= alert.t_ms ? now - alert.t_ms : 0;
        frame.banner.ink_visible = (age / kDemoBannerBlinkHalfPeriodMs) % 2 == 0;
    }
    frame.home = readHome(inputs, now);
    frame.diag = readDiag(inputs);
    frame.menu.entry_count = kMenuEntryCount;
    frame.menu.selected = screens_.menuSelection();

    // Backlog 27: the Prompt screen reads F14's provider directly (CR-016).
    // When a provider IS bound, the same read drives the D6 availability gate,
    // so the frame and the NextScreen cycle can never disagree about whether a
    // session is live. With no provider bound the gate is left untouched: F12
    // has no information about F14 and must not override a caller's setting
    // (D5 — null means "source not wired", not "no session").
    frame.prompt = readPrompt(inputs, now);
    if (inputs.prompt != nullptr) {
        screens_.setPromptAvailable(frame.prompt.available);
    }

    return frame;
}

HomeData UiModel::readHome(const UiInputs& inputs, bw::TimeMs now) const {
    HomeData home{};
    bw::TimeMs state_t_ms = 0;

    bw::StateSnapshot snapshot{};
    if (inputs.state != nullptr && inputs.state->get(snapshot)) {
        home.snapshot_valid = true;
        home.alertness = snapshot.alertness;
        home.wear = snapshot.wear;
        state_t_ms = snapshot.t_ms;
    }

    bw::PpgFeatures ppg{};
    if (inputs.ppg != nullptr && inputs.ppg->get(ppg) && ppg.hr_valid && home.snapshot_valid &&
        home.wear == bw::WearState::Worn && ppg.quality != bw::DataQuality::Insufficient) {
        // §8 kHrStaleMs: PpgFeatures are 1 Hz; older than the stale limit
        // shows "--" rather than a stale number. A producer timestamp AHEAD
        // of `now` is NOT stale: the age clamps to 0 (pinned by
        // test_home_screen_forward_timestamp_is_not_stale) — the subtraction
        // below would otherwise underflow the unsigned age to a huge value
        // and wrongly hide a fresh reading.
        const bw::TimeMs age = now >= ppg.t_ms ? now - ppg.t_ms : 0;
        // Reuse the existing HR freshness horizon for the wear snapshot: a
        // stale Worn state must not authorize a live heart-rate readout.
        const bw::TimeMs state_age = now >= state_t_ms ? now - state_t_ms : 0;
        if (age <= config_.hr_stale_ms && state_age <= config_.hr_stale_ms &&
            isRenderableMeasurement(ppg.hr_bpm)) {
            home.hr_valid = true;
            home.hr_bpm = roundToU16(ppg.hr_bpm, 999);
        }
    }

    if (inputs.battery != nullptr) {
        const bw::BatteryReading reading = inputs.battery->read();
        // A-04: valid == false renders "--", never a guessed %; a non-finite
        // estimate is treated as invalid for the same reason (audit F-4).
        if (reading.valid && isRenderableMeasurement(reading.percent_estimate)) {
            home.battery_valid = true;
            home.battery_percent = static_cast<uint8_t>(roundToU16(reading.percent_estimate, 100));
        }
    }
    return home;
}

// Backlog 27 (CR-016): the prompt screen's data. `available` is false — and
// the D6 placeholder renders — when no provider is attached, the provider has
// no state yet, or the manager reports phase Idle (a session ended: withdrawn,
// timed out or answered). The countdown is only computed when a clock exists:
// with no clock `now` is a fabricated 0 (D5), so a "time remaining" would be a
// guess. An expired offer reads 0 s, never a negative or wrapped value.
PromptData UiModel::readPrompt(const UiInputs& inputs, bw::TimeMs now) const {
    PromptData prompt{};
    if (inputs.prompt == nullptr) {
        return prompt;  // no provider attached (W2 default)
    }

    bw::ConfirmPromptState state{};
    if (!inputs.prompt->latest(state)) {
        return prompt;  // no state yet — an honest empty, not a guess
    }

    prompt.phase = state.phase;
    if (state.phase == bw::ConfirmPromptPhase::Idle) {
        return prompt;  // session over: the screen hides again (D6)
    }
    prompt.available = true;
    prompt.seq = state.seq;
    prompt.queue_depth = state.queue_depth;

    // The countdown needs a real clock: the wiring passes now = 0 when none is
    // wired (D5), and 0 is not "the deadline is 8 s away", it is "time is
    // unknown" — computing a remaining from it would fabricate a number (§10).
    if (inputs.clock != nullptr && state.phase == bw::ConfirmPromptPhase::Offered &&
        state.offer_deadline_ms > now) {
        // Integer seconds: the deadline is monotonic ms, and the screen has no
        // sub-second row to spend. Clamped so a distant deadline cannot wrap
        // the subtraction into a huge "remaining".
        const bw::TimeMs remaining = state.offer_deadline_ms - now;
        constexpr bw::TimeMs kMsPerSecond = 1000;
        const bw::TimeMs seconds = remaining / kMsPerSecond;
        prompt.deadline_valid = true;
        prompt.seconds_left = seconds > 0xFFFFu ? 0xFFFFu : static_cast<uint16_t>(seconds);
    }
    return prompt;
}

DiagData UiModel::readDiag(const UiInputs& inputs) const {
    DiagData diag{};
    bw::HealthSnapshot health{};
    if (inputs.health != nullptr && inputs.health->get(health)) {
        diag.available = true;
        diag.imu_quality = health.quality.imu;
        diag.ppg_quality = health.quality.ppg;
        diag.fault_bits = health.fault_bits;
        diag.min_free_heap = health.min_free_heap;
        diag.ble_connected = health.ble_connected;
        diag.degraded_mode = health.degraded_mode;
    }
    return diag;
}

}  // namespace bw::ui
