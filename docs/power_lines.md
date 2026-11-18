# Power lines (energy transport)

Work in progress on branch `energy`. The full design is in the project proposal
(`energy-transport-proposal.md`); this file describes what the code does today.

## Phase 1: overhead lines, pylons and substations (no energy yet)

### Map

* `TileType::Power` (new tile type) holds pylons and substations.
  * m1 bits 0..4: owner.
  * m5 bits 0..1: `PowerTileKind` (pylon, transition pylon, overhead substation, underground substation).
    Only pylons and overhead substations can be built so far.
  * m8: span length in each of the 8 directions, 2 bits each
    (0 = no wire, 1..3 = tiles to the next pylon; `POWER_MAX_SPAN` = 3, so at most 2 tiles lie in between).
* The power layer (`_power_layer`, `power_map.h`): 16 bits for every tile, allocated with the map.
  * bits 0..7: underground cable directions (reserved for phase 4),
  * bits 8..10: `PowerWireKind` of the wire passing over the tile (X, Y, N-S diagonal, E-W diagonal),
  * bits 11..14: owner of that wire.
* Saved in chunk `PWRL` (whole layer). Older savegames load with an empty layer.

### Rules

* Lines run along an axis or a map diagonal, between pylons at most 3 tiles apart.
* Wires join only at pylons or substations: a span cannot pass over a power tile,
  two wires cannot share a tile, and diagonal wires cannot cross over a tile corner.
* Wire height: `POWER_WIRE_HEIGHT` (3) levels above the base of the lower of its two pylons;
  the two pylons may differ by at most 1 level.
* `CheckPowerWireClearance()` decides what a wire may pass over: land, trees, rail, road, water,
  locks and depots, stations other than airports, tunnel portals, objects that allow bridges above,
  and low houses (`IsHouseLowForPowerWire()`: base houses by name, NewGRF houses count as tall).
  Not over industries, airports, oil rigs, bridges, tall houses or tall objects.
* Other code keeps wires clear: industries, airports and objects check `HasPowerWire()` /
  `CheckPowerWireAllowsStructure()`; towns only build low single-tile houses under wires;
  bridges refuse tiles with wires; terraforming may not raise land into a wire.
* Pylons need open land (clear or trees); slopes get a foundation.
  Pylons are not removed automatically by other construction (`DoCommandFlag::Auto`), and refuse terraforming.

### Commands

`CmdBuildPowerPylon`, `CmdBuildPowerSubstation`, `CmdBuildPowerLine` (start, end; places pylons greedily
as far apart as allowed, reusing existing own pylons), `CmdRemovePowerLine` (removes spans with both ends
in the dragged stretch and pylons left without wires). Bulldozing a pylon removes its wires.

### Prices

`Price::BuildPowerPylon`, `BuildPowerWire` (per tile, diagonal x1.4), `BuildPowerSubstation`,
`ClearPowerPylon`, `ClearPowerWire`, `ClearPowerSubstation`, appended after the existing prices.

### Interface and graphics

* Landscaping menu > Power lines opens the power toolbar: line (drag; the end snaps to the nearest
  of the 8 directions), pylon, substation, demolish, remove toggle.
* Wires over other tiles are drawn from the viewport loop (`DrawPowerWireAbove()`), also for the 3D view's sprite capture
  when the model package has no power models.
* Land info shows the number of substations in the network, and the owner of a wire above any tile.

## Phase 2: 3D models and 2D sprites

### 3D view

* Models come from OpenGFX3D (`models_power.py`, index type `power`): 0 pylon, 1 overhead substation,
  2 underground substation, 3 transition pylon, 4 a piece of conductor. The model's x axis follows the line.
* `AddPowerModels()` in `src/viewport3d/terrain_mesh.cpp` places the pylon or substation, turned along the line
  (on a bend halfway, so the cross arm lies on the bisector), and the spans that leave it towards N, NE, E or SE;
  the other spans are added by the tile at their far end. Each span is three conductors `POWER_PHASE` apart at
  `POWER_WIRE_Z` above each foot, plus an earth wire between two pylons, sagging in six pieces.
  The constants must match `WIRE_Z`, `PHASE` and `TOP` in `models_power.py`.
* With power models, the 2D wire sprites are left out of the sprite capture (`_power_wire_sprites`).
* `MarkPowerTileDirty()` also marks the far ends of a pylon's spans, because the wires follow how both ends turn.
* Previews: the pylon and substation tools show a see-through model on the tile under the mouse; dragging a line
  shows see-through pylons and wires where `CmdBuildPowerLine()` would put them (`GetPowerToolPreview()`,
  `AddPowerLinePreview()`), without checking the land.

### 2D view

* `media/baseset/power.grf` holds 23 sprites drawn from the same models by OpenGFX3D's `make_sprites.py`:
  8 bpp at normal zoom and 32 bpp at normal, 2x and 4x zoom. The order is described in that script and in
  `SPR_POWER_*` (`src/table/sprites.h`), which sits just before `SPR_NEWGRFS_BASE`.
* `LoadSpriteTables()` loads it after the base set; without the file (`_power_sprites_loaded` false) the old
  stand-ins are drawn.
* Pylons and substations turn like the 3D models, to one of four directions (`GetPowerSpriteDirection()`).
  Over other tiles one sprite carries all wires of the tile; on a power tile a half wire runs from the pylon to the
  edge of the tile for each span.

### Testing

* Console: `power_build line <x1> <y1> <x2> <y2>`, `power_build pylon <x> <y>`, `power_build substation <x> <y>`
  build for the local company, e.g. from `scripts/game_start.scr`.
