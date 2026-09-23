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
- **Slot I is a real item button; slot II is routed through Y.** Slot I uses save select index 2,
  whose button bit (`BTN_Z`) no item code reads, so the mod injects it after the pad is sampled and
  teaches the small item-button functions in `daAlink_c` about index 2 — the item equips, holds and
  fires like one on X or Y, and the binding lives in the save. (Thanks to the reporter of issue #1
  and to [OTPR26/twilight-hd-hud](https://github.com/OTPR26/twilight-hd-hud), which does the same
  for its third slot. Note that both mods would claim index 2, so they cannot both own it.)
  Slot II has no equivalent: index 3's bit is `BTN_B`, which the game really uses. A slot II press
  therefore *exchanges* the slot's item with the Y button, presses Y and leaves the item there
  (restoring the binding on release would unequip worn items); the item that was on Y takes the
  slot, and pressing again swaps back. The fork's four real item buttons need wider button masks
  and a renumbered bit enum in `daAlink_c` — that part is still a game change, not a mod one.
- The vessel-of-light glow animates per frame rather than per game tick.

## License

CC0, like Dusklight and the fork it is ported from. The companion code is igawa6's work.
