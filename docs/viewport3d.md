# Experimental 3D view

This fork adds a second, 3D view of the map next to the normal 2D game.
The 2D game, its GUI, the simulation and multiplayer are unchanged: the 3D view reads the map, and building in it goes through the same tools and commands as building in the 2D views.

Status: the window shows the map, the vehicles, and the names of towns, stations and signs as the 2D game draws them, with a free camera. The build tools of the game work in it, and vehicles, names and tiles can be clicked like in the 2D views.
Everything is drawn with the graphics of the 2D game, so it follows the base set and NewGRFs in use.
The ground, tracks and roads lie on the terrain. Trees, buildings, stations, industries, signals, bridges, vehicles and the like are flat sprites that stand upright and turn to face the camera.
From the viewing angle of the 2D game that looks the same as there; from other angles it shows that they are not real 3D models.

Real 3D models come from a package called OpenGFX3D (`bin/opengfx3d/`, see its README). What has a model there is drawn with it, everything else keeps its sprites.
For now the package has two simple sample models, a bus and a lorry, for the buses and the first lorries of the base game.

## Using it

1. Start or load a game.
2. Open the console (the key left of `1`, or the `?` menu), and type `view3d`.

A window opens that looks at the same spot as the main view.
The status bar of the window shows which graphics API is in use.
To force one, type `view3d vulkan` or `view3d opengl`.

| Action | Effect |
| --- | --- |
| Left mouse button | Use the selected build tool, or open the window of the vehicle, name, sign or tile below the cursor, like in the 2D views |
| Drag with the right mouse button | Move over the map |
| Ctrl + drag with the right mouse button | Rotate and tilt the camera |
| Mouse wheel | Zoom |

Pick a tool from the toolbars of the game as usual, then click or drag in the 3D view.
The tile selection of the tool is shown in both the 2D and the 3D view.

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
| `terrain_mesh.cpp` | Turns the map array into triangles, in chunks of 64x64 tiles, and picks the ground texture of each tile. Knows nothing about the GPU. |
| `ground_textures.cpp` | Makes the ground textures from the 2D game's ground sprites when the window opens. |
| `sprite_atlas.cpp` | Collects the sprites of things that stand on the ground, such as trees and buildings, in one texture. |
| `vehicle_sprites.cpp` | Makes the sprites of the vehicles in view, anew for every image. |
| `tile_sprites.h` | Asks a tile which sprites the 2D game draws for it. The implementation is `CollectTileSprites()` in `viewport.cpp`. |
| `renderer3d.cpp` | The only file that uses bgfx. Draws the chunks into an offscreen image and reads the pixels back. |
| `camera3d.h` | Orbit camera, matrices and frustum culling. |
| `shaders/` | Shader sources and their precompiled forms. |

The renderer creates its own GPU device without a window, renders offscreen, and copies the finished image back to normal memory.
The window then draws those pixels into OpenTTD's screen buffer like any other widget.
That costs a copy per frame, but it means the 3D view works with every video driver and never touches the game's own drawing code.
Sharing the GPU context with the video driver is a later optimisation.

A ground texture is the sprite of a flat tile, a diamond of 64x31 pixels at normal zoom, pulled straight into a square that covers one tile.
Slopes use the same texture as flat land and get their shading from the lighting instead of from the slope sprites.
Coasts are the exception: there the sprite of each slope is pulled straight, because it is the sprite that says where the water ends.
All textures are layers of one texture array; each tile names its layer in the alpha channel of its vertices.

Plain landscape tiles are handled directly. For every other tile, `CollectTileSprites()` lets the tile draw itself the way it does for a 2D viewport and keeps the list of sprites instead of drawing them.
The sprites the 2D game draws flat on the ground, such as tracks and roads, are pulled straight into a ground texture for that combination of sprites; these textures are made on demand, up to 1024 of them.
Every sprite with a bounding box, with the sprites drawn along with it, becomes one upright sprite that stands in the middle of that box.
This way NewGRF houses, industries, stations and objects work without any code of their own.

Vehicles are upright sprites as well, looked up again for every image because they move all the time.
The 2D game has sprites of each vehicle for eight directions; the 3D view picks the one that fits the side the camera sees, so a vehicle keeps pointing along its track or road when the camera goes around it.
While vehicles are in view the image is redrawn every game tick; without them it is only redrawn when something changes.

Trees are sprites on upright quads that turn to face the camera. Hedges and fences are sprites on small walls along the edge of their tile.
Both are sized and placed such that from the viewing angle of the 2D game they look the same as there.

Coordinates are OpenTTD's own world coordinates: 16 units per tile, x to the south-west, y to the south-east, z up.
Heights are scaled down: the 2D game draws a height level as 8 pixels, which at its viewing angle of 30 degrees corresponds to about 3.3 world units rather than 8.
With that scale the 3D view at the default angle has the proportions of the 2D game.

To build in the 3D view, the window finds the spot on the ground below the cursor by following the line of sight through the cursor until it goes below the terrain.
`GetTileBelowCursor()`, which all build tools of the game use, asks the 3D view first when the cursor is over it, so tools, dragging and the tile selection work without changes to the tools.
The selection itself is drawn by the 2D game on the ground of a tile, so it arrives in the 3D view as part of the ground texture of that tile.

Names of towns and stations and signs are not part of the 3D image. The window asks the 2D game to draw them on top of the image, at the place where the 3D view shows the spot they belong to (`DrawView3DSigns()`), so they look and are filtered like in the 2D views.
A click first looks for a vehicle of which the sprite is below the cursor, then for a name or sign, then for the tile, in the same order as the 2D views (`HandleView3DClicked()`).

3D models are read from binary glTF files (`model3d.cpp`) listed in the index of the OpenGFX3D package (`model_pack.cpp`).
A vehicle with a model is put on the map at its position, turned to its direction of travel and tilted on slopes, and lit with the same light as the terrain.
Parts of a model with a material named `company` take the colour of the owner.

The existing game is touched in a few small places:

* `MarkTileDirtyByTile()` in `viewport.cpp` tells the 3D view which tile changed, `SetSelectionTilesDirty()` tells it where the selection changed, `GetTileBelowCursor()` asks it for the tile below the cursor, and `CollectTileSprites()`, `HasTileSelection()`, `DrawView3DSigns()` and `HandleView3DClicked()` at the end of that file tell it what a tile draws, draw the names and pass on clicks.
* `console_cmds.cpp` has the `view3d` command.
* `window_type.h` has the new window class.
* `lang/english.txt` has the strings of the window.

## Limitations

* The renderer uses Vulkan, or OpenGL 3.3 when Vulkan is not available (Metal on macOS, untested). Direct3D is not used yet, because its shaders can only be compiled on Windows.
* A 32bpp or 40bpp blitter is required, which is the default; the 8bpp blitters do not work.
* Terrain is built for the part of the map that has been in view and stays in memory until the window is closed. On the very largest maps, looking at all of it uses a lot of memory.
* The ground textures and sprites are taken from the game when the window opens or when they are first needed. After changing NewGRFs in a running game, close and reopen the window.
* The shadows of aircraft are not drawn yet.
* Names and signs are always drawn on top, also when a hill is in front of their place, and all of them in view are shown however far away they are.
* The tile below the cursor is found on the bare terrain, so behind a tall building the tile that the building hides is picked.
* What is in front of what is decided per sprite, so now and then a vehicle shows through the corner of a building it passes behind.
* Buildings made of several sprites or covering several tiles can come apart when the camera is rotated, and tiles on foundations draw their ground as an upright sprite too.
* Transparency options of the 2D game are followed, but see-through sprites are left out rather than drawn see-through.
* Tiles with animations, such as some industries, rebuild their whole chunk of 64x64 tiles for every animation step.
* Water is not animated, and sprites far away flicker a little because they have no smaller versions yet.
* 3D models have colours per material but no textures yet, vehicles with a model turn in steps of 45 degrees like the sprites do, and liveries other than the company colour are not followed.
* There is no anti-aliasing.
* It has been run on Linux with Mesa's software Vulkan and OpenGL drivers, and on Windows with the Direct3D 11 video driver.

## Changing the shaders

The shaders are compiled ahead of time into `shaders/*.bin.h`, so a normal build needs no shader compiler.
After changing a `.sc` file, build bgfx's `shaderc` tool and run:

```
src/viewport3d/shaders/compile_shaders.sh /path/to/shaderc /path/to/bgfx/src
```
