# Proposal: trolleybuses and tramways

Status: design proposal, no code yet. Written 2026-11-18 against the fork at `viewport3d` (0c4f477) and the energy transport proposal (powerlines, branch `energy`), which this one builds on. Revised the same day with Tiago's decisions, which are listed in section 7.

## 1. Summary

Electric street transport becomes a real part of the game: trams on rails in the street and trolleybuses under a pair of wires, with their own vehicles in every climate, their own overhead wiring, and, once the energy feature exists, power that has to come from somewhere. A tram or trolleybus line is cheaper to run and better for a town than a bus line, but it costs more to build, it is tied to its wires, and it stops being cheap when the power behind it is short.

OpenTTD already has trams, so most of the plumbing is there. What is missing is everything that makes them a different kind of transport rather than a bus on rails:

| | OpenTTD today | This proposal |
|---|---|---|
| Tram track | Built-in tram road type (`ELRL`) with wires, plus any from NewGRF | Kept, plus reserved tram track and a non-electric tram type for horse trams before 1890 |
| Depots | Road depot and tram depot | Three kinds: road depot, tram depot and a new trolleybus depot. Stops stay shared. |
| Trolleybus road | Only from a NewGRF road type (usually labelled `ELRD`) | A built-in electrified road with a two-wire overhead, in every climate |
| Vehicles | No trams or trolleybuses in the base game; NewGRF only | Original trams and trolleybuses for every climate and era, shipped with the fork |
| What the wire does | Nothing: the catenary flag only draws sprites and decides which vehicles may use the road | Wires carry traction power. With the energy feature they must be fed by substations, and unfed wire means weak vehicles |
| Trolleybus behaviour | A bus that happens to be restricted to one road type | Cannot overtake on the wire, needs wire on every tile, pays for wire junctions, turning on the spot is a setting |
| Towns | Treat a tram like any road vehicle | Electric stops are quieter and cleaner: a small rating and growth bonus where they are served |
| Running cost | Fixed per vehicle | Part of it becomes energy bought from the grid, so owning the power network makes electric transport cheaper |
| 3D view | Tram rails, single-wire catenary and poles (catenary ids 4 and 5) | Two-wire trolley catenary, junction and crossing pieces, tram and trolleybus models with pantographs and trolley poles that follow the wire |

The main design choices, each explained below:

| Choice | Proposal |
|---|---|
| New transport mode or road type? | Road types. Trams stay `RoadTramType::Tram`, trolleybuses run on a road type of `RoadTramType::Road`. No new vehicle type, no new tile type. Depots are the exception: tram and trolleybus depots are their own kinds, separate from the road depot. |
| How vehicles know they are electric | From their road type: a road type with catenary is electric. Nothing new on the engine. |
| Base vehicles | Shipped as a NewGRF built from the OpenGFX3D models, not added to the base engine tables. |
| Power | Traction wire is a new kind of consumer in the energy network. A substation feeds wire within a feed distance along the line. |
| No power | Off by default (wires always live, as today). With the energy feature on and the setting on, vehicles lose power by the share missing, down to a quarter. |
| Trolleybus wires vs tram wire | A catenary style on the road type: single wire (tram) or pair of wires (trolleybus). Shared streets get both, with crossing pieces where they meet. |

## 2. What exists today, and what it means for this design

Checked in the fork at `viewport3d`:

- `RoadTramType` splits every road tile into a road half and a tram half (`road_type.h`). A tile can carry one road type and one tram type at the same time, so a street with both trams and trolleybuses already fits the map.
- The built-in road types are `ROAD` and `ELRL` (the tram, `table/roadtypes.h`). The tram has `RoadTypeFlag::Catenary` and `RoadTypeFlag::NoHouses`.
- The catenary flag is graphics plus compatibility. `DrawRoadTypeCatenary()` in `road_cmd.cpp` draws wires, and `powered_roadtypes` decides which road types a vehicle can drive on (`roadveh_cmd.cpp`, `v->compatible_roadtypes = rti->powered_roadtypes`). Nothing anywhere asks whether the wire carries power.
- `RoadVehicleInfo` has a `roadtype` and the usual power, weight, tractive effort and air drag, so trams and trolleybuses already get realistic acceleration from `GroundVehicle`.
- The base game has no tram or trolleybus vehicles. Testing trams in the cloud needs a temporary local `HasRoadTypeAvail` hack (project notes).
- The 3D view draws tram rails and tram wires with poles (`AddRoadWireModels()` in `src/viewport3d/terrain_mesh.cpp`, catenary model ids 4 and 5), only for road types whose sprites come from the base set.

So the map and vehicle code need little. The new work is in the base content (road type, vehicles, models), a handful of trolleybus rules, and the link to power.

## 3. Gameplay

### 3.1 Trams

Trams keep working as they do today: track built along or beside roads, or on its own, drive-through tram stops, tram depots, reversing at the end of the line (trams are double-ended).

New:

- Base vehicles in every climate (section 4), from the first electric trams to modern low-floor articulated trams.
- Trams draw power from the wire (3.4).
- Trams are the high-capacity end of street transport: long articulated units carry two to three times a bus, and accelerate well, but cannot leave their track.
- Reserved track: a second built-in electric tram type, "tram on its own track" (grass track or ballast), that towns never build houses beside and that has a higher speed limit (proposed 80 km/h against 60 km/h for street track). Vehicles of either kind run on both. This gives a reason to build a faster tram line out of town, like a light railway (decided).
- Horse trams before electric ones (decided): a non-electric tram type with no wires and slow horse-drawn vehicles from 1860, until the electric trams arrive in 1890. A line is upgraded by converting its track to an electric tram type, and electric trams cannot run on unwired horse tram track. Horse trams may run on electric track.

### 3.2 Trolleybuses

A trolleybus is a bus that takes power from two overhead wires, one out and one back, through two poles on its roof. That is what makes it different from a tram (whose rails are the return path) and from a bus.

- A new built-in road type, "Electrified road" (label `ELRD`, the label NewGRF road type sets already use), with two-wire overhead. It is a road, not a tram track: buses, lorries and cars use it like any road, and towns may build houses along it.
- Building it: the road tool gets the electrified road as a road type, and the convert-road tool turns an existing road (also a town's road) into electrified road, the same way rail is electrified. Wiring a town road needs the usual local authority check and lowers the rating a little during building, like road works.
- Trolleybuses drive only on electrified road (via `powered_roadtypes`), so a route must be wired from end to end, depot included. They are built and serviced only in trolleybus depots (3.6).
- Rules that make them feel like trolleybuses:
  - No overtaking while on the wire. The poles cannot pass another trolleybus's poles. A trolleybus behind a slow one waits. Other road vehicles may still overtake it.
  - Turning back is a game setting (decided), "Trolleybuses may turn around on a plain road": on (default) lets them U-turn anywhere as road vehicles do today, which never traps a vehicle; off makes them turn only at a road end, a turning loop, a junction or a drive-through stop at the end of the line, which is how real trolleybus networks work and asks the player to build loops.
  - Junctions cost more to wire: a tile where electrified road branches gets frogs and crossings in the wire, so it costs extra (proposed +50% of the wire price) and shows a junction piece in 3D.
- Trolleybuses are wired only: no vehicle runs off the wire in the first version (decided). Battery trolleybuses are on the roadmap (section 9).
- Why build them, against buses: cheaper to run (energy instead of diesel), much longer life, better on hills (high tractive effort from standstill), and better for the town (3.5). Against: wire costs money and upkeep, and the route is fixed.

### 3.3 Shared streets

- A street can carry tram track (tram half) and electrified road (road half) together. It then gets both wires: the tram's contact wire over the track and the trolleybus pair beside it. The 3D view and the sprites draw a crossing piece where a tram wire crosses trolley wires at a junction, which real cities need and which makes these streets look right.
- Tram track on plain road and electrified road over tram track both stay possible, as today.
- Level crossings with electric railway: the rail wire and the street wires cross at different heights. Allowed, with a crossing piece in 3D.
- Bridges and tunnels carry the wires for both, as the 3D view already does for tram bridges.
- Powerlines from the energy proposal may pass over tram and trolleybus streets (already allowed there, they are low structures). Their wire hangs higher, so nothing new is needed.

### 3.4 Power

This is where the proposal ties into energy transport. Everything here applies only while the energy feature is on. With it off, wires are always live and nothing changes from today.

**Traction wire is a consumer.** Wired road and tram tiles are a new kind of consumer in the energy network, next to industries and houses. This is the same idea as the energy roadmap item "electric railways that draw power", so both should be built as one mechanism: traction consumers, used by electric rail, trams and trolleybuses alike.

**Feeding the wire.**

- A substation from the energy proposal feeds the street wires around it. No new building is needed, which keeps the build toolbar small. A separate traction substation was considered and left out (decided).
- What a substation feeds: every wired tile that is joined to a wired tile inside its catchment by continuous wire, up to a feed distance along the wire (proposed 12 tiles, a setting). Real tram and trolleybus lines have a feeder every one to two kilometres, so a long line needs several substations along it, which is the gameplay of placing them.
- Wire sections: a group of wired tiles fed by the same set of substations is one section. Two substations feeding the same section share its demand evenly, as for other consumers.
- Rail track wire, tram wire and trolley wire are separate circuits and never feed each other, even on a level crossing.

**Demand.**

- A section draws an idle amount per wired tile (heaters, signals, rectifier losses), proposed 0.02 MW, plus what the vehicles on it draw.
- A vehicle draws in proportion to its power while it is moving and under power, and a little (lights, heating) while stopped. Averaged over the 256-tick balancing step, as the energy proposal balances: the section keeps a running sum of power used by its vehicles over the step, so demand follows real traffic.
- Starting numbers: a tram of 300 hp draws about 0.2 MW on average in service, a trolleybus about 0.1 MW, so a busy town line of 10 trams wants about 2 MW, a small share of a power plant. Electric rail, when it comes, will draw far more.

**Supply follows the energy rules.** Sections are served nearest plant first, with loss by distance, and when the network is short the farthest consumers go short first, as decided for energy.

**When a section is short.**

- The energy rule "no power means no bonus, never a penalty" is about industries and towns. Vehicles are different: an electric vehicle without power cannot simply run as before, and the energy roadmap already says electric trains on unpowered track lose performance. So for traction:
- A setting, "Electric vehicles need power from the grid", off by default (decided), only shown when the energy feature is on.
- When it is on, a vehicle on a section that gets less than its demand has its power cut by the share that is missing, down to a floor of a quarter of its power. A quarter keeps vehicles moving (no deadlocks at stations, no stuck level crossings) but makes them slow and late.
- A news message tells the company once when one of its lines goes short, naming the line ("Trams on Sunningbury line 3 are short of power: 40% supplied").
- A wired tile that no substation feeds at all counts as 0% supplied. With the setting on, a new line therefore needs a substation before vehicles run properly, which the build tool warns about.

**Money.**

- Traction energy is bought by the company that owns the vehicles, at the same price per unit the energy feature pays for deliveries. It shows as a new running cost line, "Traction energy", and the vehicle's fixed running cost is lower to make room for it (proposed: a third of a comparable bus's running cost stays fixed, the rest becomes energy at normal traffic).
- If the energy comes over that same company's network from a plant it feeds, it is also paid the energy sale for it. The two cancel apart from loss, so a company that builds its own grid runs its trams and trolleybuses almost on fuel cost alone. A company that relies on another's grid pays the energy price and the line fee.
- With the energy feature off, electric vehicles have an ordinary fixed running cost, a little below a bus.

### 3.5 Towns

- Clean transport: each tram or trolleybus stop served in the last month by an electric vehicle gives the owning company a small rating gain with that town each month (proposed +5 per stop, capped at +50), the way planting trees does. Diesel buses get nothing extra.
- Passengers prefer it: houses within the catchment of a stop served by trams or trolleybuses produce up to 5% more passengers (setting), on top of any power bonus the town gets from the energy feature.
- Towns building tram lines themselves is left out of the first version. Towns already build roads; a later "town tramway" feature could have big towns lay tram track along their main streets when a company runs trams there (roadmap).
- Wiring a street does not stop town growth: electrified road allows houses beside it, and tram track keeps today's `NoHouses` rule for reserved track only (street track allows houses, as now).

### 3.6 Stops and depots

**Depots are separate (decided).** There are three kinds, each its own building in the build toolbars and its own look:

| Depot | Builds and services | Built from | Look |
|---|---|---|---|
| Road depot | Buses, lorries and other road vehicles that need no wire | Road toolbar | As today |
| Tram depot | Trams, horse trams included | Tram toolbar | Tram shed with tracks into the hall, wires inside for electric tram types, stables for horse tram track |
| Trolleybus depot | Trolleybuses only | Road toolbar, only on electrified road | Bus garage with the trolley wire pair running in over each door |

- A trolleybus is never sent to, serviced in or bought from a road depot, and a bus or lorry is never sent to a trolleybus depot, even though both drive on electrified road. Servicing, "go to depot" orders and the depot finder all respect this.
- Tram depots already exist and are already separate from road depots, because they belong to the tram half of the road. What is new is the trolleybus depot and the depot look by tram type.
- A trolleybus depot must face electrified road, so its doorway is wired.

**Stops are shared (decided).** Tram stops and bus stops already exist as drive-through and bay road stops, and stay that way:

- A trolleybus stop is a bus stop on electrified road. Buses and trolleybuses use the same stops, so a street can carry both.
- Bay stops on electrified road get wire in the bay, at the wire price.
- New looks (3D and 2D): a tram platform with a shelter and ticket machine for each era, an island platform for trams in the middle of the street, and a trolleybus stop with a shelter. These are graphics on the existing stop types, chosen by the road type of the tile, so no new station types.

### 3.7 Costs

Shown relative to a piece of plain road, all tunable through new price entries appended to the end of the price list:

| Item | Proposed cost |
|---|---|
| Electric tram track, per piece | as today |
| Reserved tram track, per piece | 1.2x electric tram track |
| Electrified road, per piece | 2.5x road (two wires and poles) |
| Wire junction on electrified road | +50% for the tile |
| Converting road to electrified road | the difference, as for rail |
| Upkeep | road and tram infrastructure counts as today, electrified road counted as its own road type in the infrastructure window |

## 4. Vehicles

All original designs and names, drawn by the OpenGFX3D generator, in the same style as the existing base vehicles. Numbers are a first guess for playtesting; speeds in km/h.

**Trams** (temperate, sub-arctic and sub-tropical, with livery differences by climate):

| Name (placeholder) | Years | Speed | Capacity | Notes |
|---|---|---|---|---|
| Hackney horse tram | 1860 to 1905 | 12 | 30 passengers | Runs on horse tram track and on electric track. No wire. |
| Albion single-truck tram | 1890 to 1935 | 30 | 40 | Open platforms, trolley pole |
| Corridor bogie tram | 1925 to 1965 | 45 | 60 | Enclosed, bow collector |
| Meridian articulated tram | 1960 to 2000 | 60 | 120 | Two sections, pantograph |
| Vela low-floor tram | 1995 on | 70 | 180 | Three or five sections |
| Works tram (mail) | 1900 on | 40 | 30 bags | Mail carrier, so trams can carry mail like buses do |

**Trolleybuses** (all three climates):

| Name (placeholder) | Years | Speed | Capacity | Notes |
|---|---|---|---|---|
| Penrose trolleybus | 1930 to 1970 | 50 | 40 | Rounded body, two poles |
| Castor trolleybus | 1965 to 2005 | 65 | 60 | Square body |
| Lyra articulated trolleybus | 1990 on | 70 | 100 | Two sections |

**Toyland**: a toy tram and a toy trolleybus with the climate's usual colours, in the same years.

They all use company colours. Trams and articulated trolleybuses are articulated road vehicles, which OpenTTD already supports.

**How they are shipped.** Adding entries to the base engine tables (`table/engines.h`) changes the original engine numbering that savegames and NewGRF overrides depend on. Instead, the vehicles are a NewGRF, "OpenGFX3D Trams and Trolleybuses", built from the OpenGFX3D repository with NML, shipped beside the base graphics and turned on by default in new games. The 2D sprites in it are rendered from the same models from the classic angle (`preview.py` already draws from that angle), the same plan as the 2D sprites for energy. The 3D view finds the models for these vehicles by GRF id and local id in the model index (a new `newgrf_vehicles` section of `opengfx3d.json`), which is also the start of 3D models for other NewGRF vehicles.

## 5. Infrastructure looks

- Tram wire: one contact wire over each track, on poles beside straight track or span wires between buildings in towns. Exists in 3D (catenary ids 4 and 5). One look for all eras for now; era looks are on the roadmap (section 9).
- Trolleybus wire: a pair of wires about half a metre apart over each lane, on poles every other tile, with span wires across the road in towns. At junctions: frogs (the switch where the pair splits) and crossings (where two pairs cross); a tile with three or four roads gets a junction piece.
- Shared streets: both sets, with a crossing piece where tram and trolley wires meet.
- Feeder cables: a small feeder box beside the road where a substation's feed reaches the wire (a marker in the power view only, to keep streets clean).
- In tunnels and on bridges: wires hang from the structure, no poles.

## 6. How it fits the code

### 6.1 Road types

- `table/roadtypes.h` gains built-in entries:
  - `ELRD` electrified road: `RoadTramType::Road`, `RoadTypeFlag::Catenary`, `RoadTypeFlag::TownBuild` off (towns do not build it unless a later feature lets them), powered road types including plain `ROAD` vehicles, so buses and lorries can use it.
  - Reserved tram track (label proposal `ELRR`, to be checked against the NewGRF road type label list so it does not clash): `RoadTramType::Tram`, catenary, `NoHouses`, higher speed limit.
  - Horse tram track (`RAIL` label, which NewGRF sets use for non-electric tram track): `RoadTramType::Tram`, no catenary. Electric tram types list horse tram vehicles as powered on them, so horse trams can run on upgraded track, but not the other way round.
- Built-in road types use slots in the 64-entry `RoadType` table, leaving plenty for NewGRF sets. NewGRF road type sets that define the same labels take over as they do today.
- The catenary flag stays the single meaning of "electric". `GetRoadCatenaryStyle()` (none, single contact wire, trolley pair) tells drawing which wire to draw: single for tram types with catenary, pair for road types with catenary, the same for NewGRF road types, which matches what sets draw. No new field is needed on `RoadTypeInfo`.
- `HasRoadTypeAvail` makes the new types available from the year their first vehicle appears, as today for any road type.

### 6.2 Vehicles and rules

- "Is electric" is `HasRoadCatenary(GetRoadTypeInfo(e->u.road.roadtype))`, so NewGRF trams and trolleybuses work with the power rules without any change to them.
- Overtaking: `roadveh_cmd.cpp` already decides when a road vehicle may overtake (`RoadVehCheckOvertake` and the `overtaking` field). A trolleybus (electric vehicle on a road-half road type) never starts overtaking.
- Reversing: with the setting `trolleybus_turn_on_road` off, the U-turn on a plain road tile is refused for trolleybuses, so the pathfinder must find a loop, junction or road end. YAPF road already costs turning around, so this is a rule in the turn-around check plus a cost. If a trolleybus is trapped with no way to turn (a wired dead end without a road end), it is allowed the U-turn anyway and the company gets a news message, so a bad layout never freezes a line.
- Depots: road depot tiles already carry a road type. A depot built with an electrified road type (one whose catenary style is the trolley pair) is a trolleybus depot. The checks that decide where a road vehicle may be built, serviced or sent (`CmdBuildRoadVehicle` and the depot test in `roadveh_cmd.cpp`, the road depot search in YAPF, `CheckIfRoadVehNeedsService`) gain one rule: a trolleybus depot takes only electric road-half vehicles and a road depot takes none. Tram depots need nothing, since trams already only use tram depots. The depot window and build toolbar show the trolleybus depot as its own button and name.
- Power cut: `GroundVehicle` uses cached power for acceleration. A traction factor (0 to 255) on the vehicle scales the power used in `GetAcceleration()`. It is updated when the vehicle enters a tile whose section changed, which is one read of the power layer bit (6.3).

### 6.3 Power

All in `src/power/` from the energy proposal:

- The power layer's spare bit 15 becomes "traction wire on this tile is fed" (rebuilt, never saved). Vehicles read it when they enter a tile, so the per-tick cost is one array read.
- `TractionSection` is a new consumer type beside industries and houses. Sections are found by flood fill along wired road, tram and rail tiles from each substation, limited by feed distance, when the network or the wire changes (the same moments the energy network is rebuilt), and rebuilt on load.
- Each section keeps a running sum of vehicle draw for the balancing step. A vehicle adds its draw to the section of the tile it is on, once per tick while moving under power (an integer add); the section id is found from the tile through a small hash map built with the sections.
- Balancing treats sections as consumers in the pair list, so the nearest-plant-first walk, loss and short-supply order are unchanged. The share supplied is written to the section and, from there, the fed bit of its tiles.
- Electric rail plugs into the same code later: rail tiles with catenary are wired tiles of a third circuit, trains add their draw the same way.

### 6.4 Money, towns, settings

- `ExpensesType` gains "Traction energy". The energy sales income already planned covers the other side.
- Town rating gain and passenger bonus: in the monthly town loop (stops served by an electric vehicle last month, kept as a flag on the road stop) and `TileLoop_Town` (houses in a qualifying stop's catchment), both integer.
- Settings (game settings, synchronised): `electric_vehicles_need_power` (off), `traction_feed_distance` (12), `trolleybus_turn_on_road` (on), `electric_stop_rating_bonus`, `electric_stop_passenger_bonus`.

### 6.5 Savegame

- Through the fork-only `FORK` chunk from the energy proposal: the served flag on road stops and the new settings. Sections are rebuilt, not saved. Trolleybus depots need nothing new, since they are known from the depot's road type.
- New built-in road types are saved by label, as all road types are, so old saves load unchanged and saves with `ELRD` from a NewGRF keep working.

### 6.6 Scripts

- `ScriptRoad` already handles road types, so AIs can build electrified road and tram track with no change.
- `ScriptPower` (energy) gains traction queries: is a tile's wire fed, the share supplied on a section.
- `ScriptRoadVehicle` gains an "is electric" query.

### 6.7 3D renderer

- `terrain_mesh.cpp`: `GetRoadWireBits()` and `AddRoadWireModels()` read `GetRoadCatenaryStyle()` and pick tram or trolley wire models. New catenary ids for trolley wire, poles, span wires, the junction piece and the tram-trolley crossing piece. Span wires between houses where the street has buildings on both sides instead of poles.
- Vehicles: models for the vehicles in section 4, found through the new `newgrf_vehicles` section of the model index.
- Current collectors: trolley poles and pantographs are separate named nodes in the .glb (`pole*`, `pantograph*`). The renderer raises them to the wire height of the tile the vehicle is on, and lowers trolleybus poles inside a depot.
- Depots: a tram shed model (with an electric and a horse version) and a trolleybus garage model with the wire pair over each door. The energy proposal already asks for named nodes that turn (`rotor*`), so this extends the same mechanism to nodes that tilt.
- Night: lit windows inside trams and trolleybuses, and an occasional spark at the collector on frosty nights, both with the existing day/night glow rule. Snow on wires and roofs with the existing weather shader.
- Stops: shelter and platform models chosen by the road type of the stop tile.

### 6.8 2D view

- Trolley wire sprites are not in any base set. They are rendered from the 3D wire models into a small GRF, together with the vehicle sprites (section 4) and the energy feature's pylons and substations, so there is one rendered-sprites GRF for the fork.
- Until then, a placeholder (the tram wire sprites drawn twice with an offset) is enough to develop against.

## 7. Decisions taken

Tiago's answers of 2026-11-18. The sections above already follow them.

1. Trams and trolleybuses are road types, not a new transport mode, and share stops and the rest of the road code. Depots are the exception: tram depots and trolleybus depots are separate from the regular road depot (3.6).
2. Horse trams run before 1890, on a non-electric tram type.
3. Reserved tram track is a second electric tram type, for out-of-town light rail.
4. The energy substation feeds street wire within a feed distance along the line (12 tiles to start with). No separate traction substation.
5. Whether trolleybuses may turn around on a plain road is a game setting.
6. Trolleybuses are wired only for now. Battery trolleybuses go on the roadmap.
7. "Electric vehicles need power from the grid" is off by default. When on, power falls with the share missing, down to a floor of a quarter.
8. Wires have one look for now. Era looks go on the roadmap.
9. Running cost: a third fixed, the rest bought as energy when the energy feature is on.
10. Traction consumers are built once, here, so electric railways drawing power (energy roadmap) become a small step afterwards.

## 8. Phased plan

Each phase ends with something playable that can be merged. Sizes are relative, for one person part-time. Phases 1 to 3 do not need the energy feature; phase 4 needs energy phase 3 (economy).

| Phase | What | Size |
|---|---|---|
| 1. Trolley wire | `ELRD` electrified road as a built-in type with `GetRoadCatenaryStyle()`, convert-road support, junction cost, placeholder 2D wires, 3D trolley wire models with junction and crossing pieces. Tested with the temporary road type hack or a test NewGRF bus. | Medium |
| 2. Vehicles and depots | OpenGFX3D models for trams and trolleybuses (all eras and climates, horse trams included), the NML vehicle GRF with rendered sprites, the `newgrf_vehicles` model index, current collectors that follow the wire. Horse tram track and reserved tram track. The trolleybus depot as its own kind, tram shed and trolleybus garage models, stop looks. | Large |
| 3. Trolleybus rules | No overtaking on the wire, the turning setting, tests with busy town routes for stuck vehicles. Town rating and passenger bonus for electric stops. | Small |
| 4. Traction power | Traction sections in `src/power/`, feed distance, vehicle draw, short supply cutting power, "Traction energy" running cost, news message, power view showing fed and unfed wire in 2D and 3D. Built so electric rail can join later. | Large |
| 5. Finish | Script API additions, balance pass from play, documentation. | Small |

Phase 1 and the models in phase 2 can start any time, independently of energy. Progress: phase 1 is on branch `trolley-tram` (OpenGFX3D branch `trolley-tram` for the wire models). Electrified road can be built from 1930, before any trolleybus exists, so the wires can be laid ahead of phase 2. Phase 2 is there too: horse tramway (`RAIL`) and reserved tramway (`ELRR`, from 1920) as built-in tram types; trolleybus depots that only take trolleybuses, and road depots that refuse them; the vehicles as the NewGRF `OG3T` (`opengfx3d_trams.grf`, vehicle ids 100 to 114, 2D sprites drawn from the 3D models), added once to the NewGRFs of new games; and in 3D, trams with pantographs or poles, trolleybuses with poles on the wire, tram tracks and contact wires in both lanes, a tram shed and a trolleybus garage. Not done in phase 2: 2D depot sprites of their own, and a Windows build. Phase 3 is there as well: trolleybuses never overtake; the setting `vehicle.trolleybus_turn_on_road` (on by default) keeps them, when off, from turning around except at a road end, a junction, a drive-through stop or a depot, so the pathfinder uses turning loops, and a trolleybus that has no other way still turns and its owner gets a message once; each station near a town that a tram or trolleybus served last month gives its company `economy.electric_stop_rating_bonus` rating a month (5, up to ten stations), and houses with such a station in their catchment make `economy.electric_stop_passenger_bonus` percent more passengers (5). The "served" marks live in two spare bits of the station's saved `had_vehicle_of_type` (hidden from NewGRF), so phase 3 needs no savegame change; the served flag can move to the `FORK` chunk with phase 4. Phase 4 is the part that makes this a new kind of transport rather than new vehicles, and it should be designed together with the electric railway item on the energy roadmap. Phase 4 is on branch `trolley-tram-energy` (trolley-tram merged with energy; OpenGFX3D branch of the same name). A power substation feeds the tram and trolleybus wire in its reach, and from there along continuous wire for `economy.traction_feed_distance` tiles (12); road-half and tram-half wire are separate circuits, and tunnels and bridges carry the wire through. Every fed tile asks 0.02 MW, and every tram or trolleybus on it about a quarter of a MW per 300 hp while moving (a tenth of that standing), split between the substations that feed it; the energy balancing serves this like any other consumer. The share each piece of wire gets is kept in spare bits of the saved power layer (bits 20 to 27), so no savegame change. With `economy.electric_vehicles_need_power` on (off by default), a vehicle gets the speed of its wire's share, at least a quarter, and a quarter on wire no substation feeds; its owner gets a message once. Owners pay for the energy their vehicles take, at the energy sale price, as road vehicle running costs, and the fixed running cost of trams and trolleybuses is a third. It is switchable: the code is built only when the energy transport is in the tree and `OPTION_TRACTION_POWER` is on (default), and without it `traction.h` makes the wires always live, so branch `trolley-tram` carries the same code and builds and plays without the energy transport. Not done yet: a view of fed and unfed wire in 2D and 3D, the share in the land info window, and a Windows build.

## 9. Roadmap after the first version

- Battery trolleybuses (decided for the roadmap): from about 2010, models with a battery that charges under the wire and lets them run a few tiles off it (proposed 8 to 24 tiles by model), so a route can leave the wire for a short stretch, as real "in motion charging" lines do. They would also run on their battery on a section short of power until it is flat. Needs a fork-only engine property for range, a saved charge on the vehicle, and a fork-only flag that lets those models use plain road.
- Era looks for wires and poles (decided for the roadmap): lighter span wires and ornate poles for early trams, heavier modern poles, with the other model polish.
- Towns that build tram lines along their main streets when a company runs trams there.
- Tram-trains: vehicles that run on both tram track and electric railway, through a fork-only compatibility between a tram type and a rail type at special junction tiles.
- Guided busways and bus lanes, as a cheaper middle step between bus and tram.
- Overhead-wire lorries on electrified road (electric freight on highways), which the electrified road already allows if a set provides the vehicles.
- Line colours and route maps for tram and trolleybus lines in the town window.
