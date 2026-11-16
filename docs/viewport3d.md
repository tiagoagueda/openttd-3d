# Experimental 3D view

This fork adds a second, 3D view of the map next to the normal 2D game.
The 2D game, its GUI, the simulation and multiplayer are unchanged: the 3D view only reads the map.

Status: milestone 0. The window shows the terrain as a lit, coloured mesh with a free camera.
There are no textures, buildings, vehicles or tracks in it yet; tiles with those on them are drawn as flat coloured tiles.

## Using it

1. Start or load a game.
2. Open the console (the key left of `1`, or the `?` menu), and type `view3d`.

A window opens that looks at the same spot as the main view.
The status bar of the window shows which graphics API is in use.
To force one, type `view3d vulkan` or `view3d opengl`.

| Action | Effect |
| --- | --- |
| Drag with the left mouse button | Move over the map |
| Drag with the right mouse button | Rotate and tilt the camera |
| Mouse wheel | Zoom |

Changes to the map, such as terraforming, show up in the 3D view right away.

## Building

The 3D view is built by default and needs nothing installed beyond what OpenTTD already needs, plus `git`.
While configuring, CMake downloads [bgfx](https://github.com/bkaradzic/bgfx) (through [bgfx.cmake](https://github.com/bkaradzic/bgfx.cmake)) and builds it with the game. This needs internet access the first time and adds a few minutes to the first build.

Build as described in [COMPILING.md](../COMPILING.md). To leave the 3D view out, configure with `-DOPTION_VIEWPORT3D=OFF`.

On Windows, start the game with `run-openttd-3d.cmd` from the top of the checkout rather than by double-clicking `openttd.exe`.
The game looks for its language files in the working directory, which has to be the `build` folder; started from anywhere else it stops with "No available language packs".

On Linux, bgfx also needs the OpenGL and X11 development files, for example `libgl1-mesa-dev libegl1-mesa-dev libx11-dev` on Debian and Ubuntu.

To build without downloading, point CMake at local checkouts:

```
-DFETCHCONTENT_SOURCE_DIR_BGFX_CMAKE=/path/to/bgfx.cmake -DBX_DIR=/path/to/bx -DBIMG_DIR=/path/to/bimg -DBGFX_DIR=/path/to/bgfx
```

## How it works

All code lives in `src/viewport3d/`:

| File | Purpose |
| --- | --- |
| `view3d_gui.cpp` | The window, the camera controls, and deciding which parts of the terrain to (re)build and draw. |
| `terrain_mesh.cpp` | Turns the map array into triangles, in chunks of 64x64 tiles. Knows nothing about the GPU. |
| `renderer3d.cpp` | The only file that uses bgfx. Draws the chunks into an offscreen image and reads the pixels back. |
| `camera3d.h` | Orbit camera, matrices and frustum culling. |
| `shaders/` | Shader sources and their precompiled forms. |

The renderer creates its own GPU device without a window, renders offscreen, and copies the finished image back to normal memory.
The window then draws those pixels into OpenTTD's screen buffer like any other widget.
That costs a copy per frame, but it means the 3D view works with every video driver and never touches the game's own drawing code.
Sharing the GPU context with the video driver is a later optimisation.

Coordinates are OpenTTD's own world coordinates: 16 units per tile, 8 units per height level, x to the south-west, y to the south-east, z up.

The existing game is touched in four small places:

* `MarkTileDirtyByTile()` in `viewport.cpp` tells the 3D view which tile changed.
* `console_cmds.cpp` has the `view3d` command.
* `window_type.h` has the new window class.
* `lang/english.txt` has the strings of the window.

## Limitations

* The renderer uses Vulkan, or OpenGL 3.3 when Vulkan is not available (Metal on macOS, untested). Direct3D is not used yet, because its shaders can only be compiled on Windows.
* A 32bpp blitter is required, which is the default.
* Terrain is built for the part of the map that has been in view and stays in memory until the window is closed. On the very largest maps, looking at all of it uses a lot of memory.
* There is no anti-aliasing.
* So far it has only been run on Linux, with Mesa's software Vulkan and OpenGL drivers.

## Changing the shaders

The shaders are compiled ahead of time into `shaders/*.bin.h`, so a normal build needs no shader compiler.
After changing a `.sc` file, build bgfx's `shaderc` tool and run:

```
src/viewport3d/shaders/compile_shaders.sh /path/to/shaderc /path/to/bgfx/src
```
