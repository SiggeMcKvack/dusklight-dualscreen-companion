#pragma once
#include <cstdint>

// Slot I/II substitute. The fork made the companion's two slot buttons first-class item buttons
// 2/3 by widening daAlink_c's button masks — a class-layout change across the game that a mod
// cannot make. Here a slot press EXCHANGES the slot's item with X (slot I) / Y (slot II) and
// holds that pad button while the slot button is held; the item stays on the button afterwards
// (a binding change on release would unequip worn items), and the button's previous item takes
// the slot. Pressing again swaps back. Ooccoo is the exception: her quick-use borrows a slot and
// restores it itself, so for her the button binding is put back once the warp has started.
namespace dsc::slots {
// Game thread, after companion::beginFrameCompanionInput().
void update();
// Pad button bits (PAD_BUTTON_X / PAD_BUTTON_Y) to hold this frame for the pad-read hook.
uint32_t pad_hold_mask();
void shutdown();
}
