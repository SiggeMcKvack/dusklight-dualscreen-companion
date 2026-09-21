#pragma once
namespace dsc::hooks {
// Game thread, from mod_initialize. The loader removes the hooks on unload.
bool install();
}
