# Experimental 3D view

This fork adds a second, 3D view of the map next to the normal 2D game.
The 2D game, its GUI, the simulation and multiplayer are unchanged: the 3D view reads the map, and building in it goes through the same tools and commands as building in the 2D views.

Status: the window shows the map, the vehicles, and the names of towns, stations and signs as the 2D game draws them, with a free camera. The build tools of the game work in it, and vehicles, names and tiles can be clicked like in the 2D views.
Everything is drawn with the graphics of the 2D game, so it follows the base set and NewGRFs in use.
The ground, tracks and roads lie on the terrain. Trees, buildings, stations, industries, signals, bridges, vehicles and the like are flat sprites that stand upright and turn to face the camera.
From the viewing angle of the 2D game that looks the same as there; from other angles it shows that they are not real 3D models.

Real 3D models come from a package called OpenGFX3D, a separate repository (https://source.tiagoagueda.com/tiagoagueda/OpenGFX3D) included as a git submodule at `bin/opengfx3d/` (see its README). What has a model there is drawn with it, everything else keeps its sprites.
For now the package has simple sample models: a bus and a lorry, steam, diesel and electric engines, a railcar and the common carriages and wagons of the base game's normal railway, the common houses, flats, offices, shops and church of the base game's temperate towns, the factory, rail station platforms, buildings and roofs, and bus and lorry stops.

After cloning, fetch the package with `git submodule update --init`. Without it the game builds and runs, but everything is drawn with sprites.

## Using it

Turn on "Show the map in 3D" in the settings (Interface, Viewports; `view3d_main` in `openttd.cfg` and in the console).
The main view is then drawn in 3D, full screen behind the toolbars, in every game until the setting is turned off again.
The arrow keys, the minimap, "go to location" buttons, following a vehicle and the zoom buttons of the toolbar all work on it, because it follows the 2D main view, which stays in step underneath (`FollowMainView()` and `MoveMainView()` in `view3d_gui.cpp`).
It is a window of class `View3DMain` that covers the main window and stays below all other windows; when the 3D renderer cannot start, an error says why and the 2D view shows again.

The 3D view can also be opened as a window of its own, next to the 2D main view:

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
| Mouse wheel | Zoom in or out, towards the spot below the cursor |
| Page Up / Page Down | Turn a quarter to the left or right |
| Home | Look from the north again at the angle of the 2D view |

Zooming and turning glide rather than jump, and the camera follows the height of the ground smoothly while moving.

Pick a tool from the toolbars of the game as usual, then click or drag in the 3D view.
The tile selection of the tool is shown in both the 2D and the 3D view.
While placing a train or road vehicle depot, a rail station or a road stop of the default class, the 3D view also shows a see-through model of it on the selected tiles (`GetRailToolPreview()` in `rail_gui.cpp`, `GetRoadToolPreview()` in `road_gui.cpp`, `AddPreviewModel()` in `terrain_mesh.cpp`).

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

Trees without a 3D model are sprites on upright quads that turn to face the camera. Hedges and fences are sprites on small walls along the edge of their tile.
Both are sized and placed such that from the viewing angle of the 2D game they look the same as there.

Coordinates are OpenTTD's own world coordinates: 16 units per tile, x to the south-west, y to the south-east, z up.
Heights are scaled down: the 2D game draws a height level as 8 pixels, which at its viewing angle of 30 degrees corresponds to about 3.3 world units rather than 8. At the middle of the image the 3D view then shows hills as high as the 2D view does. The camera has a narrow field of view (about 17 degrees), so the angle changes little from the top to the bottom of the image and hills near the bottom do not look flatter than in the 2D view. Slopes are shaded a little more than their true slope would give, like the 2D ground sprites.
With that scale the 3D view at the default angle has the proportions of the 2D game.

To build in the 3D view, the window finds the spot on the ground below the cursor by following the line of sight through the cursor until it goes below the terrain.
`GetTileBelowCursor()`, which all build tools of the game use, asks the 3D view first when the cursor is over it, so tools, dragging and the tile selection work without changes to the tools.
The selection itself is drawn by the 2D game on the ground of a tile, so it arrives in the 3D view as part of the ground texture of that tile.

Names of towns and stations and signs are not part of the 3D image. The window asks the 2D game to draw them on top of the image, at the place where the 3D view shows the spot they belong to (`DrawView3DSigns()`), so they look and are filtered like in the 2D views.
A click first looks for a vehicle of which the sprite is below the cursor, then for a name or sign, then for the tile, in the same order as the 2D views (`HandleView3DClicked()`).

3D models are read from binary glTF files (`model3d.cpp`) listed in the index of the OpenGFX3D package (`model_pack.cpp`).
A vehicle with a model is put on the map at its position, turned to its direction of travel and tilted on slopes, and lit with the same light as the terrain.
A rail vehicle model lies along the line between the vehicles in front of and behind it, so a train bends through curves instead of turning in steps of 45 degrees; it is stretched to the distance between those neighbours, which is longer on diagonal track. Vehicles that are flipped in the train are turned around. Road vehicles, ships and aircraft turn towards the way they go a little later than the game turns them, so they turn smoothly (`vehicle_sprites.cpp` remembers their direction between images). The main rotor of a helicopter is a model of its own that turns while the game turns the rotor sprites.

Models cast shadows from the sun, which comes from the same direction as the light on the terrain. Each image first draws the models, as depth only, into a shadow map that looks from the sun at the area around the point the camera looks at (`GetShadowView()` in `renderer3d.cpp`); terrain, models and sprites are then darker where the map holds something between them and the sun (`shaders/shadow.sh`). Aircraft in the air cast their shadow this way too. Terrain and sprites, such as trees, cast shadows too: terrain from a little further away from the sun, and sprites turned to face the sun. Shadows fade out towards the edge of the map, so a camera far away sees none in the distance. After changing a shader, run `shaders/compile_shaders.sh`.

Smoke, steam and sparks (effect vehicles) are drawn as see-through 3D puffs (`effect` in the index) that grow and fade while they are in view (`AddEffectModel()` in `vehicle_sprites.cpp`); see-through models are blended over what is behind them and cast no shadow. Other effects keep their sprites.

The sky is one triangle behind everything (`vs_sky`, `fs_sky`), pale at the horizon and deeper blue above; terrain, sprites and models fade into the colour of the horizon with distance (`shaders/haze.sh`). Water moves with small waves, and foam washes along the edge of the water on coast tiles (`fs_terrain.sc`; `u_water` tells which ground textures are water and the time).

Models can have a simpler version for far away (`far` in the index), or be left out there, like the stones of rough land. The renderer picks it per copy by the distance to the camera.

Copies of the same model are drawn in one go with GPU instancing (`vs_model_instanced`, `vs_shadow_instanced`), which keeps forests of 3D trees fast; the load of a vehicle is still drawn one by one.

Trees, farm fields, rough land and rocks have models too (`tree`, `field`, `rough` and `rocks` in the index). Trees stand where the 2D game puts their sprites, grow from small to full size, and keep their sprites when the package has no model for their type; fields, rough land and rocks are tilted along the slope of their tile (`AddTreeModels()` and `AddClearModel()` in `terrain_mesh.cpp`).

Infrastructure has models too (the functions are in `terrain_mesh.cpp`):

* Track (`track`): one piece per track of plain rail tiles, tilted along slopes, for rail, monorail and maglev (`AddTrackModels()`).
* Signals (`signal`, by type, variant and state): they stand where the 2D game puts its signal sprites, face the trains they are for, and are mirrored when signals are on the left (`AddSignalModels()`). Their sprites are left out by their place on the tile.
* Overhead wires (`catenary`): wires above each track, and pylons where the 2D game puts its pylon sprites (`AddWireModels()`, `AddPylonModel()`). This and the rail fences are used only with the base set's sprites, not with NewGRF rail types that draw their own.
* Fences (`fence`): along rail tiles and the edges of farm fields (hedges, wooden fences, stone walls), stretched and sheared along each edge (`AddRailFenceModels()`, `AddFences()`).
* Bridges (`bridge`): a deck piece above each tile under a bridge, a pillar stretched down to open land or water, and a ramp on each head, carrying track, wires or road (`AddBridgeMiddleModel()`, `AddBridgeRampModel()`). `DrawBridgeMiddle()` calls `MarkBridgeMiddleSprites()` so `CollectTileSprites()` can tell which sprites of a tile belong to the bridge above it. Aqueducts use bridge type 17 of the package (`GetBridgeModelType()`). Road bridges with electric road or tram types carry wires.
* Tunnels (`tunnel`): a portal on the entrance tile, which the terrain then draws flat at the foot of the slope; wing walls close the sides (`AddTunnelModel()`).
* Level crossings (`crossing`, open and closed) with their track (`AddCrossingModel()`). Their ground becomes plain road, because the 2D ground sprite has the lights painted in.
* Along roads (`roadside`): pavements with kerbs on town roads, tram rails, and street lamps and trees where the 2D game puts their sprites (`AddRoadModels()`, `AddRoadDetailModel()`). Trams and electric roads get wires and a pole beside straight roads (`AddRoadWireModels()`, catenary ids 4 and 5), unless a NewGRF draws its own.

Models of buildings can have parts that move (`parts` in the index), such as the beams of pump jacks, the winding wheels of mines and the press of the toy factory; they are drawn anew for every image, from the chunks in view. Tiles that are drawn with a model are not rebuilt when the game marks them dirty without changing what model they get (animated tiles do so for every frame of their animation); `MarkTileDirty()` remembers the models of such tiles to tell.

Signals and the barriers of level crossings are "live" models: they are kept per tile (`TerrainChunkMesh::live`) and drawn every frame like vehicles, so a train passing by does not rebuild its chunk. `GetTileChangeKey()` hashes the map bits of rail tiles and crossings without signal states, reservations and barriers; `MarkTileDirty()` only rebuilds the chunk when that key changed, and otherwise just looks up the live models of the tile again. Grass, fields and trees change all over the map all the time; a change that keeps a tile bare land or trees at the same height marks its chunk `slow`, and slow chunks are rebuilt a few seconds later, one per frame.

The scene is drawn with 4x multisampling where the GPU supports it.
Parts of a model with a material named `company` take the colour of the owner. Parts with a material named `load` are the cargo: they are drawn only when the vehicle is loaded, lower down the emptier it is.

A building with a model is part of the terrain chunk it stands on (`AddTileModel()` in `terrain_mesh.cpp`): the sprites the tile draws for the building are left out and the model is put on the highest corner of the footprint, turned to face the way the building faces.
Besides houses, industry tiles, rail stations, road stops and objects, models can replace airport tiles, docks, buoys and train, road and ship depots; depots and docks face the way their entrance does, and a ship depot has one model for both its tiles.
Houses and industries take the colour the game picked for them, stations, stops and objects that of their owner.
A tile with a foundation is drawn flat at the top of the foundation, with stone walls down to the terrain around it, so that a model stands on level ground; half-tile foundations still use the sprites.

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
* The tile below the cursor is the first building with a model along the line of sight (using the box around its model), or else the terrain; bridges, vehicles and buildings drawn with sprites are not looked at.
* What is in front of what is decided per sprite, so now and then a vehicle shows through the corner of a building it passes behind.
* Buildings without a model that are made of several sprites or cover several tiles can come apart when the camera is rotated, and tiles on half-tile foundations draw their ground as an upright sprite too.
* Transparency options of the 2D game are followed, but see-through sprites are left out rather than drawn see-through.
* Tiles with animations that have no model (some industries with NewGRF graphics) rebuild their whole chunk of 64x64 tiles for every animation step; building one chunk takes about 1.5 ms.
* Water is not animated, and sprites far away flicker a little because they have no smaller versions yet.
* 3D models have colours per material but no textures yet, vehicles with a model turn in steps of 45 degrees like the sprites do, and liveries other than the company colour are not followed.
* It has been run on Linux with Mesa's software Vulkan and OpenGL drivers, and on Windows with the Direct3D 11 video driver.

## Roadmap

Next, in this order unless plans change: textures for the 3D models, day and night (light that follows the time of day, lit windows and lamps), and weather and seasons (snow, rain, autumn colours).

## Changing the shaders

The shaders are compiled ahead of time into `shaders/*.bin.h`, so a normal build needs no shader compiler.
After changing a `.sc` file, build bgfx's `shaderc` tool and run:

```
src/viewport3d/shaders/compile_shaders.sh /path/to/shaderc /path/to/bgfx/src
```
