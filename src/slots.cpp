#include "slots.hpp"

#include "dusk/companion.h"

#include "d/d_com_inf_game.h"
#include "d/d_item.h"
#include "d/d_save.h"
#include "dolphin/pad.h"

namespace dsc::slots {
namespace {

// Slot I <-> X (index 0), slot II <-> Y (index 1); the slot's binding lives in select index
// 2 + slot.
int g_holdBtn = -1;  // X/Y currently held on a slot's behalf
// Temporary borrow (Ooccoo only): the X/Y binding to put back once her warp has started.
int g_borrowBtn = -1;
u8 g_borrowSelect = 0xFF;
u8 g_borrowMix = 0xFF;
int g_restoreFrames = -1;

bool is_ooccoo(u8 item) {
    return item == dItemNo_DUNGEON_EXIT_e || item == dItemNo_DUNGEON_BACK_e;
}

void restore_borrow() {
    if (g_borrowBtn < 0) {
        return;
    }
    dComIfGs_setSelectItemIndex(g_borrowBtn, g_borrowSelect);
    dComIfGs_setMixItemIndex(g_borrowBtn, g_borrowMix);
    dComIfGp_setSelectItem(g_borrowBtn);
    g_borrowBtn = -1;
    g_restoreFrames = -1;
}

void press(int slot) {
    const u8 slotSel = dComIfGs_getSelectItemIndex(2 + slot);
    if (slotSel >= MAX_ITEM_SLOTS) {
        return;  // empty slot; the companion shows its own "no slot" message
    }
    const int btn = slot;  // X for slot I, Y for slot II
    const u8 btnSel = dComIfGs_getSelectItemIndex(btn);
    const u8 btnMix = dComIfGs_getMixItemIndex(btn);
    const u8 slotMix = dComIfGs_getMixItemIndex(2 + slot);

    if (is_ooccoo(dComIfGs_getItem(slotSel, false))) {
        // Borrow: the companion's quick-use restores the SLOT binding itself and expects the
        // press to leave no trace, so the button binding goes back after the warp event starts.
        g_borrowBtn = btn;
        g_borrowSelect = btnSel;
        g_borrowMix = btnMix;
        g_restoreFrames = -1;
        dComIfGs_setSelectItemIndex(btn, slotSel);
        dComIfGs_setMixItemIndex(btn, slotMix);
        dComIfGp_setSelectItem(btn);
    } else {
        // Exchange: the slot's item moves onto the button and STAYS there, so items that are
        // worn while equipped (iron boots, a held boomerang, the lantern) are not unequipped by
        // a binding change on release. The button's previous item takes the slot.
        dComIfGs_setSelectItemIndex(btn, slotSel);
        dComIfGs_setMixItemIndex(btn, slotMix);
        dComIfGs_setSelectItemIndex(2 + slot, btnSel);
        dComIfGs_setMixItemIndex(2 + slot, btnMix);
        dComIfGp_setSelectItem(btn);
        dComIfGp_setSelectItem(2 + slot);
    }
    g_holdBtn = btn;
    dusk::companion::setSlotParked(slot);
}

}  // namespace

void update() {
    const uint32_t trig = dusk::companion::slotTriggerBits();
    const uint32_t hold = dusk::companion::slotHoldBits();

    if (g_holdBtn < 0) {
        for (int slot = 0; slot < 2 && g_holdBtn < 0; slot++) {
            if (trig & (1u << slot)) {
                press(slot);
            }
        }
    }
    if (g_holdBtn >= 0 && (hold & (1u << g_holdBtn)) == 0) {
        // Released.
        dusk::companion::setSlotParked(-1);
        if (g_borrowBtn >= 0) {
            g_restoreFrames = 0;  // Ooccoo: wait for the warp event (or a cap)
        }
        g_holdBtn = -1;
    }
    if (g_restoreFrames >= 0) {
        g_restoreFrames++;
        if (g_restoreFrames >= 12 && (dComIfGp_event_runCheck() || g_restoreFrames >= 180)) {
            restore_borrow();
        }
    }
}

uint32_t pad_hold_mask() {
    const uint32_t hold = dusk::companion::slotHoldBits();
    if (g_holdBtn == 0 && (hold & 1u)) {
        return PAD_BUTTON_X;
    }
    if (g_holdBtn == 1 && (hold & 2u)) {
        return PAD_BUTTON_Y;
    }
    return 0;
}

void shutdown() {
    dusk::companion::setSlotParked(-1);
    restore_borrow();
}

}  // namespace dsc::slots
