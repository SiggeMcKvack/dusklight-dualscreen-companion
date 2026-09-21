#pragma once
#include <cstdint>

// Slot I/II substitute. The fork made the companion's two slot buttons first-class item buttons
// 2/3 by widening daAlink_c's button masks — a class-layout change across the game that a mod
// cannot make. Here a slot press temporarily puts the slot's item on X (slot I) or Y (slot II)
// and holds that pad button for as long as the slot button is held; the binding is restored on
// release. Visible difference: X/Y show the slot's item while a slot button is held.
namespace dsc::slots {
// Game thread, after companion::beginFrameCompanionInput().
void update();
// Pad button bits (PAD_BUTTON_X / PAD_BUTTON_Y) to hold this frame for the pad-read hook.
uint32_t pad_hold_mask();
void shutdown();
}
