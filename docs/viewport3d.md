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
The tools that build along lines show what they are about to build the same way, like the 2D views do with their track pieces under the cursor:

* The rail tools (the four track directions and autorail) put a see-through piece of track of the chosen type on each tile where the 2D views draw the autorail selection, while hovering and while dragging (`AddTrackPreviewModel()`). `GetTileMark()` in `viewport.cpp` also tells which piece that is; such tiles get no plate, as the piece shows the place, except a red one where the piece cannot be built on the slope. NewGRF track types show normal track.
* The road and tram tools (road along x or y, and autoroad) put see-through pieces of road, or of tram rails, along the drag; on a single tile only the half that a click there builds (`AddRoadPreviewModel()`, `interface` model 6 for road, `roadside` model 4 for tram rails). In remove mode the road is red.

Both lie on the foundation the piece would get (`GetRailFoundation()`, `GetRoadFoundation()`), a little above the ground, so a piece over existing track or road still shows.

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
| `lighting3d.cpp` | The light at each hour of the day and the night. |
| `weather3d.cpp` | The season and the weather of the moment. |
| `shaders/` | Shader sources and their precompiled forms. |

The renderer creates its own GPU device without a window, renders offscreen, and copies the finished image back to normal memory.
The window then draws those pixels into OpenTTD's screen buffer like any other widget.
That costs a copy per frame, but it means the 3D view works with every video driver and never touches the game's own drawing code.
Sharing the GPU context with the video driver is a later optimisation.

A ground texture is the sprite of a flat tile, a diamond of 64x31 pixels at normal zoom, pulled straight into a square that covers one tile.
Slopes use the same texture as flat land and get their shading from the lighting instead of from the slope sprites.
Coasts are the exception: there the sprite of each slope is pulled straight, because it is the sprite that says where the water ends.
These textures are layers of one texture array; each tile names its layer in its vertices.

When the model package has ground textures (`ground` in its index), the landscape uses those instead: grass, bare land, rough land, rocks, snow, desert and water.
They are larger images that are each spread over several tiles, in a second texture array (`s_terrain`), so single tiles do not show; the shader also makes whole regions a little lighter or darker to hide the repetition.
`GroundTextures::MakeTerrainTextures()` tints each one to the average colour of the ground sprite that the 2D game draws for that kind of ground, so the colours still follow the climate and the base graphics, and tracks, roads and other things whose ground is made from sprites do not stand out more than needed.
Grass that grows over bare land, and snow or desert that takes over from grass, are blends of two textures through a mask.
Reading the textures needs libpng; a build without it keeps the ground made from sprites.

The ground of tiles with things on them is then a mix (`GroundMix` in `ground_textures.h`). A texture made from sprites keeps only the pixels that show something other than plain landscape, multiplied by how much of each pixel they cover; a second texture array with the same layers (`s_mix`) says how much of the landscape, asphalt, gravel and paving of the package to add.
`GroundTextures::UseTerrain()` collects the colours of the grass, bare land and snow or desert sprites on all slopes, so the landscape around houses, stations and industries is the textured landscape, continuous with the tiles around it; the vertices name which landscape.
Plain track and plain road of the base game are not made from sprites at all, as the 3D models draw the track and the kerbs (`GetGroundPattern()` in `terrain_mesh.cpp`): a pattern of gravel beside the tracks, or asphalt with grass verges or pavements, with a dashed line down straight roads (`GroundTextures::GetPattern()`). The same goes for what stands on the ground as a model: the ballast of stations and waypoints (also of NewGRFs when the package has a model for them), the pavement of road stops, a road across the track bed at level crossings, the track bed or road out of the door of depots, and under industries with a model gravel where they dig or pump, paving where they make things and grass where they grow things.
Rail with a reservation shown, road works, one-way roads, and road and rail types of NewGRFs keep their sprites.

With those textures a coast is no longer a sprite. The corners of coast tiles that have only sea around them are drawn below sea level (`IsSeaCorner()` in `terrain_mesh.cpp`), so the land goes down under the water: sand near the water, grass from some way up.
A second quad at sea level is the water over it (`TerrainChunkMesh::water`). Its vertices hold the height of the ground below, from which the shader knows the depth: no water where the ground is above sea level, clear water over the sand, foam along the edge and solid water further out. It is drawn after everything else, blended.
Rivers that run down into the sea are handled the same way, and the open sea is drawn shallow in corners where it meets land or anything built.
Flat river tiles above sea level (`AddRiverBed()`) are a bed below the land with banks that slope down from every side where there is land, with see-through water a little below the land over them; the same shader shows the bed through the shallow water at the banks.
River tiles on a slope get the same bed and banks, following the slope, and their water is rapids: the marker layer of the water (`GROUND_TEXTURE_RAPIDS` plus 0 to 3, from `GetRapidsFlow()`) says which way it flows, and `Rapids()` in `fs_terrain.sc` streaks it white with foam that races downhill, smooth at the lip and churning at the foot. The flat river tile below rapids (`GROUND_TEXTURE_RAPIDS_FOOT` plus the direction they come from) gets the foam carried on into it, fading out within half a tile. Without ground textures in the model package, the shader finds sloped water from how its height changes across the screen and draws the same rapids over the water sprite. This is looks only; the river itself is unchanged.

Where a river runs down into the sea, its last sloped tile is rapids as well (`IsShoreSlope()` leaves river tiles out): the water ends at sea level at the lower edge, a curtain of water closes the bed off below the sea, and the sea tile below gets the foam of the foot of the rapids as a see-through quad over it.

Canals (`AddCanalBed()`, with ground textures from the model package) lie between walls of masonry: along each side without water at the same height a wall goes from the bed, half a height level below the water, to a coping a tenth of a level above the land, set a tenth of a tile into the tile; inner corners get a short pillar. The water is see-through enough to make out the bed. Buoys in a canal stand in such water too.

Canal locks (`lock3d.cpp`) fill and empty for the ships that pass them. Ships are longer than a tile, so the three tiles of a lock are one chamber with high walls all along (models `lock` 0 to 2), the lower gates at the outer end of the lower part and the upper gates, on a sill, at the outer end of the upper part. The gate leaves are models of their own (`lock` 3 lower, 4 upper), turned around their hinges for every image, and the water in the chamber is drawn for every image too, as see-through water passed to `Renderer3D::Render()`. A ship that comes towards a lock has it made ready: the far gates close, the chamber fills or empties, the near gates open. While the middle of the ship is in the chamber its gates close behind it, the water takes it up or down (`GetLockShipHeight()` draws the ship on that water) and the gates ahead open. Without ships a lock stays as the last one left it. This is looks only: the game moves ships as before. Test: `test-scripts/CanalTest` (with `canal.cfg`) builds a lock with canals, a depot, buoys and a ship, and logs river mouths.

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
What the 2D game draws on the ground to mark a tile (the selection of a tool, the catchment area and tiles of a highlighted station, the reach of a town, a failed build) is left out of the ground; `GetTileMark()` in `viewport.cpp` asks `DrawTileSelection()` what it would draw and the 3D view lays a see-through plate on the tile instead, white, blue or red like the 2D marks (`AddTileMark()`, `interface` model 5, two halves that follow the ground).

Names of towns and stations and signs stand in the world: each is a board on a post (`interface` model 0), facing the camera, above the spot it belongs to (`AddLabels()` in `view3d_gui.cpp`). The 2D game makes the list and draws each board's picture (`GetView3DLabels()`, `DrawView3DLabel()` in `viewport.cpp`; town names get a dark board), which goes into rows at the bottom of the sprite atlas that start afresh when they are full (`SpriteAtlas::GetLabel()`). Boards keep about the size of the 2D views at the spot the camera looks at, so nearer ones are larger and further ones smaller; hills hide them, and they are neither shaded nor hazed (the sprite shaders tell them by a nearness of 1000 or more). Clicking a board opens its window.

The cursor is in the world too (`AddCursorModels()`): with a build tool, a white frame with corner posts follows the ground around the selected tiles (or the tile below the mouse for tools that build along lines), and a yellow arrow bobs over the tile below the mouse; without a tool, white brackets mark the corners of that tile. The parts are `interface` models 1 to 4.

With the 3D main view, the views inside other windows (news messages, town, vehicle, station and industry windows, extra viewports) are drawn in 3D too. `Window::DrawViewport()` in `viewport.cpp` asks `DrawViewportIn3D()`, which copies the latest 3D image of that window's view and notes where the view looks; the 3D window then renders the small views in turns with its own image (`RenderSmallView()`), each into its own render target of the renderer (`Renderer3D::Render()` takes the id of the view), when one moved or, a few times a second, when something in it moved. The camera looks at the spot in the middle of the 2D view from the default angle, from as far as makes the scale match its zoom. Until its first image is ready, a view is left empty rather than flashing up in 2D. When such a view scrolls, as when it follows a vehicle, `SetViewportPosition()` has it drawn again from the 3D image rather than moving its old pixels along, which would mix two images; a new news message in the place of the last one gets an image of its own. These small views have no names, signs or cursor.

The buttons of construction windows (rail stations, train depots, waypoints, road stops, road depots, ship depots, objects and houses) show the 3D models too: `DrawView3DPicture()` gets a `View3DPicture` from the window or, for pickers, from `PickerCallbacks::GetView3DPicture()`, and the 3D window renders each picture once (`RenderPicture()`), on the colour of the button with no sky so its edges blend in, from the default angle and as close as fits the model. Until the picture is there, and for things without a model, the 2D sprites are drawn.

The 3D main view also shows behind the title screen and while a new map is made, when it then shows the map as it grows; it takes no clicks or keys there.

With the 3D main view and the setting "3D-styled windows and buttons" (`gui.view3d_interface`, on by default; needs a 32bpp blitter), `DrawFrameRect()` in `widget.cpp` draws every frame of the interface shaded: raised buttons and panels lit from above with a sheen and a dark lower edge, lowered ones shaded like a hollow (`DrawFrameRect3D()`). Tall panels get only a hint of it.
A click first looks for a vehicle of which the sprite is below the cursor, then for a name or sign, then for the tile, in the same order as the 2D views (`HandleView3DClicked()`).

3D models are read from binary glTF files (`model3d.cpp`) listed in the index of the OpenGFX3D package (`model_pack.cpp`).
A vehicle with a model is put on the map at its position, turned to its direction of travel and tilted on slopes, and lit with the same light as the terrain.
A rail vehicle model lies along the line between the vehicles in front of and behind it, so a train bends through curves instead of turning in steps of 45 degrees; it is stretched to the distance between those neighbours, which is longer on diagonal track. Vehicles that are flipped in the train are turned around. Road vehicles, ships and aircraft turn towards the way they go a little later than the game turns them, so they turn smoothly (`vehicle_sprites.cpp` remembers their direction between images). The main rotor of a helicopter is a model of its own that turns while the game turns the rotor sprites.

Models cast shadows from the sun, which comes from the same direction as the light on the terrain. Each image first draws the models, as depth only, into a shadow map that looks from the sun at the area around the point the camera looks at (`GetShadowView()` in `renderer3d.cpp`); terrain, models and sprites are then darker where the map holds something between them and the sun (`shaders/shadow.sh`). Aircraft in the air cast their shadow this way too. Terrain and sprites, such as trees, cast shadows too: terrain from a little further away from the sun, and sprites turned to face the sun. Shadows fade out towards the edge of the map, so a camera far away sees none in the distance. After changing a shader, run `shaders/compile_shaders.sh`.

Smoke, steam and sparks (effect vehicles) are drawn as see-through 3D puffs (`effect` in the index) that grow and fade while they are in view (`AddEffectModel()` in `vehicle_sprites.cpp`); see-through models are blended over what is behind them and cast no shadow. Explosions are orange puffs, bubbles of the bubble generator light blue ones, and the bulldozer of road works is a model of its own (effect 8), turned the way its sprite faces.

Disasters (zeppelin, UFOs, planes, helicopter and its rotor, submarines) have models by disaster vehicle type (`disaster`, `AddDisasterModel()`); rotors and UFOs spin. Their shadows are left out, like those of aircraft.

Houses and industries that are still being built show building sites (`construction`): one model per stage of the work and size of the plot, with scaffolding, walls and timbers that grow from stage to stage. Base-game rail and road waypoints (`waypoint`) and canal locks (`lock`, by part, and the leaves of their gates) have models too.

The sky is one triangle behind everything (`vs_sky`, `fs_sky`), pale at the horizon and deeper blue above; terrain, sprites and models fade into the colour of the horizon with distance (`shaders/haze.sh`). Water moves with small waves, and foam washes along the edge of the water on coast tiles (`fs_terrain.sc`; `u_water` tells which ground textures are water and the time, `u_terrain` how the ground textures of the model package are used).

### Seasons and weather

The setting "Seasons and weather in the 3D view" (`gui.view3d_weather`, Interface > Viewports) has four values: off, seasons only, seasons and some weather (the default), seasons and a lot of weather.
Both are only looks. Nothing in the game changes, nothing is saved, and in multiplayer every player can have a setting of their own.

`GetWeather3D()` in `weather3d.cpp` says what the image looks like at this moment (`Weather3D`): how much cloud, rain, snowfall and fog there is, how wet the ground is, how much snow lies on it, and how far the year is for leaves and grass.

* The season follows the date of the game. Temperate: fresh green and blossom in spring, drier grass in late summer, autumn colours from mid-September, bare trees and snow in winter. Sub-arctic: the same with a longer winter. Sub-tropical: a dry season in the middle of the year with yellow grass and a rainy season around it. Toyland has no seasons and no weather.
* The weather comes in spells of 24 days. What a spell brings is made up from its number and the seed of the map, with chances that depend on the climate and the season (`GetWeatherChances()`), so it is the same for every view and after loading a game. A spell turns into the next one over a quarter of its length. Snow on the ground and wet ground are worked out from the last ten spells: snow piles up while it snows, melts slowly in winter and fast in rain or outside winter.
* Thunderstorms have lightning: a flash now and then that lights up the land and the sky.

How it is drawn:

* Sky and haze (`Renderer3D::Render()`): clouds turn the sky and the haze grey; fog, and rain and snow a little, bring the haze nearer and make it thicker. Shadows fade under clouds (`u_shadowParams.w`). `WeatherGrade()` in `shaders/weather.sh`, called from `ApplyHaze()`, makes everything a little darker and greyer under clouds.
* Ground (`GroundWeather()` in `shaders/weather_terrain.sh`, called at the end of `fs_terrain.sc`): what is green changes colour with the season, wet ground is darker, and snow comes in patches that grow until they meet. Roads and other built ground get less snow, water none.
* Models (`ModelWeather()` in `shaders/weather.sh`, called from the vertex shaders of models): snow lies on faces that look up, except low down, where the ground has its own snow (this keeps rails in sight). Leaves change colour: corners of materials with `leaf`, `leaves` or `foliage` in their name get alpha `ModelVertex::LEAF` (`model3d.cpp`). Each tree turns at its own time and to its own colour, from a number made of its place on the map. `ModelPack::ReadEntries()` keeps the flag only for trees that lose their leaves and for the trees along roads; firs, spruces, pines, the trees of the tropics and the green of buildings stay as they are.
* Rain and snow (`vs_weather.sc`, `fs_weather.sc`): 16000 drops at fixed made-up places in a cube that repeats all over the world; the cube around the spot the camera looks at is drawn, and the drops move through it with the time. Two cubes, one twice the size of the other, take over from each other as the camera zooms, so drops keep their size and number on the screen. Rain is streaks along the way it falls, snow soft flakes that sway.

While it rains or snows a new image is made about 30 times a second; other weather and the seasons only ask for one when something changed enough to see.
The uniforms are `u_weather` (snow on the ground, wet, clouds, lightning) and `u_season` (autumn, bare, spring, dry).

For testing and pictures, the console has `view3d_weather <auto|clear|cloudy|rain|storm|snow|fog> [snow on the ground 0-100]` and `view3d_season <auto|1-12>`.

Sprites of the 2D game (things without a model) do not change with the seasons and get no snow.

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

Models have textures: the base colour texture of each glTF material, multiplied with the colour of the material (and so with the company colour).
The images are PNG files, inside the model or files of the package that the models name and share; `ModelPack::FindModelTexture()` reads each image once and `FinishModelTextures()` brings them to one size, as they become the layers of one texture array (`s_modelTextures`). Each vertex of a model carries its place in the texture and its layer, or -1 for a plain colour (`ModelVertex`), so a whole model is still one draw call, and the copies of one model are still drawn together.
The models of OpenGFX3D get grey textures of surfaces (brickwork, roof tiles, planks, sheet metal with seams and rivets, leaves) picked by the name of the material and laid on by size; see its README.

A building with a model is part of the terrain chunk it stands on (`AddTileModel()` in `terrain_mesh.cpp`): the sprites the tile draws for the building are left out and the model is put on the highest corner of the footprint, turned to face the way the building faces.
Besides houses, industry tiles, rail stations, road stops and objects, models can replace airport tiles, docks, buoys and train, road and ship depots; depots and docks face the way their entrance does, and a ship depot has one model for both its tiles.
Houses and industries take the colour the game picked for them, stations, stops and objects that of their owner.
A tile with a foundation is drawn flat at the top of the foundation, with stone walls down to the terrain around it, so that a model stands on level ground; an inclined foundation, as under track and road that go up over a single raised corner, slopes along the way up like the vehicles do. Half-tile foundations still use the sprites.
Rail stations and waypoints and road stops of the base game have the same 3D track (`AddStationTrack()`) and the same ballast or road ground as plain track and road, with the platforms, shelters and canopies as models.
Where straight track meets diagonal track on the next tile, a joint that bends by 45 degrees (`track` models 6 to 8) closes the gap between the rails of the two pieces.

The existing game is touched in a few small places:

* `MarkTileDirtyByTile()` in `viewport.cpp` tells the 3D view which tile changed, `SetSelectionTilesDirty()` tells it where the selection changed, `GetTileBelowCursor()` asks it for the tile below the cursor, and `CollectTileSprites()`, `GetTileMark()`, `GetView3DLabels()`, `DrawView3DLabel()` and `HandleView3DClicked()` at the end of that file tell it what a tile draws, make the names and pass on clicks; `Window::DrawViewport()` lets it draw the views of other windows.
* `console_cmds.cpp` has the `view3d` command.
* `window_type.h` has the new window class.
* `lang/english.txt` has the strings of the window.

## Day and night

The game has no clock and its days last about two seconds, so the 3D view keeps a clock of its own: a day and a night together last a number of minutes of play, the setting "Day and night in the 3D view" (Interface, Viewports; `view3d_day_length`, 30 by default, 0 for the fixed light of the 2D graphics). The clock runs on game ticks, so it stands still while the game is paused and runs fast with fast forward. A new game starts at nine in the morning. The console command `view3d_time <hour>` holds the view at an hour (handy for screenshots), `view3d_time auto` lets go again, and `view3d_time` alone tells the time. It is only for looks: nothing in the game depends on it.

`lighting3d.cpp` turns the hour into a `Lighting3D`: the direction and colour of the light that casts shadows (the sun from six to six, rising in the east and standing in the south at noon, then the moon), the light from all around, the colours of the sky and the haze, how well the stars show, how much lamps glow and which share of the windows is lit. The window hands it to the renderer with `SetLighting()` and asks for a new image when the hour has moved on by a quarter of a minute of the day.

The shaders get it through `shaders/light.sh`:

* Models are lit per pixel with `Light()`.
* The terrain keeps the light of the 2D graphics in the colours of its vertices, so chunks need no rebuild as the sun moves. The fragment shader finds which way the ground faces from how the position changes across the screen, and `Relight()` divides the baked light out and multiplies the light of the moment in. With the cycle off the two are the same and the ground looks as before.
* Sprites of the 2D game are relit as if they were flat ground. Names and signs stay bright enough to read.
* The sky draws the sun, the moon and the stars.

What glows at night goes by the name of the material in the model files, so the model package needs nothing new (`AddPrimitive()` in `model3d.cpp`), and is passed on in the alpha of the vertex colours (`shaders/material.sh`):

| Material name | What it does at night |
| --- | --- |
| contains `glass` | A window. Each separate piece of glass of a building is lit or dark by lot, from where the building stands; more are lit in the evening than deep in the night. A large pane (over 3.2 units) has many rooms behind it, which are lit one by one. Vehicles light all their windows. |
| `light`, or ends in `_lamp` | A lamp: glows, and casts a patch of light on the ground. Lamps high up, like street lamps, light a circle below them; low lamps in the front half of a model are headlights and light the road ahead; lamps on the ground, like runway lights, light a small circle. |
| ends in `_on`, `_glow` or `ufo_light` | Glows without lighting the ground: signals, crossing lights, furnaces. |

The patches of light are flat fans of triangles, made per model in `MakeLightPools()` in `renderer3d.cpp` and added to the picture after the model, just above the ground the model stands on. They do not follow slopes, and nothing is really lit by them: buildings next to a street lamp stay dark.

## Limitations

* The renderer uses Vulkan, or OpenGL 3.3 when Vulkan is not available (Metal on macOS, untested). Direct3D is not used yet, because its shaders can only be compiled on Windows.
* A 32bpp or 40bpp blitter is required, which is the default; the 8bpp blitters do not work.
* Terrain is built for the part of the map that has been in view and stays in memory until the window is closed. On the very largest maps, looking at all of it uses a lot of memory.
* The ground textures and sprites are taken from the game when the window opens or when they are first needed. After changing NewGRFs in a running game, close and reopen the window.
* The shadows of aircraft are not drawn yet.
* All names and signs in view are shown however far away they are; zoomed far out they can crowd the view.
* The tile below the cursor is the first building with a model along the line of sight (using the box around its model), or else the terrain; bridges, vehicles and buildings drawn with sprites are not looked at.
* What is in front of what is decided per sprite, so now and then a vehicle shows through the corner of a building it passes behind.
* Buildings without a model that are made of several sprites or cover several tiles can come apart when the camera is rotated, and tiles on half-tile foundations draw their ground as an upright sprite too.
* Transparency options of the 2D game are followed, but see-through sprites are left out rather than drawn see-through.
* Tiles with animations that have no model (some industries with NewGRF graphics) rebuild their whole chunk of 64x64 tiles for every animation step; building one chunk takes about 1.5 ms.
* Sprites far away flicker a little because they have no smaller versions yet.
* The ground of industries, level crossings, depots, NewGRF stations and other buildings is still made from the sprites of the 2D game where it is not plain landscape, so it looks coarser than roads and the landscape.
* The textures of 3D models only change their colour (no normal maps, no see-through parts), vehicles with a model turn in steps of 45 degrees like the sprites do, and liveries other than the company colour are not followed.
* It has been run on Linux with Mesa's software Vulkan and OpenGL drivers, and on Windows with the Direct3D 11 video driver.

## Roadmap

Textures for the landscape, the coast, the water, roads and the ground beside tracks are in. Next, in this order unless plans change (agreed 2026-11-17):

1. Fix what turns up while playing the game in 3D (second round done 2026-11-18: title screen, map generation, river banks, town stop pavements, small view flicker, 3D pictures in construction windows).
2. Replace the last ground made of 2D sprites (done for level crossings, depots, industries with models and NewGRF stations with models; half-tile foundations remain).
3. Textures for the 3D models (done: surface textures by material; lettering, liveries and textures drawn for single models can follow).
4. Day and night: light that follows the time of day, lit windows and lamps. Done, see "Day and night".
5. Weather and seasons: snow, rain, autumn colours. Done, see "Seasons and weather".

## Changing the shaders

The shaders are compiled ahead of time into `shaders/*.bin.h`, so a normal build needs no shader compiler.
After changing a `.sc` file, build bgfx's `shaderc` tool and run:

```
src/viewport3d/shaders/compile_shaders.sh /path/to/shaderc /path/to/bgfx/src
```
