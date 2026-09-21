#pragma once
namespace dsc::config {
// Registers the mod's config vars (persisted by the host), mirrors them into dusk::getSettings()
// and builds the Mods-panel controls. Game thread, from mod_initialize.
bool init();
}
