#pragma once
// The fork's "Use Slot" physical binds need host action binds, which mods cannot add; the
// companion's slot buttons are touch-only here.
namespace dusk {
enum class ActionBinds { USE_SLOT_ITEM_1, USE_SLOT_ITEM_2 };
inline bool getActionBindHold(ActionBinds, int) { return false; }
}
