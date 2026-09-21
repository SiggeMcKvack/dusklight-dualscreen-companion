// Port of the fork's src/dusk/dualscreen.cpp: renders the companion dashboard into an offscreen
// pass each frame (from the mod's BEFORE/AFTER_HUD stage hooks, which fire exactly where the
// fork called begin/endHudCapture in mDoGph_Painter) and hands the resolved texture to the
// bottom-screen present target. The desktop aux window, swap-screens mirror and the screenshot
// aid are not ported.
#include "dusk/dualscreen.h"

#include "dusk/companion.h"
#include "dusk/logging.h"
#include "dusk/main.h"
#include "dusk/settings.h"

#include "JSystem/J2DGraph/J2DOrthoGraph.h"
#include "d/d_com_inf_game.h"
#include "f_pc/f_pc_name.h"
#include "m_Do/m_Do_graphic.h"
#include "m_Do/m_Do_mtx.h"

#include "../present.hpp"
#include "mods/service.hpp"
#include "mods/svc/gfx.h"

#include <atomic>

namespace dusk::dualscreen {
namespace {

bool s_active = false;
// Whether the boot/loading splash should draw this frame (dual screen on, but the dashboard
// has never presented yet).
bool s_splash = false;
std::atomic<bool> s_everPresented{false};
// Set when the current scene change leaves gameplay (title / file select): the dashboard keeps
// drawing through the fade-out, and once the HUD meter dies the splash returns instead of a
// frozen last frame. Play-to-play stage transitions clear it.
bool s_leftGameplay = false;
// Low-health hearts pop-in latch (Cinematic only); see lowLifePopIn().
bool s_lowLifeLatch = false;
// Second physical display present (reported by the Java side).
std::atomic<bool> s_displayAvailable{false};
// Surface size of the bottom panel, for the canvas aspect.
std::atomic<uint32_t> s_surfaceW{0};
std::atomic<uint32_t> s_surfaceH{0};
// Last resolved dashboard texture, re-presented (with the live dim) on frames where the capture
// is skipped, so stage transitions hold the last frame instead of flashing.
WGPUTextureView s_lastView = nullptr;
uint32_t s_lastW = 0;
uint32_t s_lastH = 0;
float s_dim = 0.0f;

bool isEnabled() {
    // The logo scene draws its 2D lists through the same pass; only redirect once actual
    // gameplay is running. Without a physical second display the setting is inert.
    return getSettings().game.dualScreen.getValue() && s_displayAvailable &&
           (dusk::IsGameLaunched || companion::hudReady());
}

// The game leaves alpha writes disabled for most 2D drawing, so the capture's alpha channel
// stays at the pass-clear value (0) and alpha-blended consumers render it invisible. Stamp
// alpha=1 across the target, leaving RGB untouched, before resolving.
void stampOpaqueAlpha() {
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT_NULL, GX_DF_NONE,
        GX_AF_NONE);
    GXSetChanMatColor(GX_COLOR0A0, {0xFF, 0xFF, 0xFF, 0xFF});
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_SET);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetZMode(GX_DISABLE, GX_ALWAYS, GX_DISABLE);
    GXSetCullMode(GX_CULL_NONE);
    GXSetClipMode(GX_CLIP_DISABLE);
    GXSetColorUpdate(GX_FALSE);
    GXSetAlphaUpdate(GX_TRUE);
    GXLoadPosMtxImm(cMtx_getIdentity(), GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3s16(-0x4000, -0x4000, 0);
    GXPosition3s16(0x4000, -0x4000, 0);
    GXPosition3s16(0x4000, 0x4000, 0);
    GXPosition3s16(-0x4000, 0x4000, 0);
    GXEnd();
    GXSetColorUpdate(GX_TRUE);
    GXSetAlphaUpdate(GX_TRUE);
    dComIfGp_getCurrentGrafPort()->setup2D();
}

// Canvas geometry limits: logical canvas width clamp and the accepted native surface size range
// (fallback to 2x supersample outside it).
constexpr u32 kMinCanvasW = 320;
constexpr u32 kMaxCanvasW = 1024;
constexpr u32 kMinNativeDim = 320;
constexpr u32 kMaxNativeDim = 2048;

// Match the dashboard canvas to the panel's aspect ratio (8:7 on the AYN Thor) so the blit
// fills it edge to edge, and render at the panel's native resolution (1:1 blit).
void computeAuxCanvas(u32& canvasW, u32& canvasH, u32& texW, u32& texH) {
    f32 aspect = (f32)FB_WIDTH / (f32)FB_HEIGHT;
    const u32 auxWidth = s_surfaceW.load();
    const u32 auxHeight = s_surfaceH.load();
    if (auxWidth != 0 && auxHeight != 0) {
        aspect = (f32)auxWidth / (f32)auxHeight;
    }
    canvasH = FB_HEIGHT;
    canvasW = (u32)((f32)canvasH * aspect + 0.5f);
    if (canvasW < kMinCanvasW) {
        canvasW = kMinCanvasW;
    } else if (canvasW > kMaxCanvasW) {
        canvasW = kMaxCanvasW;
    }
    texW = canvasW * 2;
    texH = canvasH * 2;
    if (auxWidth >= kMinNativeDim && auxHeight >= kMinNativeDim && auxWidth <= kMaxNativeDim &&
        auxHeight <= kMaxNativeDim)
    {
        texW = auxWidth;
        texH = auxHeight;
    }
}

void setLastView(WGPUTextureView view, uint32_t w, uint32_t h) {
    if (view != nullptr) {
        wgpuTextureViewAddRef(view);
    }
    if (s_lastView != nullptr) {
        wgpuTextureViewRelease(s_lastView);
    }
    s_lastView = view;
    s_lastW = w;
    s_lastH = h;
}

void presentLast() {
    dsc::present::push_frame(s_lastView, s_lastW, s_lastH, s_dim);
}

}  // namespace

void beginHudCapture() {
    const bool wanted = getSettings().game.dualScreen.getValue() && s_displayAvailable;
    const bool enabled = isEnabled();
    s_active = enabled;
    // Boot/loading: keep the second screen on the branded splash instead of black until the
    // dashboard has real content. Leaving gameplay (quit to title, game over) re-arms it.
    s_splash = wanted && (!s_everPresented || s_leftGameplay);

    {
        const bool ready = companion::hudReady();
        const int state = (wanted ? 1 : 0) | (enabled ? 2 : 0) | (ready ? 4 : 0) |
                          (s_splash ? 8 : 0) | (s_displayAvailable ? 16 : 0) |
                          (s_everPresented ? 32 : 0) | (s_leftGameplay ? 64 : 0);
        static int s_lastState = -1;
        if (state != s_lastState) {
            s_lastState = state;
            DuskLog.info("dualscreen: displayAvailable={} setting={} launched={} wanted={} "
                         "enabled={} hudReady={} splash={} everPresented={} leftGameplay={}",
                (bool)s_displayAvailable, getSettings().game.dualScreen.getValue(),
                (bool)dusk::IsGameLaunched, wanted, enabled, ready, s_splash,
                (bool)s_everPresented, s_leftGameplay);
        }
    }

    // Low-health hearts pop-in (Cinematic only). Life is 4 units/heart, max is 5 units/heart.
    // Threshold scales with max hearts, clamped to 1..2; release one full heart higher.
    if (!enabled || mainHudRestored() || !companion::hudReady()) {
        s_lowLifeLatch = false;
    } else {
        int showHearts = (dComIfGs_getMaxLife() / 5) / 4;
        if (showHearts < 1) {
            showHearts = 1;
        } else if (showHearts > 2) {
            showHearts = 2;
        }
        const u16 life = dComIfGs_getLife();
        if (life <= (u16)(showHearts * 4)) {
            s_lowLifeLatch = true;
        } else if (life > (u16)((showHearts + 1) * 4)) {
            s_lowLifeLatch = false;
        }
    }

    if (enabled) {
        companion::update();
    }
}

void endHudCapture() {
    const bool splash = s_splash && (!s_active || !companion::hudReady());
    s_splash = false;
    if (!s_active && !splash) {
        presentLast();
        return;
    }
    s_active = false;

    // The dim is applied by the present blit, not painted into the dashboard: on frames where
    // the capture is skipped the panel keeps presenting its last texture and the fade still
    // advances over it. The splash owns its own look and is never dimmed.
    if (splash) {
        companion::resetDim();
        s_dim = 0.0f;
    } else {
        s_dim = companion::currentDim();
    }

    // During stage transitions the HUD meter is destroyed and rebuilt; skip the capture so the
    // second screen keeps its last frame instead of flashing a half-empty dashboard.
    if (!splash && !companion::hudReady()) {
        presentLast();
        return;
    }

    u32 canvasW, canvasH, texW, texH;
    computeAuxCanvas(canvasW, canvasH, texW, texH);
    if (svc_gfx->create_pass(mod_ctx, texW, texH) != MOD_OK) {
        presentLast();
        return;
    }

    // Pixel-exact 2D context for the dashboard, independent of the main screen's (possibly
    // widescreen) ortho extents.
    J2DGrafContext* prevPort = dComIfGp_getCurrentGrafPort();
    if (prevPort == NULL) {
        GfxResolveDesc discard = GFX_RESOLVE_DESC_INIT;
        discard.color = false;
        GfxResolvedTargets discarded = GFX_RESOLVED_TARGETS_INIT;
        svc_gfx->resolve_pass(mod_ctx, &discard, &discarded);
        presentLast();
        return;
    }
    J2DOrthoGraph ortho(0.0f, 0.0f, (f32)canvasW, (f32)canvasH, -1.0f, 1.0f);
    companion::setNativeCanvas(texW, texH, (f32)texH / (f32)canvasH);
    dComIfGp_setCurrentGrafPort(&ortho);
    ortho.setPort();
    companion::applyNativeViewport();

    if (splash) {
        companion::drawSplash((f32)canvasW, (f32)canvasH);
    } else {
        companion::drawDashboard((f32)canvasW, (f32)canvasH);
        s_everPresented = true;
    }
    stampOpaqueAlpha();

    dComIfGp_setCurrentGrafPort((J2DOrthoGraph*)prevPort);

    GfxResolveDesc resolve = GFX_RESOLVE_DESC_INIT;
    resolve.color = true;
    resolve.depth = false;
    GfxResolvedTargets targets = GFX_RESOLVED_TARGETS_INIT;
    if (svc_gfx->resolve_pass(mod_ctx, &resolve, &targets) == MOD_OK && targets.color != nullptr) {
        setLastView(targets.color, targets.width, targets.height);
    }
    presentLast();

    prevPort->setPort();
}

bool hudOnCompanion() {
    return getSettings().game.dualScreen.getValue() && s_displayAvailable;
}

bool mainHudRestored() {
    return hudOnCompanion() &&
           getSettings().game.dualScreenHudMode.getValue() == kDualHudFunctional;
}

bool mainHudActive() {
    return !hudOnCompanion() || mainHudRestored();
}

bool lowLifePopIn() {
    return s_lowLifeLatch;
}

void onSceneChangeReq(short procName) {
    s_leftGameplay = procName != fpcNm_PLAY_SCENE_e;
}

bool leftGameplay() {
    return s_leftGameplay;
}

void setDisplayAvailable(bool available) {
    s_displayAvailable = available;
    if (!available) {
        s_everPresented = false;
    }
}

void setSurfaceSize(uint32_t width, uint32_t height) {
    s_surfaceW = width;
    s_surfaceH = height;
}

void shutdown() {
    setLastView(nullptr, 0, 0);
    s_everPresented = false;
    s_active = false;
}

void publishSwapPreference(bool) {}

}  // namespace dusk::dualscreen
