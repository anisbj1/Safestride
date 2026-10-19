#include <bw/ui/screen_manager.h>

namespace bw::ui {

ScreenManager::ScreenManager(bw::TimeMs alert_min_show_ms)
    : alert_min_show_ms_(alert_min_show_ms) {}

ScreenManager::AlertView ScreenManager::updateAlert(bool alert_active, bw::AlertLevel level,
                                                    bw::TimeMs now) {
    AlertView out{};
    out.banner_visible = banner_visible_;
    out.raised = alert_active && !alert_active_;
    out.banner_level = banner_level_;

    if (alert_active) {
        // D3, applied on every active update: the banner is up and its
        // min-show deadline is (re)armed, so the last push stands when the
        // level returns to Normal — the banner then lingers exactly long
        // enough to avoid flicker, never less.
        banner_level_ = level;
        banner_until_ms_ = now + alert_min_show_ms_;
        banner_visible_ = true;
    } else if (banner_visible_ && now >= banner_until_ms_) {
        // Both conditions of D3 hold: Normal AND the min-show window has
        // passed — the underlying screen reappears.
        banner_visible_ = false;
        banner_level_ = bw::AlertLevel::Normal;
    }
    alert_active_ = alert_active;

    out.banner_visible = banner_visible_;
    out.banner_level = banner_level_;
    return out;
}

void ScreenManager::handleCommand(const bw::Command& cmd) {
    switch (cmd.type) {
    case bw::CommandType::NextScreen:
        if (menu_open_) {
            // While the menu is open the same gesture moves the cursor: the
            // human-rate input path needs one action per press, not two.
            menu_index_ = static_cast<uint8_t>((menu_index_ + 1) % kMenuEntryCount);
        } else {
            advanceCycle();
        }
        break;
    case bw::CommandType::OpenMenu:
        if (!menu_open_) {
            screen_before_menu_ = screen_;
            menu_open_ = true;
            menu_index_ = 0;
        }
        break;
    default:
        // Other command types belong to other features (F10's pump converts
        // them); F12 routes only the two UI commands (D4).
        break;
    }
}

void ScreenManager::advanceCycle() {
    // Fixed order Home -> Prompt -> Diagnostics -> Home; Prompt is skipped
    // while its provider is unavailable (D6), so W2's cycle is Home <-> Diag.
    const ScreenId order[] = {ScreenId::Home, ScreenId::Prompt, ScreenId::Diagnostics};
    for (size_t i = 0; i < 3; ++i) {
        if (order[i] != screen_) {
            continue;
        }
        for (size_t step = 1; step <= 3; ++step) {
            const ScreenId candidate = order[(i + step) % 3];
            if (candidate == ScreenId::Prompt && !prompt_available_) {
                continue;
            }
            screen_ = candidate;
            return;
        }
        return;  // unreachable: at least one non-Prompt screen always exists
    }
    // Current screen is not in the cycle (Menu handled by caller, Prompt can
    // only get here via direct activation) — restart the cycle at Home.
    screen_ = ScreenId::Home;
}

void ScreenManager::activateScreen(ScreenId id) {
    menu_open_ = false;
    screen_ = id;
}

void ScreenManager::setPromptAvailable(bool available) {
    prompt_available_ = available;
}

void ScreenManager::menuMoveSelection() {
    menu_index_ = static_cast<uint8_t>((menu_index_ + 1) % kMenuEntryCount);
}

void ScreenManager::menuBack() {
    if (menu_open_) {
        menu_open_ = false;
        screen_ = screen_before_menu_;
    }
}

void ScreenManager::menuSelect() {
    if (!menu_open_) {
        return;
    }
    menu_open_ = false;
    screen_ = screen_before_menu_;
    switch (menu_index_) {
    case 0:
        screen_ = ScreenId::Diagnostics;
        pending_action_ = MenuAction::OpenDiagnostics;
        break;
    case 1:
        screen_ = ScreenId::Home;
        pending_action_ = MenuAction::GoHome;
        break;
    default:
        // "Ack alerts": stages the semantic action only (§13 — no Command is
        // dispatched in W2; the wiring converts it when F10's pump exists).
        pending_action_ = MenuAction::AcknowledgeAlert;
        break;
    }
}

MenuAction ScreenManager::takeAction() {
    const MenuAction out = pending_action_;
    pending_action_ = MenuAction::None;
    return out;
}

}  // namespace bw::ui
