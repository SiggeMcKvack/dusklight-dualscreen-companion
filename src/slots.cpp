#include "slots.hpp"

#include "dusk/companion.h"

#include "d/d_com_inf_game.h"
#include "d/d_item.h"
#include "d/d_save.h"
#include "dolphin/pad.h"

namespace dsc::slots {
namespace {

int g_swapBtn = -1;  // 0 = X (slot I), 1 = Y (slot II) while a slot's item is parked there
u8 g_savedSelect = 0xFF;
u8 g_savedMix = 0xFF;
u8 g_swappedItem = dItemNo_NONE_e;
int g_restoreFrames = -1;  // >= 0: release seen, restore pending (Ooccoo needs the event to start)

void restore() {
    if (g_swapBtn < 0) {
        return;
    }
    dComIfGs_setSelectItemIndex(g_swapBtn, g_savedSelect);
    dComIfGs_setMixItemIndex(g_swapBtn, g_savedMix);
    dComIfGp_setSelectItem(g_swapBtn);
    g_swapBtn = -1;
    g_restoreFrames = -1;
}

void swap_in(int slot) {
    const u8 idx = dComIfGs_getSelectItemIndex(2 + slot);
    if (idx >= MAX_ITEM_SLOTS) {
        return;  // empty slot; the companion shows its own "no slot" message
    }
    g_savedSelect = dComIfGs_getSelectItemIndex(slot);
    g_savedMix = dComIfGs_getMixItemIndex(slot);
    g_swappedItem = dComIfGs_getItem(idx, false);
    dComIfGs_setSelectItemIndex(slot, idx);
    dComIfGs_setMixItemIndex(slot, dComIfGs_getMixItemIndex(2 + slot));
    dComIfGp_setSelectItem(slot);
    g_swapBtn = slot;
    g_restoreFrames = -1;
}

}  // namespace

void update() {
    const uint32_t trig = dusk::companion::slotTriggerBits();
    const uint32_t hold = dusk::companion::slotHoldBits();

    if (g_swapBtn < 0) {
        for (int slot = 0; slot < 2 && g_swapBtn < 0; slot++) {
            if (trig & (1u << slot)) {
                swap_in(slot);
            }
        }
    }
    if (g_swapBtn >= 0 && g_restoreFrames < 0 && (hold & (1u << g_swapBtn)) == 0) {
        // Released. Ooccoo's warp init reads which button carries her well after the press
        // (procDungeonWarpReadyInit waits for the event grant), so keep her on the button until
        // the event is running, with a cap for a use the game refused outright.
        if (g_swappedItem == dItemNo_DUNGEON_EXIT_e || g_swappedItem == dItemNo_DUNGEON_BACK_e) {
            g_restoreFrames = 0;
        } else {
            restore();
        }
    }
    if (g_restoreFrames >= 0) {
        g_restoreFrames++;
        if (g_restoreFrames >= 12 && (dComIfGp_event_runCheck() || g_restoreFrames >= 180)) {
            restore();
        }
    }
}

uint32_t pad_hold_mask() {
    const uint32_t hold = dusk::companion::slotHoldBits();
    uint32_t mask = 0;
    if (g_swapBtn == 0 && (hold & 1u)) {
        mask |= PAD_BUTTON_X;
    }
    if (g_swapBtn == 1 && (hold & 2u)) {
        mask |= PAD_BUTTON_Y;
    }
    return mask;
}

void shutdown() {
    restore();
}

}  // namespace dsc::slots
