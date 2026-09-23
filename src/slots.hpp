#pragma once
#include <cstdint>

// Companion slot buttons.
//
// Slot I (save select index 2) is a real item button -- see the hooks in hooks.cpp; nothing in
// this file drives it. Slot II (index 3) has no free mask bit of its own, so a press exchanges the
// slot's item with the Y button, presses Y, and leaves the item there (restoring the binding on
// release would unequip worn items); the item that was on Y takes the slot, and pressing again
// swaps back. Ooccoo's quick-use restores the slot binding itself, so for her the Y binding is put
// back once the warp has started.
namespace dsc::slots {
// Game thread, after companion::beginFrameCompanionInput().
void update();
// Pad button bits (PAD_BUTTON_X / PAD_BUTTON_Y) to hold this frame for the pad-read hook.
uint32_t pad_hold_mask();
void shutdown();
}
