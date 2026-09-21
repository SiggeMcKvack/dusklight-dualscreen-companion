# Dusklight Dual Screen Companion

> **Note:** this mod was developed with the help of Claude (Anthropic's AI assistant): the port of
> the fork's companion code, the hook layer and the Android plumbing were written with it and
> verified on hardware.

A [Dusklight](https://github.com/TwilitRealm/dusklight) mod that puts a companion dashboard on the
second screen of dual-screen Android handhelds (developed and tested on the AYN Thor): HUD status strip, hearts,
button cluster, interactive map (pan/pinch), items, quest and collection pages, an offline walkthrough
reader with an in-app browser to fetch guides, touch item buttons, warp and transform buttons, screen
dimming in step with the main screen.

It is a port of the dual-screen companion from the [igawa6/dusklight](https://github.com/igawa6/dusklight)
fork into a **pure mod**: no changes to Dusklight itself. The fork's game-source edits are replaced by
HookService hooks, its aurora aux window by a GfxService present target on an `android.app.Presentation`,
and its Java by a DEX that the mod loads at runtime. Android only.

## Requirements

- Dusklight **v2.0.1** installed on the device (the hooks are matched to that release's binary).
- A dual-screen Android device whose second screen is a normal secondary display (as on the Thor).
  The dashboard lays itself out from the second screen's aspect ratio and renders at its native
  resolution (clamped to 320–2048 px per side); only the Thor's 8:7 panel has been tested.
- To build: Android SDK (a `build-tools` with `d8`, a `platforms/android-*`), NDK r27, a JDK 17, CMake ≥ 3.25, Ninja.

## Building

```bash
git clone --recurse-submodules=extern/dusklight <this repo>
cd dusklight-dualscreen-companion
git -C extern/dusklight submodule update --init extern/aurora   # Dawn header provider
export ANDROID_HOME=~/Library/Android/sdk
cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$ANDROID_HOME/ndk/27.2.12479018/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
# -> build/mods/dev.siggemckvack.dualscreen_companion.dusk
```

The SDK downloads a link stub for the game binary automatically. Install the `.dusk` by copying it
into Dusklight's `mods/` folder (with a shared data folder: `adb push build/mods/dev.siggemckvack.dualscreen_companion.dusk
/sdcard/Dusklight/mods/`) and enable it in the Mods panel.

## Settings (Mods panel)

- **Layout** — *Wii U style* (whole HUD on the bottom screen) or *3DS style* (hearts and A/B stay on
  the main screen, the bottom screen becomes a control surface).
- **Haptic feedback**, **FPS counter on bottom screen**, **Walkthrough guide** (+ *Get a guide* opens
  the in-app browser).

## How it works

- `src/jni_bridge.cpp` — obtains a `JavaVM` through `SDL_GetAndroidJNIEnv` (resolved from the symbol
  manifest; falls back to hooking SDL's `Java_*` entry points), loads the embedded DEX with
  `InMemoryDexClassLoader`, binds natives with `RegisterNatives`.
- `java/` — `Presentation` on the secondary display hosting a `SurfaceView` (non-focusable so the
  gamepad stays with the game), touch/pinch forwarding, battery, display hot-plug.
- `src/present.cpp` — `WGPUSurface` from the `ANativeWindow`, registered as a GfxService present target;
  aspect-fit blit with the dim as a blend constant. Surface loss blocks `surfaceDestroyed` until the
  swapchain is dropped (presenting to a dead surface is a fatal device loss).
- `src/companion/` — the fork's companion sources, ported; `dualscreen.cpp` renders the dashboard into a
  GfxService offscreen pass from the `FRAME_BEFORE_HUD` / `FRAME_AFTER_HUD` stage hooks.
- `src/include/dusk/game_access.h` + `src/game_access.cpp` — the fork's added game-class accessors on
  data-free "access" subclasses (`ga::cast(x)->method()`).
- `src/hooks.cpp` — the fork's game-source edits as pre/post hooks (HUD partition, pulses, A/B corner
  shift, meter lifecycle, contextual panels, minimap, pad injection, scene changes, gear reload pump).
- `src/slots.cpp` — slot I/II substitute (see below). `src/config.cpp` — settings + Mods panel.

## Differences from the fork

- **No swap-screens** (needs a launcher Activity). The guide reader and its in-app browser are
  ported; the import worker does not fetch images over HTTP (the browser hands them over when it
  saves a page).
- **Slot I/II** are not real item buttons 2/3 (that is a `daAlink_c` layout change across the game).
  A slot press temporarily parks the slot's item on X (slot I) / Y (slot II) and holds that button;
  the original binding is restored on release. X/Y show the slot's item while held.
- Switching **Layout** mid-game re-partitions immediately, but the A/B buttons keep their previous
  spot until the game next redraws them.
- The vessel-of-light glow animates per frame rather than per game tick.

## License

CC0, like Dusklight and the fork it is ported from. The companion code is igawa6's work.
