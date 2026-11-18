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
* The power layer (`_power_layer`, `power_map.h`): 16 bits for every tile (32 since phase 4), allocated with the map.
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

## Phase 3: the energy economy

Code: `src/power_economy.cpp`. Power is counted in tenths of a MW. Every 256 ticks (a "balancing step",
about 8.7 per month) the networks share out power.

- **Power plants.** An industry that accepts coal or oil and produces nothing is a fuel power plant
  (the coal power station, the oil power station of other climates, and NewGRF industries of the same
  shape). Delivered fuel goes into a store: a tonne of coal gives 2 MW for a month, 1000 litres of oil
  3 MW. The store holds two months of full output. A coal plant gives at most 600 MW, an oil plant 450 MW.
  Plants burn only what they sell; no fuel, no power.
- **Consumers.** Every other industry needs idle power plus power for what it made last month
  (organic 2 MW + 4 MW per 100 units, extractive 8 + 18, processing 10 + 25, no output 3 MW).
  Each building needs 0.1 MW plus 1 MW per 100 inhabitants, 10% more while its town grows, and 0.5 MW
  while it is being built.
- **Substations** reach industries and buildings within `economy.power_catchment` tiles (default 4,
  a 9x9 square), shown around the cursor when building one. Something reached by several substations is
  split evenly between them.
- **Networks** are all power tiles joined by spans of the same owner. They are found again from the map
  whenever a power tile changes (`_power_network_version`); nothing about them is saved.
- **Sharing out**: all pairs of a plant-side and a consumer-side substation of a network, nearest first
  (line length: 24 per straight tile, 34 per diagonal tile). Loss is 1% at the substation for a local pair,
  2% between two substations, plus 1% for every 16 tiles of line, at most 60%. When there is not enough
  power, the farthest consumers go short first.
- **Bonus**: an industry or town with all the power it needs makes `economy.power_bonus`% more
  (default 10%, 0 to 50); partial power gives part of it. Industries make more cargo; towns make more
  passengers and mail and grow faster. No power gives no bonus and no penalty.
- **Income**: the owner of the substation that takes power from a plant is paid for what arrives
  (`Price::PowerSale`, base 30 per 10 MW per step, so about 2,600 a month for 100 MW at 1990 prices).
  It shows as "Energy Sales" under revenue in the finance window (`ExpensesType::EnergySales`,
  savegame version `EnergySales`; older savegames load with their 13 kinds of expenses moved into place).
- **Windows**: the industry window shows what a plant sells and its days of fuel, or what a consumer
  gets and its bonus; the town window shows the same for the town.
- **Settings**: Environment > Energy.
- **Savegame**: chunk `PWRE` holds the plants' fuel and the last step's figures of industries and towns.
- **Tests**: console `power_build info` lists plants, consumers with power, towns and company income;
  `power_build fuel <industry> <amount>` delivers fuel in single player.

## Phase 4: underground cables

- **Cables** run tile by tile under anything, along an axis or a map diagonal (cable tool, drag like
  the line tool). Bits 0..7 of the power layer say in which directions a cable leaves a tile; bits
  16..19 hold its owner. The layer is now 32 bits per tile; chunk `PWRL` stores 32 bits and still loads
  the 16-bit layer of older savegames.
- Cables of one company join wherever they meet on a tile, straight or diagonal, and may cross between
  tiles without joining. They connect to the network at an **underground substation** or a
  **transition pylon**; a cable under an ordinary pylon, overhead substation or wire does not connect to it.
  A cable may end anywhere, but an end that reaches no substation or transition pylon carries nothing.
- **Prices**: cable 1,900 per straight tile (about 8 times overhead wire with its pylons), 1.4 times on
  a diagonal, twice under water; removing 100. An underground substation costs twice an overhead one.
  A transition pylon costs three pylons; a pylon can be turned into one, keeping its wires.
- **Loss**: cable counts half as long as overhead line (12 per straight tile, 17 per diagonal), so it
  loses 0.5% per 16 tiles.
- **Look**: marker posts with orange caps stand over cables on open land (model `power` 5 in OpenGFX3D,
  sprite 23 of power.grf). The power network view (toolbar button, turned on by the cable tool) shows
  the cables as orange lines (model 6, sprites 24..31).
- **Company gone**: its cables go to the buyer or are removed with its other property
  (`ChangePowerCableOwner`).
- **Tests**: `power_build transition|usubstation <x> <y>`, `power_build cable|removecable <x1> <y1> <x2> <y2>`.

## Phase 5: weather and time in the game

- **Game state**: `src/weather.cpp` works out the season, the weather and the hour of the day with
  integers, from the date, `TimerGameTick::counter`, the map seed and game settings only. Nothing is
  saved and every computer in a game gets the same answer. `GetGameWeather()` caches it per tick.
- **Settings** (Environment > Weather and time): `economy.weather` (off, some, much),
  `economy.day_length` (minutes of play for a day and a night; 0 for none) and
  `economy.weather_slows_vehicles`.
- **Vehicles**: in heavy rain trains, road vehicles and ships run up to 15% slower, in a thunderstorm 25%
  and in a snowstorm 30% (`GetWeatherVehicleSpeedFactor`, applied in `DoUpdateSpeed` and
  `ShipAccelerate`); running vehicles lose reliability up to 1.5 times as fast in heavy rain and twice as
  fast in a storm (`GetWeatherBreakdownFactor`, in `CheckVehicleBreakdown`). Aircraft are not affected.
- **Status bar**: a panel next to the date shows the hour and the weather, in orange while the weather
  slows vehicles down.
- **3D view**: draws the game's weather and hour; `gui.view3d_weather` and `gui.view3d_night` only choose
  what to draw. The console overrides (`view3d_weather`, `view3d_season`, `view3d_time`) change only the
  looks.
- **Plants**: `GetWeatherSolarFactor`, `GetWeatherWindFactor` and `GetWeatherRiverFlow` give the output
  factors for solar, wind and hydro plants.
