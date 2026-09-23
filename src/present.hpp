#pragma once

#include <cstdint>
#include <webgpu/webgpu.h>

struct ANativeWindow;

// Owns the bottom-screen WGPUSurface + GfxService present target and blits a source texture
// (the companion's resolved offscreen pass) into it with aspect-fit letterboxing and a dim.
namespace dsc::present {

// Game thread. Takes ownership of `window` (an acquired ANativeWindow reference); null drops
// the current target.
void set_native_window(ANativeWindow* window, uint32_t width, uint32_t height);
bool has_target();
// Thread-safe view of has_target() for the UI-thread surface callback.
bool target_active();
// Game thread, once per frame from a GfxService stage callback. `source` may be null (clear
// only, in clearColor); when non-null it must stay valid for the frame (the host records the
// blit into the frame encoder). dim: 0 lit .. 1 black.
void push_frame(WGPUTextureView source, uint32_t sourceWidth, uint32_t sourceHeight, float dim,
    const float clearColor[3] = nullptr);
// Game thread: re-registers a lost target; call once per frame outside the stage callback.
void update();
void shutdown();

}  // namespace dsc::present
