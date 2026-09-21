#include "config.hpp"

#include "dusk/settings.h"
#include "dusk/guide/browser.hpp"

#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/log.hpp"
#include "mods/svc/ui.h"

namespace dsc::config {
namespace {

ConfigVarHandle g_hudMode = 0;
ConfigVarHandle g_haptics = 0;
ConfigVarHandle g_fpsOnCompanion = 0;
ConfigVarHandle g_guideEnabled = 0;

const char* const kHudModes[] = {"Wii U style (HUD on bottom)", "3DS style (hearts + A/B on top)"};

// Pull every var into the in-memory settings the ported companion code reads.
void sync_from_config() {
    auto& s = dusk::getSettings();
    bool b = false;
    int64_t i = 0;
    if (svc_config->get_int(mod_ctx, g_hudMode, &i) == MOD_OK) {
        s.game.dualScreenHudMode.setValue(i == 0 ? kDualHudCinematic : kDualHudFunctional);
    }
    if (svc_config->get_bool(mod_ctx, g_haptics, &b) == MOD_OK) {
        s.game.dualScreenHaptics.setValue(b);
    }
    if (svc_config->get_bool(mod_ctx, g_guideEnabled, &b) == MOD_OK) {
        s.game.guideEnabled.setValue(b);
    }
    if (svc_config->get_bool(mod_ctx, g_fpsOnCompanion, &b) == MOD_OK) {
        // The host's overlay never knows corner 4; the companion just reads it.
        s.video.enableFpsOverlay.setValue(b);
        s.video.fpsOverlayCorner.setValue(b ? kFpsCornerCompanion : 0);
    }
}

void on_changed(ModContext*, ConfigVarHandle, const ConfigVarValue*, const ConfigVarValue*, void*) {
    sync_from_config();
}

bool register_bool(const char* name, bool def, ConfigVarHandle& out) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_BOOL;
    desc.default_bool = def;
    return svc_config->register_var(mod_ctx, &desc, &out) == MOD_OK &&
           svc_config->subscribe(mod_ctx, out, on_changed, nullptr, nullptr) == MOD_OK;
}

bool register_int(const char* name, int64_t def, ConfigVarHandle& out) {
    ConfigVarDesc desc = CONFIG_VAR_DESC_INIT;
    desc.name = name;
    desc.type = CONFIG_VAR_INT;
    desc.default_int = def;
    return svc_config->register_var(mod_ctx, &desc, &out) == MOD_OK &&
           svc_config->subscribe(mod_ctx, out, on_changed, nullptr, nullptr) == MOD_OK;
}

ModResult add_bound(UiElementHandle panel, UiControlKind kind, const char* label, const char* help,
    ConfigVarHandle var) {
    UiControlDesc control = UI_CONTROL_DESC_INIT;
    control.kind = kind;
    control.label = label;
    control.help_rml = help;
    control.binding = UI_BINDING_CONFIG_VAR;
    control.config_var = var;
    if (kind == UI_CONTROL_DROPDOWN) {
        control.options = kHudModes;
        control.option_count = 2;
    }
    return svc_ui->pane_add_control(mod_ctx, panel, &control, nullptr);
}

void on_open_browser(ModContext*, void*) {
    if (!dusk::guide::open_browser(dusk::guide::kDefaultGuideUrl)) {
        mods::log::warn("guide browser could not be opened");
    }
}

bool browser_disabled(ModContext*, void*) {
    return !dusk::getSettings().game.guideEnabled.getValue() || !dusk::guide::browser_available();
}

ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    ModResult r = add_bound(panel, UI_CONTROL_DROPDOWN, "Layout",
        "Wii U style moves the whole HUD to the bottom screen. 3DS style keeps hearts and the "
        "A/B buttons on the main screen and turns the bottom screen into a control surface.",
        g_hudMode);
    if (r != MOD_OK) {
        return r;
    }
    r = add_bound(panel, UI_CONTROL_TOGGLE, "Haptic feedback",
        "Rumble on bottom-screen button presses.", g_haptics);
    if (r != MOD_OK) {
        return r;
    }
    r = add_bound(panel, UI_CONTROL_TOGGLE, "FPS counter on bottom screen",
        "Draw the frame-rate counter on the companion instead of the main screen.",
        g_fpsOnCompanion);
    if (r != MOD_OK) {
        return r;
    }
    r = add_bound(panel, UI_CONTROL_TOGGLE, "Walkthrough guide",
        "Offline walkthrough reader on the bottom screen. Save pages with the in-app browser "
        "below, or put browser-saved .html pages in the guides/import folder of the mod's data "
        "directory.",
        g_guideEnabled);
    if (r != MOD_OK) {
        return r;
    }
    UiControlDesc button = UI_CONTROL_DESC_INIT;
    button.kind = UI_CONTROL_BUTTON;
    button.label = "Get a guide (in-app browser)";
    button.help_rml = "Opens zeldadungeon.net in an overlay on the main screen; the game keeps "
                      "running. Save a walkthrough page and its chapters are imported for the reader.";
    button.on_pressed = on_open_browser;
    button.is_disabled = browser_disabled;
    return svc_ui->pane_add_control(mod_ctx, panel, &button, nullptr);
}

}  // namespace

bool init() {
    if (!register_int("hud_mode", 1, g_hudMode) || !register_bool("haptics", true, g_haptics) ||
        !register_bool("fps_on_companion", false, g_fpsOnCompanion) ||
        !register_bool("guide_enabled", true, g_guideEnabled))
    {
        mods::log::error("failed to register config vars");
        return false;
    }
    sync_from_config();

    UiModsPanelDesc panelDesc = UI_MODS_PANEL_DESC_INIT;
    panelDesc.build = build_panel;
    if (svc_ui->register_mods_panel(mod_ctx, &panelDesc) != MOD_OK) {
        mods::log::error("failed to register the mod panel");
        return false;
    }
    return true;
}

}  // namespace dsc::config
