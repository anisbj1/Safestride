#pragma once

// bw/ui/screen_manager.h — F12 D3/D6: the screen state machine.
//
// Pure logic (01 §1 rule 5): no canvas, no platform calls, time arrives as a
// TimeMs argument. ScreenManager owns WHICH screen is active, the alert-banner
// overlay bookkeeping (D3: the banner is an overlay, not a screen — the user's
// place survives it) and the tiny menu's semantic actions (D6/§13: menu
// selections dispatch no Command in W2; MenuAction is the tested port).

#include <cstdint>

#include <bw/alerts.h>
#include <bw/commands.h>
#include <bw/types.h>

#include <bw/ui/ui_config.h>

namespace bw::ui {

// Which screen the body of the display shows. The alert banner is NOT here on
// purpose (D3): it overlays whatever screen is active.
enum class ScreenId : uint8_t { Home, Prompt, Diagnostics, Menu };

// Semantic menu selection (§13): the wiring converts this to a Command only
// when a consumer exists; in W2 nothing routes it and that is a tested state.
enum class MenuAction : uint8_t { None, OpenDiagnostics, GoHome, AcknowledgeAlert };

// Single source of truth for the menu: ScreenManager (index -> action) and the
// renderer (index -> name) must not drift apart.
constexpr uint8_t kMenuEntryCount = 3;
inline constexpr const char* kMenuEntryNames[kMenuEntryCount] = {"Diagnostics", "Home",
                                                                 "Ack alerts"};

class ScreenManager {
public:
    // The min-show knob defaults to the spec §8 value; UiModel forwards its
    // UiConfig so host tests can shorten the timeline.
    explicit ScreenManager(bw::TimeMs alert_min_show_ms = kAlertScreenMinShowMs);

    // Result of one alert-status update (D3 bookkeeping).
    struct AlertView {
        bool banner_visible;          // draw the banner band this frame
        bool raised;                  // Warning+ edge: a NEW banner just appeared (wake)
        bw::AlertLevel banner_level;  // last non-Normal level, while visible
    };

    // Feed the current alert state. `alert_active` = (AlertStatus.level >=
    // Warning). A raise sets the min-show deadline; after the level returns to
    // Normal the banner stays visible until that deadline passes (no flicker).
    AlertView updateAlert(bool alert_active, bw::AlertLevel level, bw::TimeMs now);

    // Routes one Command. NextScreen cycles Home <-> Diagnostics, skipping
    // Prompt while prompts are unavailable (D6); while the menu is open
    // NextScreen moves its cursor instead. OpenMenu opens the menu over the
    // current screen. Other command types are ignored (F10's pump owns them).
    void handleCommand(const bw::Command& cmd);

    // Direct activation (tests + future wiring): sets the screen and closes
    // the menu. Activating Prompt while it is unavailable is legal and renders
    // the D6 placeholder.
    void activateScreen(ScreenId id);

    // D6 integration point: flipping this re-enters Prompt into the NextScreen
    // cycle without any other change.
    void setPromptAvailable(bool available);

    // Menu navigation. moveSelection wraps the cursor (human-rate input, a
    // lost press is forgiving); back() closes the menu and restores the screen
    // it was opened from; select() applies the highlighted entry, closes the
    // menu and (for "Ack alerts") stages a MenuAction.
    void menuMoveSelection();
    void menuBack();
    void menuSelect();

    // Drains the staged MenuAction (None when nothing is pending).
    MenuAction takeAction();

    ScreenId screen() const { return menu_open_ ? ScreenId::Menu : screen_; }
    bool menuOpen() const { return menu_open_; }
    uint8_t menuSelection() const { return menu_index_; }
    bool promptAvailable() const { return prompt_available_; }

private:
    // Advances one step in the fixed order Home -> Prompt -> Diagnostics ->
    // Home, skipping Prompt while it is unavailable (D6).
    void advanceCycle();

    ScreenId screen_ = ScreenId::Home;  // active non-menu screen
    ScreenId screen_before_menu_ = ScreenId::Home;
    bool menu_open_ = false;
    uint8_t menu_index_ = 0;
    bool prompt_available_ = false;  // F14 provider is W3 (D6)

    bool banner_visible_ = false;
    bool alert_active_ = false;       // previous tick's >= Warning state
    bw::TimeMs alert_min_show_ms_;    // D3 anti-flicker window (§8)
    bw::TimeMs banner_until_ms_ = 0;  // min-show deadline (D3)
    bw::AlertLevel banner_level_ = bw::AlertLevel::Normal;

    MenuAction pending_action_ = MenuAction::None;
};

}  // namespace bw::ui
