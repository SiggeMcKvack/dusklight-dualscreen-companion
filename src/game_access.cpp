// Fork-added game accessors, transplanted onto the access views declared in game_access.h.
// Bodies are the fork's (igawa6/dusklight src/d/*.cpp) with the class names swapped.
#include "dusk/game_access.h"

#include "dusk/dualscreen.h"

#include "JSystem/JKernel/JKRArchive.h"
#include "Z2AudioLib/Z2AudioMgr.h"
#include "d/d_com_inf_game.h"
#include "d/d_item.h"
#include "d/d_kantera_icon_meter.h"
#include "d/d_meter_HIO.h"
#include "JSystem/J2DGraph/J2DGrafContext.h"
#include "JSystem/J2DGraph/J2DTextBox.h"
#include "d/d_msg_unit.h"
#include "d/d_msg_class.h"
#include "d/actor/d_a_midna.h"
#include "JSystem/JMessage/JMessage.h"
#include "d/d_save.h"
#include "m_Do/m_Do_controller_pad.h"

// ---------------------------------------------------------------------------
// dMeter2Draw_c (fork d_meter2_draw.cpp:65-577)
// ---------------------------------------------------------------------------

bool dMeter2DrawAccess::sItemUsable[4] = {true, true, true, true};

// The action labels are pre-rendered texture panes (the label texture is
// swapped per context action). Expose the current texture rather than the
// pane: pane-level draws inherit animated transforms and render skewed.
const ResTIMG* dMeter2DrawAccess::getActionLabelTimg(int i_which) {
    CPaneMgr* mgr = i_which == 0 ? mpBTextA : mpBTextB;
    if (mgr == NULL || mgr->getPanePtr() == NULL || !mgr->getPanePtr()->isVisible()) {
        return NULL;
    }
    J2DPicture* pic = (J2DPicture*)mgr->getPanePtr();
    if (pic->getTexture(0) == NULL) {
        return NULL;
    }
    return pic->getTexture(0)->getTexInfo();
}

// First textured picture in a pane subtree. Prefers color textures over
// intensity ones (formats 0-3): button subtrees start with greyscale drop
// shadows that would otherwise win.
static J2DPicture* findPictureWithTexture(J2DPane* i_pane, bool i_colorOnly) {
    if (i_pane == NULL) {
        return NULL;
    }
    if (i_pane->getKind() == MULTI_CHAR('PIC1') || i_pane->getKind() == MULTI_CHAR('PIC2')) {
        J2DPicture* pic = (J2DPicture*)i_pane;
        if (pic->getTexture(0) != NULL &&
            (!i_colorOnly || pic->getTexture(0)->getFormat() > 3))
        {
            return pic;
        }
    }
    for (J2DPane* child = i_pane->getFirstChildPane(); child != NULL;
         child = child->getNextChildPane())
    {
        J2DPicture* pic = findPictureWithTexture(child, i_colorOnly);
        if (pic != NULL) {
            return pic;
        }
    }
    return NULL;
}

static J2DPicture* findBestPicture(CPaneMgr* i_mgr) {
    if (i_mgr == NULL || i_mgr->getPanePtr() == NULL) {
        return NULL;
    }
    J2DPicture* pic = findPictureWithTexture(i_mgr->getPanePtr(), true);
    if (pic == NULL) {
        pic = findPictureWithTexture(i_mgr->getPanePtr(), false);
    }
    return pic;
}

// ---------------------------------------------------------------------------
// Dual-screen companion accessors (dusk fork): pane/texture getters and draw
// hooks consumed by src/dusk/companion*.cpp. Everything up to initLife().
// ---------------------------------------------------------------------------

// Button pane managers: 0=A 1=B 2=X 3=Y 4=Z, NULL otherwise.
CPaneMgr* dMeter2DrawAccess::getButtonMgr(int i_which) {
    switch (i_which) {
    case 0:
        return mpButtonA;
    case 1:
        return mpButtonB;
    case 2:
    case 3:
    case 4:
        return mpButtonXY[i_which - 2];
    default:
        return NULL;
    }
}

// Whole button pane subtrees (0=A 1=B 2=X 3=Y), for layer-composited
// rendering on the companion.
J2DPane* dMeter2DrawAccess::getButtonPane(int i_which) {
    CPaneMgr* mgr = getButtonMgr(i_which);
    return mgr != NULL ? mgr->getPanePtr() : NULL;
}

// Face button base pictures for the companion cluster (0=A 1=B 2=X 3=Y).
// Returned as pictures so the caller can reuse their tint colors: the button
// graphics are intensity textures colored by the pane's black/white TEV.
J2DPicture* dMeter2DrawAccess::getButtonBasePicture(int i_which) {
    return findBestPicture(getButtonMgr(i_which));
}

// Current A action word ("Speak", "Blow", ...); the mpAText panes are real
// text boxes (unlike b_text_a, which is the "A" glyph picture). NULL/empty
// when no contextual action.
const char* dMeter2DrawAccess::getActionTextA() {
    if (mpAText[0] == NULL || mpAText[0]->getPanePtr() == NULL ||
        !mpAText[0]->getPanePtr()->isVisible())
    {
        return NULL;
    }
    return (const char*)((J2DTextBox*)mpAText[0]->getPanePtr())->getStringPtr();
}

// B-button action word ("Attack", "Dig", ...); NULL when the game hides it.
const char* dMeter2DrawAccess::getActionTextB() {
    if (mpTextB == NULL || mpTextB->getPanePtr() == NULL ||
        !mpTextB->getPanePtr()->isVisible() || mpBText[0] == NULL ||
        mpBText[0]->getPanePtr() == NULL)
    {
        return NULL;
    }
    return (const char*)((J2DTextBox*)mpBText[0]->getPanePtr())->getStringPtr();
}

// X/Y-button action words ("Sense", "Dig", ... — wolf form); NULL when the
// game hides them. i_no: 0 = X, 1 = Y.
const char* dMeter2DrawAccess::getActionTextXY(int i_no) {
    if (i_no < 0 || i_no > 1 || mpTextXY[i_no] == NULL ||
        mpTextXY[i_no]->getPanePtr() == NULL || mpXYText[0][i_no] == NULL ||
        mpXYText[0][i_no]->getPanePtr() == NULL)
    {
        return NULL;
    }
    // ONLY Functional hides these panes by hand (its cluster stays visible,
    // so the words would otherwise draw on the main screen). There the flag
    // is ours, not the game's, so it cannot gate — the caller checks wolf
    // form instead. Every other mode keeps the game's own flag authoritative.
    if (!dusk::dualscreen::mainHudRestored() && !mpTextXY[i_no]->getPanePtr()->isVisible()) {
        return NULL;
    }
    return (const char*)((J2DTextBox*)mpXYText[0][i_no]->getPanePtr())->getStringPtr();
}

// Midna (Z) button pane subtree regardless of the prompt's active state
// (the companion dims it instead of hiding).
J2DPane* dMeter2DrawAccess::getMidnaButtonPaneRaw() {
    if (mpButtonMidona == NULL || mpButtonMidona->getPanePtr() == NULL) {
        return NULL;
    }
    // Functional hides the MAIN-screen copy of this pane (the partition owns
    // its visibility there, and the companion Z corner is the caller) — that
    // deliberate hide must not read as "prompt gone".
    if (!dusk::dualscreen::mainHudRestored() && !mpButtonMidona->getPanePtr()->isVisible()) {
        return NULL;
    }
    return mpButtonMidona->getPanePtr();
}

// A tear-of-light picture from the vessel layout.
J2DPicture* dMeter2DrawAccess::getLightDropPicture() {
    return findBestPicture(mpLightDropParent);
}

// D-pad label strings from the cross HUD textboxes (localized by the game).
const char* dMeter2DrawAccess::getDpadLabel(int i_no) {
    if (mpScreen == NULL) {
        return NULL;
    }
    J2DPane* pane = mpScreen->search(i_no == 0 ? MULTI_CHAR('cont_ju0') : MULTI_CHAR('cont_ju5'));
    if (pane == NULL) {
        return NULL;
    }
    return (const char*)((J2DTextBox*)pane)->getStringPtr();
}

// Midna prompt pulse, drawn at companion coordinates (frame state shared
// with the main HUD, which skips its own draw in dual-screen mode).
void dMeter2DrawAccess::drawMidnaPikariAt(f32 i_posX, f32 i_posY) {
    if (field_0x738 <= 0.0f) {
        return;
    }
    drawPikari(i_posX, i_posY, &field_0x738, g_drawHIO.mMidnaIconPikariScale,
               g_drawHIO.mMidnaIconPikariFrontOuter, g_drawHIO.mMidnaIconPikariFrontInner,
               g_drawHIO.mMidnaIconPikariBackOuter, g_drawHIO.mMidnaIconPikariBackInner,
               g_drawHIO.mMidnaIconPikariAnimSpeed, 3);
}

// D-pad cross HUD pane subtree (up = item wheel, right = map).
J2DPane* dMeter2DrawAccess::getButtonCrossPane() {
    return mpButtonCrossParent != NULL ? mpButtonCrossParent->getPanePtr() : NULL;
}

// Vessel of Light pane subtree for full companion compositing.
J2DPane* dMeter2DrawAccess::getLightDropPane() {
    return mpLightDropParent != NULL ? mpLightDropParent->getPanePtr() : NULL;
}

// Whether the X (0) / Y (1) item can currently be used. Reads the per-frame
// snapshot taken in setButtonIconAlpha — the live dMeter2Info bits are reset
// before the companion's draw pass runs, so they can't be read directly.
bool dMeter2DrawAccess::isItemUsable(int i_xy) {
    if (i_xy < 0 || i_xy > 3) {
        return false;
    }
    return sItemUsable[i_xy];
}

bool dMeter2DrawAccess::isOxygenActive() {
    // The game fades the bar in and out through mMeterAlphaRate; riding that
    // keeps the companion in step with the main screen's own timing instead
    // of popping the moment Link enters water.
    return mMeterAlphaRate[2] > 0.0f && dComIfGp_getMaxOxygen() > 0;
}

J2DPane* dMeter2DrawAccess::getVesselTearPane(int i_idx) {
    if (i_idx < 0 || i_idx >= 16 || mpSIParts[i_idx][1] == NULL) {
        return NULL;
    }
    return mpSIParts[i_idx][1]->getPanePtr();
}

// Companion twin of the main-screen vessel tear glow (skipped there in
// dual-screen mode): same animation state machine, but each tear's pikari
// draws at the caller-mapped companion position, sized by i_sizeScale.
// Positions below -9000 mark tears the caller could not map.
void dMeter2DrawAccess::drawVesselPikariForCompanion(const f32* i_tearX, const f32* i_tearY,
                                                 f32 i_sizeScale) {
    if (mpLightDropParent == NULL || mpLightDropParent->getAlphaRate() == 0.0f) {
        return;
    }

    f32 dropScale = g_drawHIO.mLightDrop.mPikariScaleNormal;
    f32 dropSpeed = g_drawHIO.mLightDrop.mDropPikariAnimSpeed;

    if (field_0x756 >= 0) {
        dropSpeed = g_drawHIO.mLightDrop.mDropPikariAnimSpeed_Completed;
        int completeEnd = g_drawHIO.mLightDrop.mPikariInterval * 15;
        dropScale = g_drawHIO.mLightDrop.mPikariScaleComplete;
        if (true /* frame_interp tick gate not exported to mods */) {
            if (field_0x756 <= completeEnd) {
                const int tick = (int)field_0x756;
                int phase = tick % g_drawHIO.mLightDrop.mPikariInterval;
                int tearIdx = tick / g_drawHIO.mLightDrop.mPikariInterval;

                if (phase == 0 && field_0x62c[tearIdx] == 0.0f) {
                    field_0x62c[tearIdx] = 18.0f;
                }
                field_0x756++;
            } else {
                int holdStart = completeEnd + 1;

                if (field_0x756 == holdStart) {
                    if (field_0x62c[15] == 0.0f) {
                        field_0x756++;
                    }
                } else if (field_0x756 >= g_drawHIO.mLightDrop.field_0x54 + holdStart) {
                    for (int i = 0; i < 16; i++) {
                        field_0x62c[i] = 18.0f - dropSpeed;
                        field_0x66c[i] = 18.0f - g_drawHIO.mLightDrop.mPikariLoopAnimSpeed;
                    }
                    field_0x756 = -1;
                } else {
                    field_0x756++;
                }
            }
        }
    }

    for (int i = 0; i < 16; i++) {
        if (i_tearX[i] < -9000.0f) {
            continue;
        }
        if (field_0x66c[i] > 0.0f) {
            drawPikari(i_tearX[i], i_tearY[i], &g_drawHIO.mLightDrop.mPikariLoopBackStopFrame,
                       g_drawHIO.mLightDrop.mPikariLoopBackScale * i_sizeScale,
                       g_drawHIO.mLightDrop.mPikariLoopFrontOuter[1],
                       g_drawHIO.mLightDrop.mPikariLoopFrontInner[1],
                       g_drawHIO.mLightDrop.mPikariLoopBackOuter[1],
                       g_drawHIO.mLightDrop.mPikariLoopBackInner[1], 0.0f, 3);
            drawPikari(i_tearX[i], i_tearY[i], &field_0x66c[i],
                       g_drawHIO.mLightDrop.mPikariLoopScale * i_sizeScale,
                       g_drawHIO.mLightDrop.mPikariLoopFrontOuter[0],
                       g_drawHIO.mLightDrop.mPikariLoopFrontInner[0],
                       g_drawHIO.mLightDrop.mPikariLoopBackOuter[0],
                       g_drawHIO.mLightDrop.mPikariLoopBackInner[0],
                       g_drawHIO.mLightDrop.mPikariLoopAnimSpeed, 3);
        }

        if (g_drawHIO.mLightDrop.mAnimDebug &&
            dComIfGp_getNeedLightDropNum() !=
                dComIfGs_getLightDropNum(dComIfGp_getStartStageDarkArea()))
        {
            field_0x66c[i] = 0.0f;
        }

        if (field_0x62c[i] > 0.0f) {
            drawPikari(i_tearX[i], i_tearY[i], &field_0x62c[i], dropScale * i_sizeScale,
                       g_drawHIO.mLightDrop.mDropPikariFrontOuter,
                       g_drawHIO.mLightDrop.mDropPikariFrontInner,
                       g_drawHIO.mLightDrop.mDropPikariBackOuter,
                       g_drawHIO.mLightDrop.mDropPikariBackInner, dropSpeed, field_0x75f);
        }
    }
}

// Re-apply the vessel's per-drop fill tints/alphas from the live tear count
// at full opacity — the game's own fade leaves it invisible between pickups,
// but the companion shows it for the whole quest.
void dMeter2DrawAccess::refreshVesselForCompanion() {
    if (mpLightDropParent == NULL) {
        return;
    }
    mpLightDropParent->setAlphaRate(g_drawHIO.mParentAlpha);
    for (int i = 0; i < 2; i++) {
        if (mpSIParent[i] != NULL) {
            mpSIParent[i]->setAlphaRate(1.0f);
        }
    }
    drawLightDrop(dComIfGs_getLightDropNum(dComIfGp_getStartStageDarkArea()),
                  dComIfGp_getNeedLightDropNum(), g_drawHIO.mLightDrop.mVesselPosX,
                  g_drawHIO.mLightDrop.mVesselPosY, g_drawHIO.mLightDrop.mVesselScale,
                  g_drawHIO.mLightDrop.mVesselAlpha[0], 0);
}

// The vessel panes whose state the companion saves and restores: the parent,
// the two tear-count parents, then the 16 tears' two parts each.
void dMeter2DrawAccess::collectVesselPanes(CPaneMgr** o_panes) {
    o_panes[0] = mpLightDropParent;
    for (int i = 0; i < 2; i++) {
        o_panes[1 + i] = mpSIParent[i];
    }
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 2; j++) {
            o_panes[3 + i * 2 + j] = mpSIParts[i][j + 1];
        }
    }
}

void dMeter2DrawAccess::pushVesselStateForCompanion(f32* o_alpha, f32* o_x, f32* o_y,
    f32* o_scale) {
    CPaneMgr* panes[VESSEL_ALPHA_SAVE_COUNT];
    collectVesselPanes(panes);
    for (int k = 0; k < VESSEL_ALPHA_SAVE_COUNT; k++) {
        if (panes[k] != NULL) {
            o_alpha[k] = panes[k]->getAlphaRate();
            o_x[k] = panes[k]->getPosX();
            o_y[k] = panes[k]->getPosY();
        } else {
            o_alpha[k] = 0.0f;
            o_x[k] = 0.0f;
            o_y[k] = 0.0f;
        }
    }
    o_scale[0] = panes[0] != NULL ? panes[0]->getScaleX() : 1.0f;
    o_scale[1] = panes[0] != NULL ? panes[0]->getScaleY() : 1.0f;
    // Canonical corner layout (positions, scale, textures, alphas).
    refreshVesselForCompanion();
}

void dMeter2DrawAccess::popVesselStateForCompanion(const f32* i_alpha, const f32* i_x,
    const f32* i_y, const f32* i_scale) {
    CPaneMgr* panes[VESSEL_ALPHA_SAVE_COUNT];
    collectVesselPanes(panes);
    for (int k = 0; k < VESSEL_ALPHA_SAVE_COUNT; k++) {
        if (panes[k] != NULL) {
            panes[k]->setAlphaRate(i_alpha[k]);
            panes[k]->getPanePtr()->move(i_x[k], i_y[k]);
        }
    }
    if (panes[0] != NULL) {
        panes[0]->scale(i_scale[0], i_scale[1]);
    }
}


// The live kantera (lantern oil) meters, repositionable via setPos.
dKantera_icon_c* dMeter2DrawAccess::getKanteraMeter(int i_no) {
    if (i_no < 0 || i_no >= 2) {
        return NULL;
    }
    return mpKanteraMeter[i_no];
}

// Dual-screen: the status panes are hidden on the main screen and composited
// on the companion, which should never inherit the main HUD's idle fade —
// pin them fully opaque every frame (runs after the game's own fade logic).
void dMeter2DrawAccess::forceCompanionAlpha() {
    const f32 full = g_drawHIO.mParentAlpha;
    const f32 btn = g_drawHIO.mParentAlpha * g_drawHIO.mMainHUDButtonsAlpha;
    // Pin only what the companion actually composites. In Cinematic that is
    // everything below, and pinning is invisible because those panes are
    // hidden on the main screen. Functional moves hearts, the A/B/Z cluster
    // and the d-pad back to the main screen, where the game's own fades have
    // to play out — pinning them there would freeze the HUD fully opaque
    // through cutscenes. The panes that stay on the companion (rupee/key
    // readouts, X/Y) are hidden on main in both modes, so they are always
    // safe to pin.
    const bool restored = dusk::dualscreen::mainHudRestored();
    if (!restored && mpLifeParent != NULL && mpLifeParent->getAlphaRate() != full) {
        mpLifeParent->setAlphaRate(full);
        setAlphaLifeChange(true);
    }
    if (mpRupeeParent[0] != NULL) {
        mpRupeeParent[0]->setAlphaRate(full);
    }
    if (mpKeyParent != NULL) {
        mpKeyParent->setAlphaRate(full);
    }
    if (!restored && mpButtonParent != NULL && mpButtonParent->getAlphaRate() != btn) {
        // Pinning the parent propagates init alphas to ALL children — but the
        // Midna prompt is alpha-gated by game state; preserve its own alpha.
        u8 midnaAlpha = 0;
        const bool hasMidna = mpButtonMidona != NULL && mpButtonMidona->getPanePtr() != NULL;
        if (hasMidna) {
            midnaAlpha = mpButtonMidona->getPanePtr()->getAlpha();
        }
        mpButtonParent->setAlphaRate(btn);
        if (hasMidna) {
            mpButtonMidona->setAlpha(midnaAlpha);
        }
    }
    if (!restored && mpButtonCrossParent != NULL) {
        mpButtonCrossParent->setAlphaRate(btn);
    }
    // X and Y stay on the companion in both modes.
    if (mpButtonXY[0] != NULL) {
        mpButtonXY[0]->setAlphaRate(btn);
    }
    if (mpButtonXY[1] != NULL) {
        mpButtonXY[1]->setAlphaRate(btn);
    }
    // Z rides with the cluster, so Functional leaves it to the main screen.
    if (!restored && mpButtonXY[2] != NULL) {
        mpButtonXY[2]->setAlphaRate(btn);
    }
}

bool dMeter2DrawAccess::isButtonClusterVisible() {
    // The companion cluster is always shown (user preference): cutscenes,
    // grass blowing, dialogue, the item wheel and pause screens all hide or
    // fade the main HUD buttons, but the second screen keeps them.
    return mpButtonParent != NULL;
}

// Counter pane subtrees (icon + live digits): 0 = rupees, 1 = small keys.
J2DPane* dMeter2DrawAccess::getCounterPane(int i_which) {
    CPaneMgr* mgr = i_which == 0 ? mpRupeeParent[0] : mpKeyParent;
    return mgr != NULL ? mgr->getPanePtr() : NULL;
}

// Collects the pictures composing heart i_no as currently displayed (empty
// base + full fill, or base + quarter texture for the partial heart), letting
// the companion dashboard draw the original assets at any size. Returns the
// number of pictures written to o_pics (up to 2); 0 when the heart is hidden.
int dMeter2DrawAccess::getHeartPictures(int i_no, J2DPicture** o_pics) {
    if (i_no < 0 || i_no >= 20 || mpLifeParts[i_no] == NULL ||
        mpLifeParts[i_no]->getPanePtr() == NULL ||
        !mpLifeParts[i_no]->getPanePtr()->isVisible())
    {
        return 0;
    }
    int count = 0;
    if (mpHeartBase[i_no] != NULL && mpHeartBase[i_no]->getPanePtr() != NULL) {
        o_pics[count++] = (J2DPicture*)mpHeartBase[i_no]->getPanePtr();
    }
    if (mpLifeTexture[i_no][1] != NULL && mpLifeTexture[i_no][1]->getPanePtr() != NULL &&
        mpLifeTexture[i_no][1]->getPanePtr()->isVisible())
    {
        // Full heart.
        o_pics[count++] = (J2DPicture*)mpLifeTexture[i_no][1]->getPanePtr();
    } else if (mpBigHeart != NULL && mpBigHeart->getPanePtr() != NULL &&
               mpBigHeart->getPanePtr()->isVisible() && mpScreen != NULL)
    {
        // The heart drawLife parks the shared quarter pane on (also used for
        // the last full heart, showing bigh_00): pick whichever quarter
        // texture the game left visible.
        const s16 life = dComIfGs_getLife();
        s16 partialHeart = life / 4;
        if (life % 4 == 0) {
            partialHeart--;
        }
        if (i_no == partialHeart) {
            static u64 const tag_bigh[] = {MULTI_CHAR('bigh_00'), MULTI_CHAR('bigh_01'),
                MULTI_CHAR('bigh_02'), MULTI_CHAR('bigh_03')};
            for (u64 tag : tag_bigh) {
                J2DPane* quarterPane = mpScreen->search(tag);
                if (quarterPane != NULL && quarterPane->isVisible()) {
                    o_pics[count++] = (J2DPicture*)quarterPane;
                    break;
                }
            }
        }
    }
    return count;
}

f32 dMeter2DrawAccess::getLightDropAlpha() {
    return mpLightDropParent != NULL ? mpLightDropParent->getAlphaRate() : 0.0f;
}
// ---------------------------------------------------------------------------
// Menus (fork d_menu_dmap.cpp / d_menu_fmap2D.cpp)
// ---------------------------------------------------------------------------

// Live A/B prompt for the companion cluster; the dungeon map empties the string when hidden.
const char* dMenuDmapBgAccess::getButtonLabel(int i_which) {
    if (mButtonScreen == NULL) {
        return NULL;
    }
    J2DPane* pane =
        mButtonScreen->search(i_which == 0 ? MULTI_CHAR('font_at') : MULTI_CHAR('font_bt'));
    if (pane == NULL) {
        return NULL;
    }
    return (const char*)((J2DTextBox*)pane)->getStringPtr();
}

// NULL when the map screen has the prompt hidden (mAlphaButton* at ALPHA_MIN).
const char* dMenuFmap2DTopAccess::getButtonLabel(int i_which) {
    if (mpTitleScreen == NULL) {
        return NULL;
    }
    if ((i_which == 0 ? mAlphaButtonA : mAlphaButtonB) == ALPHA_MIN) {
        return NULL;
    }
    J2DPane* pane =
        mpTitleScreen->search(i_which == 0 ? MULTI_CHAR('font_at1') : MULTI_CHAR('font_bt1'));
    if (pane == NULL) {
        return NULL;
    }
    return (const char*)((J2DTextBox*)pane)->getStringPtr();
}

// ---------------------------------------------------------------------------
// daAlink_c quick transform (fork d_a_alink_dusk.cpp)
// ---------------------------------------------------------------------------

bool daAlinkAccess::checkQuickTransformOK() {
    if (!dComIfGs_isEventBit(dSv_event_flag_c::M_077)) {
        return false;
    }
    const auto meterClassPtr = g_meter2_info.getMeterClass();
    if (!meterClassPtr) {
        return false;
    }
    const auto meterDrawPtr = meterClassPtr->getMeterDrawPtr();
    if (!meterDrawPtr) {
        return false;
    }
    if (checkEventRun()) {
        return false;
    }
    // Don't allow quick transform while in the STAR tent.
    if (checkStageName("R_SP161")) {
        return false;
    }
    // The fork also required the main screen's Z button to be fully lit (as a proxy for "Midna
    // available"). This port hides the Z pane on the main screen in BOTH layouts, so the game's
    // own dim animation leaves that alpha at an arbitrary value and the proxy read false in the
    // Wii U layout. The conditions it stood for are all checked explicitly here.
    // The game will crash if trying to quick transform while holding the Ball and Chain.
    if (mEquipItem == dItemNo_IRONBALL_e) {
        return false;
    }
    if (m_midnaActor == NULL || !m_midnaActor->checkMetamorphoseEnableBase()) {
        return false;
    }
    if (mLinkAcch.ChkGroundHit() && !checkModeFlg(MODE_PLAYER_FLY) && !checkMagneBootsOn()) {
        if (checkMidnaRide()) {
            if ((checkWolf() &&
                    (checkModeFlg(MODE_UNK_1000) || dComIfGp_checkPlayerStatus0(0, 0x10))) ||
                (!checkWolf() && (checkEventRun() || getMidnaActor()->checkMetamorphoseEnable()) &&
                    (checkModeFlg(4) || dComIfGp_checkPlayerStatus0(0, 0x10))))
            {
                return true;
            }
        }
    }
    return false;
}

void daAlinkAccess::tryQuickTransform() {
    if (!checkQuickTransformOK()) {
        Z2GetAudioMgr()->seStart(Z2SE_SYS_ERROR, NULL, 0, 0, 1.0f, 1.0f, -1.0f, -1.0f, 0);
        return;
    }
    procCoMetamorphoseInit();
}

// ---------------------------------------------------------------------------
// COutFont_c::getBtiName table (upstream d_msg_out_font.cpp), by index
// ---------------------------------------------------------------------------

const char* ga::outFontBtiName(int i_nameIdx) {
    static const char* mpIconName[] = {
        "font_00.bti",
        "font_01.bti",
        "font_09.bti",
        "font_04.bti",
        "font_05.bti",
        "font_02.bti",
        "font_03.bti",
        "font_06.bti",
        "font_08.bti",
        "font_07_01.bti",
        "font_10.bti",
        "font_10.bti",
        "font_10.bti",
        "font_10.bti",
        "font_07_01.bti",
        "font_07_01.bti",
        "font_07_01.bti",
        "font_07_01.bti",
        "font_07_01.bti",
        "font_07_01.bti",
        "font_15.bti",
        "font_15.bti",
        "font_15.bti",
        "font_12.bti",
        "font_15.bti",
        "font_16_backlight.bti",
        "font_13.bti",
        "font_14.bti",
        "font_10.bti",
        "font_46.bti",
        "font_47.bti",
        "font_35.bti",
        "font_36.bti",
        "font_19.bti",
        "font_20.bti",
        "font_19.bti",
        "font_22.bti",
        "font_23.bti",
        "font_24.bti",
        "font_25.bti",
        "font_40.bti",
        "font_39.bti",
        "font_29.bti",
        "font_28.bti",
        "font_30.bti",
        "font_31.bti",
        "font_29.bti",
        "font_28.bti",
        "font_32.bti",
        "font_33.bti",
        "font_41.bti",
        "font_42.bti",
        "font_50.bti",
        "font_49.bti",
        "font_51.bti",
        "font_52.bti",
        "font_53.bti",
    };
    if (i_nameIdx >= 31 && i_nameIdx <= 40) {
        return dMeter2Info_getNumberTextureName(i_nameIdx - 31);
    }
    if (i_nameIdx < 0 || i_nameIdx >= (int)(sizeof(mpIconName) / sizeof(mpIconName[0]))) {
        return NULL;
    }
    return mpIconName[i_nameIdx];
}

// ---------------------------------------------------------------------------
// dMeter2Info_c::getStringFull (fork d_meter2_info.cpp:515-660)
// ---------------------------------------------------------------------------

namespace {

// Message tag -> COutFont_c icon index, mirroring the do_outfont() calls in dMsgString_c's draw
// processor (d_msg_class.cpp). -1 when the tag draws no icon.
int msgTagOutfontIndex(u32 i_tag) {
    switch (i_tag) {
    case MSGTAG_RED_TARGET: return 20;
    case MSGTAG_YELLOW_TARGET: return 21;
    case MSGTAG_WHITE_TARGET: return 24;
    case MSGTAG_ABTN_STAR: return 23;
    case MSGTAG_WARP_ICON: return 25;
    case MSGTAG_BOMB_BAG_ICON: return 41;
    case MSGTAG_HEART: return 27;
    case MSGTAG_QUAVER: return 28;
    case MSGTAG_GROUP(6) | MSGTAG_BULLET: return 42;
    case MSGTAG_GROUP(6) | MSGTAG_BULLET_SPACE: return 43;
    default: break;
    }
    if (i_tag >= 10 && i_tag <= 29) {
        return (int)i_tag - 10;
    }
    return -1;
}

struct dMeter2InfoAccess : dMeter2Info_c {
    void getStringFull(u32 i_stringID, char* o_string, int i_cap, int i_xyButton);
};

// Full-fidelity plain-text fetch for the companion reader: no 0x200 source cap, resolves the
// player/horse-name tags, keeps ruby base text, and emits inline icon markers (0x02 followed by
// outfont index + 1) for the controller-button tags; every other tag is skipped whole.
void dMeter2InfoAccess::getStringFull(u32 i_stringID, char* o_string, int i_cap, int i_xyButton) {
    if (i_cap <= 0) {
        return;
    }
    o_string[0] = '\0';

    u8* msgRes;
    if (mMsgResource == NULL) {
        msgRes = (u8*)JKRGetTypeResource('ROOT', "zel_00.bmg", dComIfGp_getMsgDtArchive(0));
        if (msgRes == NULL) {
            return;
        }
    } else {
        msgRes = (u8*)mMsgResource;
    }

    JMSMesgInfo_c* bmg_inf = (JMSMesgInfo_c*)(msgRes + sizeof(bmg_header_t));
    u8* bmg_data = (u8*)bmg_inf + bmg_inf->header.size;
    u8* string_data = bmg_data + sizeof(bmg_section_t);

    for (u16 i = 0; i < bmg_inf->entry_num; i++) {
        if (i_stringID != bmg_inf->entries[i].message_id) {
            continue;
        }
        const u8* p = string_data + bmg_inf->entries[i].string_offset;
        int n = 0;
        int rubySkip = 0;
        while (*p != '\0' && n < i_cap - 1) {
            if (*p == 0x1A) {
                const int size = p[1];
                if (size < 5) {
                    break;  // malformed
                }
                const u32 tag = ((u32)p[2] << 16) | ((u32)p[3] << 8) | p[4];
                if (tag == 0xFF0002 || tag == 0xFFFF02) {
                    for (int k = 0; k < size - 6 && n < i_cap - 1; k++) {
                        o_string[n++] = (char)p[6 + k];
                    }
                    rubySkip = p[5] * 2;
                } else if (tag == 0) {
                    const char* name = dComIfGs_getPlayerName();
                    while (*name != '\0' && n < i_cap - 1) {
                        o_string[n++] = *name++;
                    }
                } else if (tag == 34) {
                    const char* name = dComIfGs_getHorseName();
                    while (*name != '\0' && n < i_cap - 1) {
                        o_string[n++] = *name++;
                    }
                } else if (msgTagOutfontIndex(tag) >= 0 && n < i_cap - 2) {
                    o_string[n++] = 0x02;
                    o_string[n++] = (char)(msgTagOutfontIndex(tag) + 1);
                } else if ((tag == 46 || tag == 47) && n < i_cap - 2) {
                    // X-or-Y button, picked by where the item is equipped; i_xyButton 0 X, 1 Y.
                    const int icon = (tag == 46) == (i_xyButton == 0) ? 5 : 6;
                    o_string[n++] = 0x02;
                    o_string[n++] = (char)(icon + 1);
                } else if (tag == 55 || tag == 56) {
                    // MSGTAG_BOMB_MAX / ARROW_MAX with their localized unit (unit 7 bombs, 0 arrows).
                    constexpr int UNIT_ARROWS = 0;
                    constexpr int UNIT_BOMBS = 7;
                    int value;
                    int unit;
                    if (tag == 55) {
                        u8 bombType = dItemNo_NORMAL_BOMB_e;
                        if (size >= 6 && p[5] == 1) {
                            bombType = dItemNo_WATER_BOMB_e;
                        } else if (size >= 6 && p[5] == 2) {
                            bombType = dItemNo_POKE_BOMB_e;
                        }
                        value = dComIfGs_getBombMax(bombType);
                        unit = UNIT_BOMBS;
                    } else {
                        value = dComIfGs_getArrowMax();
                        unit = UNIT_ARROWS;
                    }
                    char num[40];
                    num[0] = '\0';
                    dMsgUnit_setTag(unit, value, TEXT_SPAN(num));
                    for (const char* c = num; *c != 0 && n < i_cap - 1; c++) {
                        o_string[n++] = *c;
                    }
                }
                p += size;
                continue;
            }
            if (rubySkip > 0) {
                rubySkip--;
                p++;
                continue;
            }
            o_string[n++] = (char)*p++;
        }
        o_string[n] = '\0';
        if (mMsgResource == NULL) {
            dComIfGp_getMsgDtArchive(0)->removeResourceAll();
        }
        return;
    }

    if (mMsgResource == NULL) {
        dComIfGp_getMsgDtArchive(0)->removeResourceAll();
    }
}

}  // namespace

void dMeter2Info_getStringFull(u32 i_stringID, char* o_string, int i_cap, int i_xyButton) {
    static_cast<dMeter2InfoAccess*>(&g_meter2_info)
        ->getStringFull(i_stringID, o_string, i_cap, i_xyButton);
}
