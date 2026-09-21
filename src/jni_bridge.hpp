#pragma once

#include <cstddef>
#include <cstdint>

struct ANativeWindow;

// Bridge between the companion's Java side (Presentation + SurfaceView, loaded from an embedded
// DEX) and the mod. UI-thread natives only hand data off; the game thread pulls it here.
namespace dsc::jni {

struct SurfaceChange {
    ANativeWindow* window;  // acquired reference (owned by the taker); null = surface lost
    uint32_t width;
    uint32_t height;
};

struct TouchEvent {
    int action;  // 0 down, 1 move, 2 up, 3 cancel (pinch took over)
    float u;
    float v;
};

// Game thread. Tries the direct route (SDL_GetAndroidJNIEnv via the symbol manifest); if that
// is unavailable, installs hooks on SDL's JNI entry points to capture the JavaVM from the UI
// thread. Returns false only on hard failure.
bool init(uint32_t panelWidth, uint32_t panelHeight);
// Game thread, once per frame: completes the bootstrap once a JavaVM is known.
void update();
// Game thread: dismisses the Presentation synchronously, unbinds natives, drops refs.
void shutdown();

bool is_bootstrapped();

// Game thread: device vibrator pulse (CompanionManager.vibrate); amplitude 0..1.
void vibrate(uint32_t durationMs, float amplitude);

// Game thread: show the guide browser overlay (GuideBrowser.open, which hops to the UI thread).
bool open_guide_browser(const char* url);

// Pending surface change since the last call (the taker owns the returned window reference).
bool take_surface_change(SurfaceChange& out);
// Game thread: call once the change from take_surface_change has been applied. A surface loss
// blocks the UI thread's surfaceDestroyed until this (bounded), so the swapchain is torn down
// before Android destroys the window underneath it (presenting to a dead surface is a Vulkan
// device loss, which the host treats as fatal).
void ack_surface_change();
size_t drain_touch(TouchEvent* out, size_t max);
// Multiplicative pinch factor accumulated since the last call (1.0 when none).
float take_pinch();
bool display_available();
int battery_percent();  // negative = unknown
bool battery_charging();

}  // namespace dsc::jni
