#pragma once

// Mod-side replacement for the fork's dusk settings surface: only the companion's own options,
// in-memory, mirrored to/from ConfigService by mod.cpp (see dsc::config). Same call shape as
// the fork so the ported companion sources compile unchanged.

#include <array>

// video.fpsOverlayCorner: 0-3 are screen corners, 4 means the companion screen draws the
// counter instead. The host's own overlay never knows value 4; the mod only reads it.
constexpr int kFpsCornerCompanion = 4;

// game.dualScreenHudMode: which HUD split the second screen uses.
//   Cinematic  - the whole status HUD lives on the companion.
//   Functional - the gameplay-critical HUD moves back to the main screen and the companion
//                becomes a control surface.
constexpr int kDualHudCinematic = 0;
constexpr int kDualHudFunctional = 1;

namespace dusk {

template <typename T>
struct ConfigVar {
    T value{};
    T getValue() const { return value; }
    void setValue(T v) { value = v; }
    operator T() const { return value; }
};

struct Settings {
    struct {
        // Always on: the mod's whole purpose; disable the mod instead. Still gated at runtime
        // by whether a second display is actually present.
        ConfigVar<bool> dualScreen{true};
        ConfigVar<int> dualScreenHudMode{kDualHudFunctional};
        ConfigVar<bool> dualScreenSwap{false};
        ConfigVar<int> dualScreenDisplay{-1};
        ConfigVar<int> dualScreenPosX{0};
        ConfigVar<int> dualScreenPosY{0};
        ConfigVar<bool> dualScreenHaptics{true};
        ConfigVar<bool> enableMirrorMode{false};
        ConfigVar<bool> guideEnabled{false};
    } game;
    struct {
        ConfigVar<bool> enableFpsOverlay{false};
        ConfigVar<int> fpsOverlayCorner{0};
    } video;
};

Settings& getSettings();

}  // namespace dusk

using dusk::getSettings;
