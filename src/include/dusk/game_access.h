#pragma once

// The fork adds accessor methods to game classes (dMeter2Draw_c, dMenu_Fmap_c, ...). A mod cannot
// edit those headers, but every member the companion needs is public or protected in the decomp,
// so the same methods live here on "access" subclasses that add no data and no virtuals. The
// companion reaches them through ga::cast(ptr)->method(): the object really is the base type,
// which is fine in practice for a data-free non-virtual derived view on this ABI.

#include "JSystem/J2DGraph/J2DPicture.h"
#include "d/actor/d_a_alink.h"
#include "d/d_map.h"
#include "d/d_menu_collect.h"
#include "d/d_menu_dmap.h"
#include "d/d_menu_fmap.h"
#include "d/d_menu_fmap2D.h"
#include "d/d_menu_window.h"
#include "d/d_meter2.h"
#include "d/d_meter2_draw.h"
#include "d/d_meter2_info.h"
#include "d/d_meter_map.h"
#include "d/d_msg_out_font.h"

struct dMeter2DrawAccess : dMeter2Draw_c {
    static const int VESSEL_ALPHA_SAVE_COUNT = 35;
    const ResTIMG* getActionLabelTimg(int i_which);  // 0 = A, 1 = B
    J2DPane* getButtonPane(int i_which);             // 0 = A, 1 = B, 2 = X, 3 = Y, 4 = Z
    J2DPicture* getButtonBasePicture(int i_which);
    const char* getActionTextA();
    const char* getActionTextB();
    const char* getActionTextXY(int i_no);
    J2DPane* getMidnaButtonPaneRaw();
    J2DPicture* getLightDropPicture();
    const char* getDpadLabel(int i_no);
    void drawMidnaPikariAt(f32 i_posX, f32 i_posY);
    J2DPane* getButtonCrossPane();
    J2DPane* getLightDropPane();
    bool isItemUsable(int i_xy);
    bool isOxygenActive();
    J2DPane* getVesselTearPane(int i_idx);
    void drawVesselPikariForCompanion(const f32* i_tearX, const f32* i_tearY, f32 i_sizeScale);
    void refreshVesselForCompanion();
    void pushVesselStateForCompanion(f32* o_alpha, f32* o_x, f32* o_y, f32* o_scale);
    void popVesselStateForCompanion(
        const f32* i_alpha, const f32* i_x, const f32* i_y, const f32* i_scale);
    dKantera_icon_c* getKanteraMeter(int i_no);
    void forceCompanionAlpha();
    bool isButtonClusterVisible();
    J2DPane* getCounterPane(int i_which);  // 0 = rupees, 1 = keys
    int getHeartPictures(int i_no, J2DPicture** o_pics);
    f32 getLightDropAlpha();

    // Per-frame usability snapshot for X/Y (+ slots), taken by the mod's post-hook on
    // setButtonIconAlpha (the live dMeter2Info bits are reset before the companion draws).
    static bool sItemUsable[4];

private:
    CPaneMgr* getButtonMgr(int i_which);  // same indexing as getButtonPane
    void collectVesselPanes(CPaneMgr** o_panes);  // VESSEL_ALPHA_SAVE_COUNT entries
};

struct dMenuFmapAccess : dMenu_Fmap_c {
    dMenu_Fmap2DTop_c* getDraw2DTop() { return mpDraw2DTop; }
    bool isWarpMapMode() const { return mIsWarpMap; }
};

struct dMenuDmapAccess : dMenu_Dmap_c {
    dMenu_DmapBg_c* getDrawBg() { return mpDrawBg; }
};

struct dMenuDmapBgAccess : dMenu_DmapBg_c {
    const char* getButtonLabel(int i_which);  // 0 = A, 1 = B; empty if hidden
};

struct dMenuFmap2DTopAccess : dMenu_Fmap2DTop_c {
    const char* getButtonLabel(int i_which);  // 0 = A, 1 = B; NULL if hidden
};

struct dMenuWindowAccess : dMw_c {
    dMenu_Fmap_c* getMenuFmap() { return mpMenuFmap; }
    dMenu_Dmap_c* getMenuDmap() { return mpMenuDmap; }
    dMenu_Collect_c* getMenuCollect() { return mpMenuCollect; }
};

struct dMenuCollectAccess : dMenu_Collect_c {
    dMenu_Collect2D_c* getCollect2D() { return mpCollect2D; }
};

struct dMenuCollect2DAccess : dMenu_Collect2D_c {
    // Category icon panes: 0 = fish journal, 1 = hidden skills scroll, 2 = letters,
    // 3-7 = scent icons (medicine, children, fish, youth's/Ilia, poe).
    J2DPane* getIconPane(int i_which) {
        if (mpScreen == NULL) {
            return NULL;
        }
        static const u64 tags[8] = {
            MULTI_CHAR('fish_3_n'), MULTI_CHAR('maki_5_n'), MULTI_CHAR('lett_4_n'),
            MULTI_CHAR('wolf_med'), MULTI_CHAR('wolf_chi'), MULTI_CHAR('wolf_fis'),
            MULTI_CHAR('wolf_iri'), MULTI_CHAR('wolf_pou'),
        };
        if (i_which < 0 || i_which >= 8) {
            return NULL;
        }
        return mpScreen->search(tags[i_which]);
    }
};

struct dMeter2Access : dMeter2_c {
    dMeterSub_c* getSubContentsDlst() { return mpSubContents; }
    dMeterString_c* getSubSubContentsDlst() { return mpSubSubContents; }
};

struct dMeterMapAccess : dMeterMap_c {
    dMap_c* getDMap() { return mMap; }
};

struct dMapAccess : dMap_c {
    u16 getTexSizeX() const { return mTexSizeX; }
    f32 getCenterX() const { return mCenterX; }
};

struct J2DPictureAccess : J2DPicture {
    u32 getCornerColorRaw(int i_no) const { return mCornerColor[i_no]; }
};

struct daAlinkAccess : daAlink_c {
    // Quick-transform availability, side-effect free (no sound, no pad writes).
    bool checkQuickTransformOK();
    // Transform now if allowed, error beep otherwise. Game-thread frame context only.
    void tryQuickTransform();
};

namespace ga {

inline dMeter2DrawAccess* cast(dMeter2Draw_c* p) { return static_cast<dMeter2DrawAccess*>(p); }
inline dMenuFmapAccess* cast(dMenu_Fmap_c* p) { return static_cast<dMenuFmapAccess*>(p); }
inline dMenuDmapAccess* cast(dMenu_Dmap_c* p) { return static_cast<dMenuDmapAccess*>(p); }
inline dMenuDmapBgAccess* cast(dMenu_DmapBg_c* p) { return static_cast<dMenuDmapBgAccess*>(p); }
inline dMenuFmap2DTopAccess* cast(dMenu_Fmap2DTop_c* p) {
    return static_cast<dMenuFmap2DTopAccess*>(p);
}
inline dMenuWindowAccess* cast(dMw_c* p) { return static_cast<dMenuWindowAccess*>(p); }
inline dMenuCollectAccess* cast(dMenu_Collect_c* p) { return static_cast<dMenuCollectAccess*>(p); }
inline dMenuCollect2DAccess* cast(dMenu_Collect2D_c* p) {
    return static_cast<dMenuCollect2DAccess*>(p);
}
inline dMeter2Access* cast(dMeter2_c* p) { return static_cast<dMeter2Access*>(p); }
inline dMeterMapAccess* cast(dMeterMap_c* p) { return static_cast<dMeterMapAccess*>(p); }
inline dMapAccess* cast(dMap_c* p) { return static_cast<dMapAccess*>(p); }
inline J2DPictureAccess* cast(J2DPicture* p) { return static_cast<J2DPictureAccess*>(p); }
inline daAlinkAccess* cast(daAlink_c* p) { return static_cast<daAlinkAccess*>(p); }

// COutFont_c::getBtiName reads no members; the fork made it static. Same table, by index.
const char* outFontBtiName(int i_nameIdx);

}  // namespace ga

// Full-fidelity plain-text fetch (fork's dMeter2Info_c::getStringFull), see game_access.cpp.
void dMeter2Info_getStringFull(u32 i_stringID, char* o_string, int i_cap, int i_xyButton = 0);
