#pragma once

// bw/ui/ui_model.h — F12 D1/D5/D8: the pure UI state machine and the frame
// contract between model and renderers.
//
// UiModel::step reads the bw_core data sources, owns the power/timeout policy
// (D8) and delegates screen/banner/menu bookkeeping to ScreenManager (D3/D6).
// It is deterministic, host-testable and platform-free: time arrives as a
// TimeMs argument (the wiring reads IClock, 01 §1 rule 5).

#include <cstdint>

#include <bw/alerts.h>
#include <bw/clock.h>
#include <bw/commands.h>
#include <bw/containers/latest_value.h>
#include <bw/context.h>
#include <bw/features.h>
#include <bw/hardware.h>
#include <bw/providers.h>
#include <bw/quality.h>
#include <bw/state.h>
#include <bw/types.h>

#include <bw/ui/screen_manager.h>
#include <bw/ui/ui_config.h>

namespace bw::ui {

// UiInputs — D5: a plain struct of bw_core pointers, every one nullable.
// mailboxes are bw_core (host-safe), so host tests fill them with real
// LatestValue instances — no fakes needed. Null means "source not wired
// yet": the model renders that field as unknown, never fabricates (§10).
struct UiInputs {
    const bw::LatestValue<bw::StateSnapshot>* state = nullptr;
    const bw::LatestValue<bw::AlertStatus>* alert = nullptr;
    const bw::LatestValue<bw::PpgFeatures>* ppg = nullptr;
    // F23 lands this in W2: nullptr until then, the Diagnostics screen shows
    // "n/a" lines (tested).
    const bw::LatestValue<bw::HealthSnapshot>* health = nullptr;
    bw::IBatteryMonitor* battery = nullptr;  // read() is non-const (02 §3)
    bw::IClock* clock = nullptr;             // nullptr: timeouts disabled (D5)
    // CR-016 / F14 (backlog 27): the prompt provider behind the Prompt screen.
    // nullptr = no provider attached = the D6 placeholder, exactly as W2.
    const bw::IConfirmPromptProvider* prompt = nullptr;
};

// Alert banner band content (D3 overlay, drawn over the active screen).
struct BannerState {
    bool visible;
    bw::AlertLevel level;  // last non-Normal level while visible
    bool acknowledged;
    bool fullscreen = false;
    bool ink_visible = true;
};

// Home screen fields. `*_valid == false` renders "--" / "No data" (§10).
// (No confidence field on purpose: the Home screen renders no confidence
// today, and §11 keeps the frame small — audit F-3 dropped the dead payload.)
struct HomeData {
    bool snapshot_valid;  // a StateSnapshot was ever published
    bw::AlertnessState alertness;
    bw::WearState wear;
    bool hr_valid;    // fresh AND hr_valid per kHrStaleMs (§8)
    uint16_t hr_bpm;  // rounded; valid only when hr_valid
    bool battery_valid;
    uint8_t battery_percent;  // rounded; valid only when battery_valid
};

// Pin (audit F-3): HomeData carries only render-consumed fields — the frame
// is "small on purpose" (§11). A new field must come with a renderer and a
// spec section, not just payload; re-adding the dead `confidence` float
// makes sizeof 12 and fails this build. (The 2026-10-15 `hr_uncertain`
// field was removed the same day it was added — developer reversed the
// uncertainty marker; the pin did its job.)
static_assert(sizeof(HomeData) == 8, "HomeData must stay render-consumed-only (audit F-3)");

// Diagnostics screen fields; health == nullptr ⇒ available == false ⇒ "n/a".
struct DiagData {
    bool available;
    bw::DataQuality imu_quality;
    bw::DataQuality ppg_quality;
    uint32_t fault_bits;
    uint32_t min_free_heap;
    bool ble_connected;
    bool degraded_mode;
};

struct MenuData {
    uint8_t entry_count;  // == kMenuEntryCount; kept for future tables
    uint8_t selected;
};

// Prompt screen fields (CR-016 / F14). `available == false` renders the D6
// placeholder ("No prompts"): no provider attached, provider has no state yet,
// or the manager reports phase Idle (a session ended — withdrawn, timed out or
// answered). Never fabricated: an unknown deadline renders "--" (§10).
struct PromptData {
    bool available;
    bw::ConfirmPromptPhase phase;
    uint32_t seq;           // event currently offered; 0 = none
    uint8_t queue_depth;    // pending events behind the offered one
    bool deadline_valid;    // Offered AND a clock is wired (else "--")
    uint16_t seconds_left;  // remaining reply time; valid only when deadline_valid
};

// One rendered frame: everything a screen needs, computed in one place (D1).
// Small on purpose (§11 budget: UiModel frame + ScreenManager < 2 KB).
struct UiFrame {
    bool power;  // false ⇒ renderers draw and flush nothing (D8)
    ScreenId screen;
    BannerState banner;
    HomeData home;
    DiagData diag;
    MenuData menu;
    PromptData prompt;
};

class UiModel {
public:
    explicit UiModel(const UiConfig& config = defaultUiConfig());

    // Feeds one routed UI command (D4: from wire_ui's own queue); any command
    // resets the inactivity timer (D8).
    void handleCommand(const bw::Command& cmd, bw::TimeMs now);

    // Produces the next frame. `now` is monotonic ms; 0 (no clock wired, D5)
    // disables timeouts and staleness because no age can grow.
    UiFrame step(const UiInputs& inputs, bw::TimeMs now);

    // Staged menu semantic action (§13 port), drained by the wiring.
    MenuAction takeAction() { return screens_.takeAction(); }

    ScreenManager& screens() { return screens_; }
    const UiConfig& config() const { return config_; }

private:
    HomeData readHome(const UiInputs& inputs, bw::TimeMs now) const;
    DiagData readDiag(const UiInputs& inputs) const;
    PromptData readPrompt(const UiInputs& inputs, bw::TimeMs now) const;

    UiConfig config_;
    ScreenManager screens_;
    bw::TimeMs last_activity_ms_ = 0;  // D8: timeout origin (command/alert wake)
};

}  // namespace bw::ui
