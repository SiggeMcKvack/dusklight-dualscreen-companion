#include "config.hpp"
#include "hooks.hpp"
#include "slots.hpp"
#include "jni_bridge.hpp"
#include "present.hpp"

#include "dusk/companion.h"
#include "dusk/dualscreen.h"
#include "dusk/game_access.h"
#include "dusk/guide/store.hpp"
#include "dusk/main.h"
#include "dusk/settings.h"
#include "dusk/version.hpp"
#include "aurora/lib/device.hpp"

#include "mods/service.hpp"
#include "mods/svc/config.h"
#include "mods/svc/gfx.h"
#include "mods/svc/hook.h"
#include "mods/svc/host.h"
#include "mods/svc/log.hpp"
#include "mods/svc/ui.h"

#include "d/d_com_inf_game.h"
#include "d/d_meter2_info.h"
#include "d/d_save.h"
#include "dolphin/dvd.h"

#include <cstring>
#include <filesystem>

DEFINE_MOD();
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HostService, svc_host);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(GfxService, svc_gfx);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);

// ---- shims the ported companion code links against ----

namespace dusk {
bool IsGameLaunched = false;

Settings& getSettings() {
    static Settings s;
    return s;
}

namespace version {
bool isRegionPal() {
    const DVDDiskID* id = DVDGetCurrentDiskID();
    return id != nullptr && id->gameName[3] == 'P';
}
}  // namespace version
}  // namespace dusk

namespace aurora::device {
// The fork's haptics are the device vibrator (aurora opens SDL's VIBRATOR_SERVICE haptic), not
// the controller motor; same strength mix as aurora, routed through the Java side.
void rumble(uint16_t lowFreq, uint16_t highFreq, uint16_t durationMs) noexcept {
    const float mixed = static_cast<float>(lowFreq) * 0.6f + static_cast<float>(highFreq) * 0.4f;
    dsc::jni::vibrate(durationMs, mixed / 65535.0f);
}
}  // namespace aurora::device

namespace {

// Surface buffer size for the panel; 0 = the display's own size, so the dashboard's canvas
// (derived from the surface aspect in dualscreen.cpp) follows whatever the second screen is.
// A fixed size here would be stretched by Android onto a screen of another aspect.
constexpr uint32_t kPanelWidth = 0;
constexpr uint32_t kPanelHeight = 0;

GfxStageHookHandle g_beforeHudHook = 0;
GfxStageHookHandle g_afterHudHook = 0;

void on_frame_before_hud(ModContext*, const GfxStageContext*, void*) {
    dusk::dualscreen::beginHudCapture();
}

void on_frame_after_hud(ModContext*, const GfxStageContext*, void*) {
    dusk::dualscreen::endHudCapture();
}

void pump_input() {
    dsc::jni::TouchEvent events[64];
    const size_t n = dsc::jni::drain_touch(events, 64);
    for (size_t i = 0; i < n; ++i) {
        dusk::companion::touchEvent(events[i].action, events[i].u, events[i].v);
    }
    const float pinch = dsc::jni::take_pinch();
    if (pinch != 1.0f) {
        dusk::companion::pinchZoom(pinch);
    }
    dusk::dualscreen::setDisplayAvailable(dsc::jni::display_available());
    dusk::companion::setBatteryStatus(dsc::jni::battery_percent(), dsc::jni::battery_charging());
}

// The fork's duskExecute() additions: requests the touch layer queued during the draw pass are
// consumed here, on the game thread, in the frame loop.
void run_companion_frame() {
    // The game only refreshes the play-side mirror of select items 0/1 (dMeter2_c::_create
    // loops i < 2), so after a save is loaded the slot buttons' entries hold stale raw values and
    // the companion draws the wrong icon for them. Resolve them from the save every frame; the
    // setter is trivial and idempotent.
    for (int i = SELECT_ITEM_DOWN; i < MAX_SELECT_ITEM; i++) {
        dComIfGp_setSelectItem(i);
    }

    dusk::companion::beginFrameCompanionInput();
    dsc::slots::update();

    if (dusk::companion::consumeTransformRequest()) {
        // No dynamic_cast: RTTI does not reliably match across the mod/game boundary.
        if (daAlink_c* link = daAlink_getAlinkActorClass()) {
            ga::cast(link)->tryQuickTransform();
        } else {
            mods::log::warn("transform requested but no Link actor");
        }
    }
    if (dusk::companion::consumeWarpRequest()) {
        constexpr u8 MAP_STATUS_FIELD_MAP_WARP = 3;
        dMeter2Info_setMapStatus(MAP_STATUS_FIELD_MAP_WARP);
    }
    dusk::companion::flushQueuedSounds();
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    GfxStageHookDesc before = GFX_STAGE_HOOK_DESC_INIT;
    before.callback = on_frame_before_hud;
    GfxStageHookDesc after = GFX_STAGE_HOOK_DESC_INIT;
    after.callback = on_frame_after_hud;
    if (svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_FRAME_BEFORE_HUD, &before, &g_beforeHudHook) != MOD_OK ||
        svc_gfx->register_stage_hook(
            mod_ctx, GFX_STAGE_FRAME_AFTER_HUD, &after, &g_afterHudHook) != MOD_OK)
    {
        return mods::set_error(error, MOD_ERROR, "failed to register frame hooks");
    }
    if (!dsc::jni::init(kPanelWidth, kPanelHeight)) {
        return mods::set_error(error, MOD_UNAVAILABLE, "no JNI access on this build");
    }
    if (!dsc::hooks::install()) {
        return mods::set_error(error, MOD_ERROR, "failed to install game hooks");
    }
    if (!dsc::config::init()) {
        return mods::set_error(error, MOD_ERROR, "failed to set up configuration");
    }
    const char* dataDir = nullptr;
    if (svc_host->data_dir(mod_ctx, &dataDir) == MOD_OK && dataDir != nullptr) {
        dusk::guide::set_guides_root(std::filesystem::path(dataDir) / "guides");
        if (!dusk::guide::ensure_dirs()) {
            mods::log::warn("could not create the guide store under {}", dataDir);
        }
    } else {
        mods::log::warn("no data directory; the guide reader is unavailable");
    }
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError*) {
    dusk::IsGameLaunched = true;  // mod_update only runs from the game's frame loop
    dsc::jni::update();

    dsc::jni::SurfaceChange change{};
    if (dsc::jni::take_surface_change(change)) {
        dsc::present::set_native_window(change.window, change.width, change.height);
        dusk::dualscreen::setSurfaceSize(
            change.window != nullptr ? change.width : 0, change.window != nullptr ? change.height : 0);
        dsc::jni::ack_surface_change();
    }
    dsc::present::update();
    pump_input();
    run_companion_frame();
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError*) {
    if (g_beforeHudHook != 0) {
        svc_gfx->unregister_stage_hook(mod_ctx, g_beforeHudHook);
        g_beforeHudHook = 0;
    }
    if (g_afterHudHook != 0) {
        svc_gfx->unregister_stage_hook(mod_ctx, g_afterHudHook);
        g_afterHudHook = 0;
    }
    dsc::slots::shutdown();
    // Drop the swapchain before the Presentation goes away (see ack_surface_change).
    dsc::present::shutdown();
    dusk::dualscreen::shutdown();
    dsc::jni::shutdown();  // dismisses the Presentation synchronously
    return MOD_OK;
}
}
