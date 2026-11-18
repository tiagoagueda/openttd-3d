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
* Placeholder 2D graphics: the transmitter sprite for pylons, the power station transformer sprite for
  substations, catenary wire sprites at wire height (diagonals as two conductors).
  Wires over other tiles are drawn from the viewport loop (`DrawPowerWireAbove()`), also for the 3D view's sprite capture.
* Land info shows the number of substations in the network, and the owner of a wire above any tile.
