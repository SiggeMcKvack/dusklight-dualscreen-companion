// Game-side hooks standing in for the fork's edits to game sources (see the plan's hook table).
// Everything here runs on the game thread inside the hooked call.
#include "dusk/companion.h"
#include "dusk/dualscreen.h"
#include "dusk/game_access.h"
#include "dusk/logging.h"
#include "slots.hpp"

#include "mods/service.hpp"
#include "mods/svc/hook.hpp"

#include "d/d_com_inf_game.h"
#include "d/d_meter2.h"
#include "d/d_meter2_draw.h"
#include "d/d_meter2_info.h"
#include "d/d_map.h"
#include "d/d_map_path.h"
#include "d/d_meter_map.h"
#include "d/d_kantera_icon_meter.h"
#include "d/d_pane_class.h"
#include "JSystem/J2DGraph/J2DPicture.h"
#include "d/actor/d_a_alink.h"
#include "d/actor/d_a_player.h"
#include "d/d_meter_HIO.h"
#include "d/d_msg_object.h"
#include "d/d_save.h"
#include "f_op/f_op_scene_mng.h"
#include "m_Do/m_Do_controller_pad.h"

#include <aurora/math.hpp>

#include <thread>

// Defined in d_map_path.cpp with no header; exported by the game.
aurora::Vec2<u16> map_render_size_for(u16 width, u16 height);

DEFINE_HOOK(&dMeter2Draw_c::draw, MeterDrawDraw);
DEFINE_HOOK(&dMeter2Draw_c::setButtonIconAlpha, MeterDrawIconAlpha);
DEFINE_HOOK(static_cast<void (dMeter2Draw_c::*)(CPaneMgr*, f32*, f32, JUtility::TColor,
                JUtility::TColor, JUtility::TColor, JUtility::TColor, f32, u8)>(
                &dMeter2Draw_c::drawPikari),
    MeterDrawPikariPane);
DEFINE_HOOK(&dDlst_KanteraIcon_c::draw, KanteraIconDraw);
DEFINE_HOOK(&CPaneMgrAlpha::show, PaneMgrShow);
DEFINE_HOOK(static_cast<void (J2DPicture::*)(f32, f32, f32, f32, bool, bool, bool)>(&J2DPicture::draw),
    PictureDrawRect);
DEFINE_HOOK(&dMeter2_c::_create, Meter2Create);
DEFINE_HOOK(&dMeter2_c::_delete, Meter2Delete);
DEFINE_HOOK(&dMeter2_c::_draw, Meter2Draw);
DEFINE_HOOK(&dMeter2Draw_c::presentButtonA, MeterDrawPresentA);
DEFINE_HOOK(&dMeter2Draw_c::presentButtonB, MeterDrawPresentB);
DEFINE_HOOK(&dMeterMap_c::_draw, MeterMapDraw);
DEFINE_HOOK(&dMap_c::_draw, MapDraw);
DEFINE_HOOK(&renderingAmap_c::getPlayerCursorSize, AmapPlayerCursorSize);
DEFINE_HOOK(&renderingAmap_c::getRestartCursorSize, AmapRestartCursorSize);
DEFINE_HOOK(&map_render_size_for, MapRenderSizeFor);
DEFINE_HOOK(&mDoCPd_c::read, PadRead);
DEFINE_HOOK(&dComIfGp_setSelectItem, SetSelectItem);
DEFINE_HOOK(&dMsgObject_c::setSmellTypeLocal, SetSmellType);
DEFINE_HOOK(&fopScnM_ChangeReq, SceneChangeReq);
DEFINE_HOOK(&daAlink_c::execute, LinkExecute);

namespace dsc::hooks {
namespace {

using namespace dusk;

// Set while dMeter2Draw_c::draw (the MAIN screen's HUD draw) is running, so the finer-grained
// hooks below know whose draw they are in; the companion's own pass runs later.
bool s_inMainDraw = false;
dMeter2Draw_c* s_mainDrawMd = nullptr;
bool s_kanteraScreenHidden = false;

// Fork dMeter2Draw_c::dualScreenSyncPaneVisibility. Partition the HUD between the screens:
//   Cinematic  - every status readout and the whole button cluster is on the companion;
//                contextual bottom prompts stay on the main view.
//   Functional - hearts, vessel, the A/B/Z cluster and the d-pad move back to the main
//                screen; only the rupee/key readouts and the X/Y item buttons stay on the
//                companion.
// The host's draw() re-shows mpButtonParent every frame (touch-controls logic) right after this
// pre-hook; on_pane_show_pre blocks that one call while the cluster belongs to the companion.
void syncPaneVisibility(dMeter2Draw_c* md, bool dualScreenHud, bool mainHudRestored,
    bool lowLifeHearts) {
    enum { STATE_MAIN, STATE_CINEMATIC, STATE_FUNCTIONAL };
    const int state =
        !dualScreenHud ? STATE_MAIN : (mainHudRestored ? STATE_FUNCTIONAL : STATE_CINEMATIC);
    static int sPrevState = STATE_MAIN;
    if (state == STATE_MAIN && sPrevState == STATE_MAIN) {
        return;
    }

    CPaneMgr* companionPanes[] = {md->mpMagicParent, md->mpRupeeKeyParent, md->mpButtonXY[0],
        md->mpButtonXY[1], md->mpItemXY[0], md->mpItemXY[1], md->mpButtonCrossParent};
    for (CPaneMgr* pane : companionPanes) {
        if (pane != NULL) {
            if (state == STATE_MAIN) {
                pane->show();
            } else {
                pane->hide();
            }
        }
    }

    // Cinematic-only: Functional moves these back to the main screen. The hearts alone come
    // back even in Cinematic while the low-health pop-in holds.
    CPaneMgr* cinematicOnlyPanes[] = {md->mpLifeParent, md->mpLightDropParent, md->mpButtonParent};
    for (CPaneMgr* pane : cinematicOnlyPanes) {
        if (pane != NULL) {
            const bool show =
                state != STATE_CINEMATIC || (pane == md->mpLifeParent && lowLifeHearts);
            if (show) {
                pane->show();
            } else {
                pane->hide();
            }
        }
    }

    // Functional-only: the Z/Midna prompt and the Z glyph are children of the button cluster,
    // which Functional keeps on the main screen for A/B — but the companion's Z corner already
    // shows them, so the main-screen copies hide rather than duplicating. The game drives both
    // via alpha only (never visibility), so an unconditional show() on the way out cannot
    // resurrect a stale state.
    CPaneMgr* zPanes[] = {md->mpButtonMidona, md->mpButtonXY[2]};
    for (CPaneMgr* pane : zPanes) {
        if (pane != NULL) {
            if (state == STATE_FUNCTIONAL) {
                pane->hide();
            } else {
                pane->show();
            }
        }
    }

    // Functional-only: the X/Y action words ("Sense"/"Dig"). Cinematic hides the buttons
    // themselves; Functional keeps the cluster visible for A/B/Z, so these must be hidden by
    // hand. Never shown here on the way out (drawButtonXY re-asserts them on a status change).
    if (state == STATE_FUNCTIONAL) {
        CPaneMgr* functionalOnlyPanes[] = {md->mpTextXY[0], md->mpTextXY[1]};
        for (CPaneMgr* pane : functionalOnlyPanes) {
            if (pane != NULL) {
                pane->hide();
            }
        }
    }

    sPrevState = state;
}

bool idleCluster(dMeter2Draw_c* md) {
    return !dComIfGp_event_runCheck() && !md->getCameraSubject() && !md->getItemSubject() &&
           !md->getPlayerSubject();
}

// Functional: carry the uzu swirl ornament and the Vessel of Light up-right with the relocated
// A/B pair (fork draw() prologue). Idempotent re-apply / restore.
void carryOrnaments(dMeter2Draw_c* md, bool mainHudRestored) {
    const bool carry = mainHudRestored && idleCluster(md);
    if (md->mpUzu != NULL && md->mpUzu->getPanePtr() != NULL) {
        static f32 sUzuSetX = -99999.0f;
        static f32 sUzuSetY = 0.0f;
        J2DPane* uzuPane = md->mpUzu->getPanePtr();
        const JGeometry::TBox2<f32>& ub = uzuPane->getBounds();
        if (carry) {
            if (ub.i.x != sUzuSetX || ub.i.y != sUzuSetY) {
                uzuPane->move(ub.i.x + 42.0f, ub.i.y - 50.0f);
                const JGeometry::TBox2<f32>& nb = uzuPane->getBounds();
                sUzuSetX = nb.i.x;
                sUzuSetY = nb.i.y;
            }
        } else {
            if (ub.i.x == sUzuSetX && ub.i.y == sUzuSetY && sUzuSetX > -99998.0f) {
                uzuPane->move(ub.i.x - 42.0f, ub.i.y + 50.0f);
            }
            sUzuSetX = -99999.0f;
        }
    }
    if (md->mpLightDropParent != NULL && md->mpLightDropParent->getPanePtr() != NULL) {
        static f32 sVesselSetX = -99999.0f;
        static f32 sVesselSetY = 0.0f;
        J2DPane* vesselPane = md->mpLightDropParent->getPanePtr();
        const JGeometry::TBox2<f32>& vb = vesselPane->getBounds();
        if (carry) {
            if (vb.i.x != sVesselSetX || vb.i.y != sVesselSetY) {
                vesselPane->move(vb.i.x + 42.0f, vb.i.y - 50.0f);
                const JGeometry::TBox2<f32>& nv = vesselPane->getBounds();
                sVesselSetX = nv.i.x;
                sVesselSetY = nv.i.y;
            }
        } else {
            if (vb.i.x == sVesselSetX && vb.i.y == sVesselSetY && sVesselSetX > -99998.0f) {
                vesselPane->move(vb.i.x - 42.0f, vb.i.y + 50.0f);
            }
            sVesselSetX = -99999.0f;
        }
    }
}

// ---- dMeter2Draw_c::draw ----

HookAction on_meter_draw_pre(ModContext*, void* args, void*, void*) {
    auto* md = ::mods::arg<dMeter2Draw_c*>(args, 0);
    const bool dualScreenHud = dualscreen::hudOnCompanion();
    const bool mainHudRestored = dualscreen::mainHudRestored();
    syncPaneVisibility(md, dualScreenHud, mainHudRestored, dualscreen::lowLifePopIn());
    carryOrnaments(md, mainHudRestored);
    s_inMainDraw = true;
    s_mainDrawMd = md;
    if (!dualScreenHud) {
        return HOOK_CONTINUE;
    }
    // The parts of draw() that draw explicitly (not through pane visibility) and belong to the
    // companion in both modes: the X/Y item count digits and the lantern oil gauges are skipped
    // by the J2DPicture::draw / dDlst_KanteraIcon_c::draw hooks below while s_inMainDraw; the
    // kantera/oxygen screen is hidden for the call (drawKanteraScreen's final draw).
    if (md->mpKanteraScreen != NULL && md->mpKanteraScreen->isVisible()) {
        md->mpKanteraScreen->hide();
        s_kanteraScreenHidden = true;
    }
    return HOOK_CONTINUE;
}

void on_meter_draw_post(ModContext*, void* args, void*, void*) {
    auto* md = ::mods::arg<dMeter2Draw_c*>(args, 0);
    s_inMainDraw = false;
    s_mainDrawMd = nullptr;
    if (s_kanteraScreenHidden) {
        if (md->mpKanteraScreen != NULL) {
            md->mpKanteraScreen->show();
        }
        s_kanteraScreenHidden = false;
    }
}

// draw() re-shows mpButtonParent every frame for the touch-controls toggle; in Cinematic the
// partition hid it (the whole cluster lives on the companion), so that show() must not land.
HookAction on_pane_show_pre(ModContext*, void* args, void*, void*) {
    if (!s_inMainDraw || s_mainDrawMd == nullptr) {
        return HOOK_CONTINUE;
    }
    auto* pane = ::mods::arg<CPaneMgrAlpha*>(args, 0);
    if (pane == s_mainDrawMd->mpButtonParent && dualscreen::hudOnCompanion() &&
        !dualscreen::mainHudRestored())
    {
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

// Lantern oil gauges: companion-only in both modes.
HookAction on_kantera_icon_draw_pre(ModContext*, void*, void*, void*) {
    if (s_inMainDraw && dualscreen::hudOnCompanion()) {
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

// X/Y ammo digits (mpItemNumTex) are drawn explicitly by draw(); companion-only in both modes.
HookAction on_picture_draw_pre(ModContext*, void* args, void*, void*) {
    if (!s_inMainDraw || s_mainDrawMd == nullptr || !dualscreen::hudOnCompanion()) {
        return HOOK_CONTINUE;
    }
    auto* pic = ::mods::arg<J2DPicture*>(args, 0);
    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 3; j++) {
            if (pic == s_mainDrawMd->mpItemNumTex[i][j]) {
                return HOOK_SKIP_ORIGINAL;
            }
        }
    }
    return HOOK_CONTINUE;
}

// Button pulses (A/B/XY/Midna) and the vessel tear glow are direct draws at the panes'
// main-screen positions. Cinematic moves them all to the companion; the X/Y pulses stay
// companion-only in both modes since the X/Y buttons never come back.
HookAction on_pikari_pre(ModContext*, void* args, void*, void*) {
    if (!s_inMainDraw) {
        return HOOK_CONTINUE;
    }
    auto* md = ::mods::arg<dMeter2Draw_c*>(args, 0);
    auto* pane = ::mods::arg<CPaneMgr*>(args, 1);
    if (!dualscreen::mainHudActive()) {
        return HOOK_SKIP_ORIGINAL;
    }
    if (dualscreen::hudOnCompanion() && (pane == md->mpBTextXY[0] || pane == md->mpBTextXY[1])) {
        return HOOK_SKIP_ORIGINAL;
    }
    return HOOK_CONTINUE;
}

// Functional keeps only A and B on the main screen: shift the pair as a group up into the
// top-right corner (same delta for both, preserving their vanilla diagonal). Idle-cluster
// state only. This Dusklight version applies the positions every presentation frame through
// presentButtonA/B (drawButtonA/B no longer position anything on PC), so the shift goes on the
// present args: (posX, posY, textPosX, textPosY, scale).
bool a_aiming() {
    daPy_py_c* player = daPy_getPlayerActorClass();
    dMeter2_c* meter = dMeter2Info_getMeterClass();
    return (meter != NULL && (meter->mStatus & 0x100)) ||
           (player != NULL && (player->checkHawkWait() || player->checkGrassWhistle()));
}

HookAction on_present_a_pre(ModContext*, void* args, void*, void*) {
    auto* md = ::mods::arg<dMeter2Draw_c*>(args, 0);
    if (dualscreen::mainHudRestored() && idleCluster(md) && !a_aiming()) {
        ::mods::arg_ref<f32>(args, 1) += 43.0f;
        ::mods::arg_ref<f32>(args, 2) -= 48.0f;
        ::mods::arg_ref<f32>(args, 3) += 43.0f;
        ::mods::arg_ref<f32>(args, 4) -= 48.0f;
    }
    return HOOK_CONTINUE;
}

HookAction on_present_b_pre(ModContext*, void* args, void*, void*) {
    auto* md = ::mods::arg<dMeter2Draw_c*>(args, 0);
    if (dualscreen::mainHudRestored() && idleCluster(md)) {
        ::mods::arg_ref<f32>(args, 1) += 43.0f;
        ::mods::arg_ref<f32>(args, 2) -= 48.0f;
        ::mods::arg_ref<f32>(args, 3) += 43.0f;
        ::mods::arg_ref<f32>(args, 4) -= 48.0f;
    }
    return HOOK_CONTINUE;
}

// dMeter2Info's use-button bits are only truthful between Link's execute (which sets them) and
// the reset at the end of the meter's own execute — the companion's draw pass runs after that
// reset. setButtonIconAlpha runs every frame inside the truthful window: snapshot here.
HookAction on_icon_alpha_pre(ModContext*, void* args, void*, void*) {
    if (::mods::arg<int>(args, 1) == 0) {
        dMeter2DrawAccess::sItemUsable[0] = dMeter2Info_isUseButton(METER2_USEBUTTON_X);
        dMeter2DrawAccess::sItemUsable[1] = dMeter2Info_isUseButton(METER2_USEBUTTON_X << 1);
        dMeter2DrawAccess::sItemUsable[2] = dMeter2DrawAccess::sItemUsable[0];
        dMeter2DrawAccess::sItemUsable[3] = dMeter2DrawAccess::sItemUsable[1];
    }
    return HOOK_CONTINUE;
}

// ---- dMeter2_c ----

// The companion reads the meter global every frame; stale pointers crashed on stage
// transitions in the fork before these two.
HookAction on_meter2_create_pre(ModContext*, void* args, void*, void*) {
    ::mods::arg<dMeter2_c*>(args, 0)->mpMeterDraw = NULL;
    return HOOK_CONTINUE;
}

void on_meter2_delete_post(ModContext*, void*, void*, void*) {
    dMeter2Info_setMeterMapClass(NULL);
    dMeter2Info_setMeterClass(NULL);
}

dMeterSub_c* s_savedSub = nullptr;
dMeterString_c* s_savedSubSub = nullptr;
bool s_subParked = false;

// The contextual control panel (grass/sumo/fishing controls) renders on the companion instead
// — except the horse spur/stamina meter (1), crawling arrows (2) and the scope overlay (4),
// which are gameplay-critical and stay on the main view. Functional keeps every panel on main.
// Parked for the call so the host's own early-outs (minimal HUD etc.) still apply.
HookAction on_meter2_draw_pre(ModContext*, void* args, void*, void*) {
    auto* meter = ::mods::arg<dMeter2_c*>(args, 0);
    companion::dmapUpdate();
    const bool subContentsOnMain = dualscreen::mainHudActive() || meter->mSubContentType == 1 ||
                                   meter->mSubContentType == 2 || meter->mSubContentType == 4;
    if (!subContentsOnMain) {
        s_savedSub = meter->mpSubContents;
        s_savedSubSub = meter->mpSubSubContents;
        meter->mpSubContents = NULL;
        meter->mpSubSubContents = NULL;
        s_subParked = true;
    }
    return HOOK_CONTINUE;
}

void on_meter2_draw_post(ModContext*, void* args, void*, void*) {
    auto* meter = ::mods::arg<dMeter2_c*>(args, 0);
    if (s_subParked) {
        meter->mpSubContents = s_savedSub;
        meter->mpSubSubContents = s_savedSubSub;
        s_subParked = false;
    }
}

// The minimap lives on the second screen: keep the map render-texture fresh (dMap_c::_draw)
// but skip compositing it on the main view.
HookAction on_meter_map_draw_pre(ModContext*, void* args, void*, void*) {
    if (!dualscreen::hudOnCompanion()) {
        return HOOK_CONTINUE;
    }
    auto* meterMap = ::mods::arg<dMeterMap_c*>(args, 0);
    if (dMap_c* map = ga::cast(meterMap)->getDMap()) {
        map->_draw();
    }
    return HOOK_SKIP_ORIGINAL;
}

// ---- minimap render (fork d_map.cpp / d_map_path.cpp) ----

dMap_c* minimap() {
    dMeterMap_c* meterMap = dMeter2Info_getMeterMapClass();
    return meterMap != NULL ? ga::cast(meterMap)->getDMap() : NULL;
}

// View adjustment for the live minimap render while the MAP page is panned or zoomed out:
// world-space centre offset plus a cm-per-texel multiplier. Applied to the members for the
// duration of the call.
f32 s_savedCenterX, s_savedCenterZ, s_savedTexel;
bool s_mapParked = false;

HookAction on_map_draw_pre(ModContext*, void* args, void*, void*) {
    auto* map = ::mods::arg<dMap_c*>(args, 0);
    f32 offX, offZ, texelScale;
    if (map == minimap() && companion::mapViewAdjust(&offX, &offZ, &texelScale)) {
        s_savedCenterX = map->mCenterX;
        s_savedCenterZ = map->mCenterZ;
        s_savedTexel = map->field_0x58;
        map->mCenterX += offX;
        map->mCenterZ += offZ;
        map->field_0x58 *= texelScale;
        s_mapParked = true;
    }
    return HOOK_CONTINUE;
}

void on_map_draw_post(ModContext*, void* args, void*, void*) {
    auto* map = ::mods::arg<dMap_c*>(args, 0);
    if (s_mapParked) {
        map->mCenterX = s_savedCenterX;
        map->mCenterZ = s_savedCenterZ;
        map->field_0x58 = s_savedTexel;
        s_mapParked = false;
    }
}

// Smaller player/restart cursor on the companion minimap, shrinking further when zoomed out.
void on_cursor_size_post(ModContext*, void* args, void* retval, void*) {
    auto* map = ::mods::arg<renderingAmap_c*>(args, 0);
    if (map != minimap()) {
        return;
    }
    f32& size = *static_cast<f32*>(retval);
    if (dualscreen::hudOnCompanion()) {
        size *= 0.72f;
    }
    f32 offX, offZ, texelScale;
    if (companion::mapViewAdjust(&offX, &offZ, &texelScale) && texelScale > 1.0f) {
        size /= texelScale;
    }
}

// The minimap render texture gets 2.4x the resolution while it is the companion's map page
// (drawn much larger there); the game's own map screens keep the stock size.
void on_map_render_size_post(ModContext*, void*, void* retval, void*) {
    const int windowStatus = dMeter2Info_getWindowStatus();
    const bool gameMapScreenOpen = windowStatus == 4 || windowStatus == 5;
    if (!dualscreen::hudOnCompanion() || gameMapScreenOpen) {
        return;
    }
    auto& size = *static_cast<aurora::Vec2<u16>*>(retval);
    auto boost = [](u16 v) {
        const u32 scaled = static_cast<u32>(static_cast<f32>(v) * 2.4f + 0.5f);
        return static_cast<u16>(scaled > 0xFFFFu ? 0xFFFFu : scaled);
    };
    size.x = boost(size.x);
    size.y = boost(size.y);
}

// ---- companion slot I lives in select index 2 (fork d_com_inf_game.cpp / d_msg_object.cpp) ----

// Retail's index 2 is the Wii wolf down-button: dComIfGp_setSelectItem stores the RAW SLOT NUMBER
// there, which the companion's slot I (an ordinary item binding) then shows as whatever item
// that number is (rupees). Resolve it like the other item buttons instead.
HookAction on_set_select_item_pre(ModContext*, void* args, void*, void*) {
    const int idx = ::mods::arg<int>(args, 0);
    if (idx != SELECT_ITEM_DOWN) {
        return HOOK_CONTINUE;
    }
    if (dComIfGs_getSelectItemIndex(idx) != 0xFF) {
        const u8 item = dComIfGs_getItem(dComIfGs_getSelectItemIndex(idx), false);
        g_dComIfG_gameInfo.play.setSelectItem(idx, item);
        if (item == dItemNo_NONE_e) {
            dComIfGs_setSelectItemIndex(idx, 0xFF);
        }
    } else {
        g_dComIfG_gameInfo.play.setSelectItem(idx, dItemNo_NONE_e);
    }
    return HOOK_SKIP_ORIGINAL;
}

// Learning a scent parks the scent's item number in select index 2 — that would wipe slot I's
// binding. Every reader that matters gets the scent via dComIfGs_getCollectSmell (also set by
// this function), so put the binding back afterwards.
u8 s_smellSavedSlot = 0xFF;

HookAction on_set_smell_pre(ModContext*, void*, void*, void*) {
    s_smellSavedSlot = dComIfGs_getSelectItemIndex(SELECT_ITEM_DOWN);
    return HOOK_CONTINUE;
}

void on_set_smell_post(ModContext*, void*, void*, void*) {
    dComIfGs_setSelectItemIndex(SELECT_ITEM_DOWN, s_smellSavedSlot);
}

// ---- pad injection (fork mDoCPd_c::read) ----

void on_pad_read_post(ModContext*, void*, void*, void*) {
    // The companion's warp button acts as the map screen's Z (portal-warp mode). The fork
    // OR-ed warpTogglePressed() into dMenu_Fmap_c's dMw_Z_TRIGGER() sites; injecting a real Z
    // here reaches the same checks (one frame later, since the latch is set in mod_update).
    const bool injectZ = companion::consumeZPress() || companion::warpTogglePressed();
    const u32 holdMask = companion::padHoldMask() | slots::pad_hold_mask();
    interface_of_controller_pad& pad = mDoCPd_c::m_cpadInfo[0];
    if (injectZ) {
        pad.mButtonFlags |= PAD_TRIGGER_Z;
        pad.mPressedButtonFlags |= PAD_TRIGGER_Z;
    }
    static u32 sPrevHoldMask;
    pad.mButtonFlags |= holdMask;
    pad.mPressedButtonFlags |= holdMask & ~sPrevHoldMask;
    sPrevHoldMask = holdMask;
}

// ---- scene changes ----

HookAction on_scene_change_pre(ModContext*, void* args, void*, void*) {
    dualscreen::onSceneChangeReq(::mods::arg<s16>(args, 1));
    return HOOK_CONTINUE;
}

// ---- daAlink_c::execute: clothes/shield reload pump ----
//
// loadModelDVD frees Link's whole archive the frame the clothes timer hits 2, but changeLink()
// rebuilds the models only a later frame. Vanilla is safe because clothes only change inside
// the pause status window, where Link's procs never run. The companion equips gear during live
// play, so drive the reload to completion here so no gameplay frame runs over freed model
// data. Same for the shield swap. Bounded; bails to the next frame if the DVD thread stalls.
HookAction on_link_execute_pre(ModContext*, void* args, void* retval, void*) {
    auto* link = ::mods::arg<daAlink_c*>(args, 0);
    if (dComIfGp_isPauseFlag()) {
        return HOOK_CONTINUE;
    }
    if (link->mClothesChangeWaitTimer != 0 && link->mProcID != daAlink_c::PROC_METAMORPHOSE &&
        link->mProcID != daAlink_c::PROC_METAMORPHOSE_ONLY)
    {
        bool reloaded = false;
        for (int guard = 0; guard < 100000; guard++) {
            if (link->loadModelDVD() != 0) {
                reloaded = true;
                break;
            }
            std::this_thread::yield();
        }
        if (!reloaded) {
            *static_cast<int*>(retval) = 1;
            return HOOK_SKIP_ORIGINAL;
        }
    }
    if (link->mShieldChangeWaitTimer != 0) {
        bool reloaded = false;
        for (int guard = 0; guard < 100000; guard++) {
            if (link->loadShieldModelDVD() != 0) {
                reloaded = true;
                break;
            }
            std::this_thread::yield();
        }
        if (!reloaded) {
            *static_cast<int*>(retval) = 1;
            return HOOK_SKIP_ORIGINAL;
        }
    }
    return HOOK_CONTINUE;
}

}  // namespace

bool install() {
    using namespace ::mods::hook;
    bool ok = true;
    ok &= add_pre<MeterDrawDraw>(on_meter_draw_pre) == MOD_OK;
    ok &= add_post<MeterDrawDraw>(on_meter_draw_post) == MOD_OK;
    ok &= add_pre<MeterDrawPikariPane>(on_pikari_pre) == MOD_OK;
    ok &= add_pre<KanteraIconDraw>(on_kantera_icon_draw_pre) == MOD_OK;
    ok &= add_pre<PictureDrawRect>(on_picture_draw_pre) == MOD_OK;
    ok &= add_pre<MeterDrawPresentA>(on_present_a_pre) == MOD_OK;
    ok &= add_pre<MeterDrawPresentB>(on_present_b_pre) == MOD_OK;
    ok &= add_pre<PaneMgrShow>(on_pane_show_pre) == MOD_OK;
    ok &= add_pre<MeterDrawIconAlpha>(on_icon_alpha_pre) == MOD_OK;
    ok &= add_pre<Meter2Create>(on_meter2_create_pre) == MOD_OK;
    ok &= add_post<Meter2Delete>(on_meter2_delete_post) == MOD_OK;
    ok &= add_pre<Meter2Draw>(on_meter2_draw_pre) == MOD_OK;
    ok &= add_post<Meter2Draw>(on_meter2_draw_post) == MOD_OK;
    ok &= add_pre<MeterMapDraw>(on_meter_map_draw_pre) == MOD_OK;
    ok &= add_pre<MapDraw>(on_map_draw_pre) == MOD_OK;
    ok &= add_post<MapDraw>(on_map_draw_post) == MOD_OK;
    ok &= add_post<AmapPlayerCursorSize>(on_cursor_size_post) == MOD_OK;
    ok &= add_post<AmapRestartCursorSize>(on_cursor_size_post) == MOD_OK;
    ok &= add_post<MapRenderSizeFor>(on_map_render_size_post) == MOD_OK;
    ok &= add_post<PadRead>(on_pad_read_post) == MOD_OK;
    ok &= add_pre<SetSelectItem>(on_set_select_item_pre) == MOD_OK;
    ok &= add_pre<SetSmellType>(on_set_smell_pre) == MOD_OK;
    ok &= add_post<SetSmellType>(on_set_smell_post) == MOD_OK;
    ok &= add_pre<SceneChangeReq>(on_scene_change_pre) == MOD_OK;
    ok &= add_pre<LinkExecute>(on_link_execute_pre) == MOD_OK;
    if (!ok) {
        DuskLog.error("companion: one or more game hooks failed to install");
    }
    return ok;
}

}  // namespace dsc::hooks
