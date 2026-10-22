# tc2vr

A VR mod for **The Crew 2** (offline mode). It renders the game in stereo
through OpenXR, with head tracking, a curved virtual screen for menus, and a
desktop mirror for recording. It loads as a `dinput8.dll` proxy, so the game
files are left untouched.

## Status

| Area | State |
|---|---|
| Stereo rendering (two eye views + desktop view) | working |
| Full scene in every view (buildings, trees, props) | working |
| 60 fps with all three views | working |
| OpenXR session, head tracking, per-eye output | working under Proton |
| Curved screen while paused or in menus | working under Proton |
| Desktop mirror of the left eye | working |
| Native Windows | untested |
| Controller steering, hand IK | not started |

## Requirements

* The Crew 2 (Steam), offline mode.
* An OpenXR runtime: SteamVR, Oculus, WMR or Monado on Windows, or Proton's
  built-in `wineopenxr` with SteamVR on Linux.
* Without a headset the mod still runs: the eye views render at 1280x720 and
  can be inspected with the dump keys.

## Install

1. Copy `dinput8.dll` next to `TheCrew2.exe`.
2. **Proton only:** set the Steam launch options to
   `WINEDLLOVERRIDES="dinput8=n,b" %command%` so Wine loads the mod instead of
   its own `dinput8`.
3. Start the game with the headset connected and the OpenXR runtime running.

To remove the mod, delete `dinput8.dll`.

## Using it

* **Driving:** you see the game in stereo through the headset. Look around
  freely; the view follows your head on top of the game camera.
* **Paused, menus, loading:** the game's normal image is shown on a curved
  screen 2.5 m in front of you (69 degrees wide).
* **Forward direction:** set by where the headset faces when the VR session
  starts.

### Keys

| Key | Action |
|---|---|
| F9 | Save the current eye images and the desktop image as BMP files |
| F10 | Toggle the desktop mirror (left eye shown on the monitor while driving) |

Creating an empty file `tc2vr_dump.trigger` in the game folder does the same
as F9 (useful for scripts); the mod deletes the file when it has saved.

### Output files

All in the game folder:

* `tc2vr.log`: what the mod did, plus frame timings every 120 frames.
* `tc2vr_eyeL_NN.bmp`, `tc2vr_eyeR_NN.bmp`, `tc2vr_back_NN.bmp`: dumps.

## Checking it works

Look for these lines in `tc2vr.log`:

| Line | Meaning |
|---|---|
| `hooks: camera=1 present=1` | the mod is active |
| `xr: headset found, recommended WxH per eye` | the OpenXR runtime and headset were found |
| `xr: eye targets WxH, fov X x Y deg` | the eye render size and field of view |
| `left eye created` / `right eye created` | the eye views exist |
| `scene hooks installed` | the per-view render fixes are in place |
| `ring fix: installed` | the frame-rate fix is active |
| `frame 16.67 ms (60.0 fps)` | running at the game's 60 fps cap |

If `ring fix` reports a failure, expect about 40 to 45 fps with VR active.

## How it works

The engine supports several render views per frame, but its full-scene
pipeline assumes it is the only one. The mod adds two render-to-texture views
for the eyes and fixes the per-frame state that a second and third view
would otherwise find used up:

* **Eye views:** extra scene cameras and render-to-texture viewports, built
  the same way the game builds its own. Their settings (exposure, white
  balance, fog) are copied from the main view every frame.
* **Instancing counters:** the instanced-geometry renderer hands work to job
  threads through counters that the game resets once per frame. The mod
  resets them before every view; without that, every view after the first
  is missing buildings, trees and props.
* **Culling state:** the eye views are marked as secondary views (the game's
  own flag for menu and garage views), so only the main view advances the
  culler's per-object fade state.
* **Frame rate:** the game's GPU ring allocator detours through the copy
  protection's virtual machine every time it maps a buffer, which cost about
  80% of each view's render time. The mod replaces that allocator with a
  native copy without the detour.
* **Head tracking:** just before each eye renders, the mod asks OpenXR for
  the head pose of the frame being drawn and composes it onto that frame's
  camera, then submits exactly that pose so the runtime can reproject.
* **Screen switching:** the curved screen is shown whenever the vehicle
  camera is not updating (pause, menus, loading).

Everything is done with pointer swaps in game data (a vtable slot, the
render pipeline table), except the allocator replacement, which patches one
function. Each patch checks the game's bytes first and does nothing if they
do not match this build.

## Building

Cross-compiles on Linux with MinGW:

```sh
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build
```

The output is `build/dinput8.dll`. OpenXR headers are vendored in
`third_party/openxr`; no OpenXR loader is needed, the mod loads the active
runtime directly from the registry.

## Source layout

| File | Purpose |
|---|---|
| `src/main.cpp` | `dinput8` proxy exports and start-up |
| `src/game.hpp`, `src/game.cpp` | game addresses, structure offsets, build check |
| `src/stereo.cpp` | eye view creation and per-frame camera updates |
| `src/render_hook.cpp` | wraps the full-scene build and execute for each view |
| `src/ring_fix.cpp` | native GPU ring allocator |
| `src/xr.cpp` | OpenXR runtime, session, swapchains and layers |
| `src/mirror.cpp` | desktop mirror blit |
| `src/present.cpp` | Present hook: dumps, timing, OpenXR frame, mirror |
| `docs/ENGINE.md` | reverse-engineering notes on the engine |

## Known limitations

* The eye views include the game's HUD.
* The forward direction cannot be recentered yet.
* The chase camera's pitch and roll pass through to the headset view.
* Built and tested against one Steam build of `TheCrew2.exe`; other builds
  are detected and the mod stays inactive.
