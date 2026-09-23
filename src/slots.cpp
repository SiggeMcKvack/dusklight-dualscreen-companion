#include "slots.hpp"

#include "dusk/companion.h"

#include "d/d_com_inf_game.h"
#include "d/d_item.h"
#include "d/d_save.h"
#include "dolphin/pad.h"

namespace dsc::slots {
namespace {

// Slot I (save select index 2) is a real item button: hooks.cpp injects its BTN_Z bit after the
// pad is sampled and teaches the item-button functions about index 2, so nothing is needed here.
//
// Slot II (index 3) has no free mask bit of its own -- 1 << 3 is BTN_B, which is live item code --
// so it still routes through the Y button: a press exchanges the slot's item with Y's and leaves
// it there (restoring the binding on release would unequip worn items), and the button's previous
// item takes the slot. Ooccoo is the exception: her quick-use restores the slot binding itself, so
// the Y binding is put back once the warp has started.
constexpr int kSlotII = 1;
constexpr int kButtonY = SELECT_ITEM_Y;
constexpr int kSlotSelectIndex = SELECT_ITEM_DOWN;  // slot I

bool g_slotIIHeld = false;
bool g_borrowed = false;
u8 g_borrowSelect = 0xFF;
u8 g_borrowMix = 0xFF;
int g_restoreFrames = -1;

bool is_ooccoo(u8 item) {
    return item == dItemNo_DUNGEON_EXIT_e || item == dItemNo_DUNGEON_BACK_e;
}

void restore_borrow() {
    if (!g_borrowed) {
        return;
    }
    dComIfGs_setSelectItemIndex(kButtonY, g_borrowSelect);
    dComIfGs_setMixItemIndex(kButtonY, g_borrowMix);
    dComIfGp_setSelectItem(kButtonY);
    g_borrowed = false;
    g_restoreFrames = -1;
}

void press_slot_ii() {
    const u8 slotSel = dComIfGs_getSelectItemIndex(kSlotSelectIndex + 1);
    if (slotSel >= MAX_ITEM_SLOTS) {
        return;  // empty slot; the companion shows its own "no slot" message
    }
    const u8 btnSel = dComIfGs_getSelectItemIndex(kButtonY);
    const u8 btnMix = dComIfGs_getMixItemIndex(kButtonY);
    const u8 slotMix = dComIfGs_getMixItemIndex(kSlotSelectIndex + 1);

    dComIfGs_setSelectItemIndex(kButtonY, slotSel);
    dComIfGs_setMixItemIndex(kButtonY, slotMix);
    if (is_ooccoo(dComIfGs_getItem(slotSel, false))) {
        g_borrowed = true;
        g_borrowSelect = btnSel;
        g_borrowMix = btnMix;
        g_restoreFrames = -1;
    } else {
        dComIfGs_setSelectItemIndex(kSlotSelectIndex + 1, btnSel);
        dComIfGs_setMixItemIndex(kSlotSelectIndex + 1, btnMix);
        dComIfGp_setSelectItem(kSlotSelectIndex + 1);
    }
    dComIfGp_setSelectItem(kButtonY);
    g_slotIIHeld = true;
    dusk::companion::setSlotParked(kSlotII);
}

}  // namespace

void update() {
    const uint32_t trig = dusk::companion::slotTriggerBits();
    const uint32_t hold = dusk::companion::slotHoldBits();

    if (!g_slotIIHeld && (trig & (1u << kSlotII))) {
        press_slot_ii();
    }
    if (g_slotIIHeld && (hold & (1u << kSlotII)) == 0) {
        dusk::companion::setSlotParked(-1);
        if (g_borrowed) {
            g_restoreFrames = 0;  // Ooccoo: wait for the warp event (or a cap)
        }
        g_slotIIHeld = false;
    }
    if (g_restoreFrames >= 0) {
        g_restoreFrames++;
        if (g_restoreFrames >= 12 && (dComIfGp_event_runCheck() || g_restoreFrames >= 180)) {
            restore_borrow();
        }
    }
}

uint32_t pad_hold_mask() {
    // Slot I presses reach Link as BTN_Z from the setStickData hook, not through the pad.
    const uint32_t hold = dusk::companion::slotHoldBits();
    return (g_slotIIHeld && (hold & (1u << kSlotII))) ? PAD_BUTTON_Y : 0u;
}

void shutdown() {
    dusk::companion::setSlotParked(-1);
    restore_borrow();
    g_slotIIHeld = false;
}

}  // namespace dsc::slots
