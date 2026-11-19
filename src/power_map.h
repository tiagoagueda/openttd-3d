/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file power_map.h Map accessors for power line tiles and for the power layer above and below every tile. */

#ifndef POWER_MAP_H
#define POWER_MAP_H

#include "tile_map.h"
#include "direction_func.h"
#include "company_type.h"
#include "water_map.h"

/**
 * What stands on a tile of #TileType::Power.
 * Stored in m5 bits 0..1.
 */
enum class PowerTileKind : uint8_t {
	Pylon = 0, ///< A pylon carrying overhead wires.
	TransitionPylon = 1, ///< A pylon where an overhead line goes underground.
	OverheadSubstation = 2, ///< A substation that overhead lines connect to.
	UndergroundSubstation = 3, ///< A substation that underground cables connect to.
};

/**
 * Which way an overhead wire passes over a tile of the power layer.
 * Stored in bits 8..10 of the layer.
 */
enum class PowerWireKind : uint8_t {
	None = 0, ///< No wire above the tile.
	AxisX = 1, ///< Along the X axis (NE - SW).
	AxisY = 2, ///< Along the Y axis (NW - SE).
	DiagNS = 3, ///< Map diagonal from the north corner to the south corner (N - S).
	DiagEW = 4, ///< Map diagonal from the east corner to the west corner (E - W).
};

/** How far apart two pylons may stand at most, counted in tiles (so 2 tiles lie between them). */
static constexpr uint POWER_MAX_SPAN = 3;
/** Height of the wires above the base of the lower of their two pylons, in height levels. */
static constexpr int POWER_WIRE_HEIGHT = 3;

/**
 * The power layer: 32 bits for every tile, next to the map array.
 * Bits 0..7: underground cable leaves the tile in each of the 8 directions.
 * Bits 8..10: #PowerWireKind of the overhead wire passing over the tile.
 * Bits 11..14: company owning that wire.
 * Bit 15: spare.
 * Bits 16..19: company owning the cable under the tile.
 * Bits 20..31: spare.
 * Tiles of #TileType::Power keep their wires in the tile itself (m8), not here; their cables are here like any other tile's.
 */
extern std::unique_ptr<uint32_t[]> _power_layer;

/**
 * Get the kind of overhead wire passing over a tile.
 * @param t The tile.
 * @return The kind of wire, #PowerWireKind::None if there is none.
 */
inline PowerWireKind GetPowerWireKind(TileIndex t)
{
	return static_cast<PowerWireKind>(GB(_power_layer[t.base()], 8, 3));
}

/**
 * Is there an overhead power wire passing over a tile (between two pylons)?
 * @param t The tile.
 * @return True if a wire passes over it.
 */
inline bool HasPowerWire(TileIndex t)
{
	return _power_layer != nullptr && GetPowerWireKind(t) != PowerWireKind::None;
}

/**
 * Get the owner of the overhead wire passing over a tile.
 * @param t The tile.
 * @pre HasPowerWire(t)
 * @return The company that owns the wire.
 */
inline Owner GetPowerWireOwner(TileIndex t)
{
	return static_cast<Owner>(GB(_power_layer[t.base()], 11, 4));
}

/**
 * Set or remove the overhead wire passing over a tile.
 * @param t The tile.
 * @param kind The kind of wire, #PowerWireKind::None to remove it.
 * @param owner The company that owns the wire.
 */
inline void SetPowerWire(TileIndex t, PowerWireKind kind, Owner owner)
{
	SB(_power_layer[t.base()], 8, 3, to_underlying(kind));
	SB(_power_layer[t.base()], 11, 4, kind == PowerWireKind::None ? 0 : (owner.base() & 0xF));
}

/**
 * Get the wire kind for a wire running in a direction.
 * @param dir The direction.
 * @return The kind of wire.
 */
inline PowerWireKind GetPowerWireKindForDir(Direction dir)
{
	switch (dir) {
		case Direction::NE: case Direction::SW: return PowerWireKind::AxisX;
		case Direction::SE: case Direction::NW: return PowerWireKind::AxisY;
		case Direction::N: case Direction::S: return PowerWireKind::DiagNS;
		default: return PowerWireKind::DiagEW;
	}
}

/**
 * Get one of the two directions a kind of wire runs in.
 * @param kind The kind of wire.
 * @pre kind != PowerWireKind::None
 * @return The direction; the other one is its reverse.
 */
inline Direction GetPowerWireDir(PowerWireKind kind)
{
	switch (kind) {
		case PowerWireKind::AxisX: return Direction::NE;
		case PowerWireKind::AxisY: return Direction::SE;
		case PowerWireKind::DiagNS: return Direction::N;
		default: return Direction::E;
	}
}

/**
 * Is this a power line tile (pylon or substation)?
 * @param t The tile.
 * @return True if it is.
 */
/**
 * Get the directions in which an underground cable leaves a tile.
 * @param t The tile.
 * @return Bit set of directions (bit n for Direction n).
 */
inline uint8_t GetPowerCableDirs(TileIndex t)
{
	return _power_layer == nullptr ? 0 : GB(_power_layer[t.base()], 0, 8);
}

/**
 * Whether an underground cable runs under a tile.
 * @param t The tile.
 * @return True if a cable leaves the tile in any direction.
 */
inline bool HasPowerCable(TileIndex t)
{
	return GetPowerCableDirs(t) != 0;
}

/**
 * Get the company owning the cable under a tile.
 * @param t The tile.
 * @return The owner.
 */
inline Owner GetPowerCableOwner(TileIndex t)
{
	return static_cast<Owner>(GB(_power_layer[t.base()], 16, 4));
}

/**
 * Set the directions in which a cable leaves a tile, and its owner.
 * @param t The tile.
 * @param dirs Bit set of directions; 0 removes the cable.
 * @param owner The owner.
 */
inline void SetPowerCable(TileIndex t, uint8_t dirs, Owner owner)
{
	SB(_power_layer[t.base()], 0, 8, dirs);
	SB(_power_layer[t.base()], 16, 4, dirs == 0 ? 0 : (owner.base() & 0xF));
}

inline bool IsPowerTile(Tile t)
{
	return IsTileType(t, TileType::Power);
}

/**
 * Get what stands on a power line tile.
 * @param t The tile.
 * @pre IsPowerTile(t)
 * @return The kind.
 */
inline PowerTileKind GetPowerTileKind(Tile t)
{
	assert(IsPowerTile(t));
	return static_cast<PowerTileKind>(GB(t.m5(), 0, 2));
}

/**
 * Is this tile a substation?
 * @param t The tile.
 * @return True if it is a power tile with a substation.
 */
inline bool IsPowerSubstation(Tile t)
{
	if (!IsPowerTile(t)) return false;
	PowerTileKind kind = GetPowerTileKind(t);
	return kind == PowerTileKind::OverheadSubstation || kind == PowerTileKind::UndergroundSubstation;
}

/**
 * Can overhead wires end at this tile (pylon or overhead substation)?
 * @param t The tile.
 * @return True if wires can be attached.
 */
inline bool CanAttachPowerWires(Tile t)
{
	if (!IsPowerTile(t)) return false;
	PowerTileKind kind = GetPowerTileKind(t);
	return kind == PowerTileKind::Pylon || kind == PowerTileKind::OverheadSubstation || kind == PowerTileKind::TransitionPylon;
}

/**
 * Get the length of the span of wires leaving a power tile in a direction.
 * @param t The tile.
 * @param dir The direction.
 * @pre IsPowerTile(t)
 * @return 0 if no wires leave in that direction, else the distance in tiles to the next pylon (1..#POWER_MAX_SPAN).
 */
/**
 * Whether underground cables can end at and join at a power tile: an underground substation or a transition pylon.
 * @param t The tile.
 * @return True if cables connect here.
 */
inline bool CanAttachPowerCables(Tile t)
{
	if (!IsPowerTile(t)) return false;
	PowerTileKind kind = GetPowerTileKind(t);
	return kind == PowerTileKind::UndergroundSubstation || kind == PowerTileKind::TransitionPylon;
}

inline uint GetPowerSpan(Tile t, Direction dir)
{
	assert(IsPowerTile(t));
	return GB(t.m8(), to_underlying(dir) * 2, 2);
}

/**
 * Set the length of the span of wires leaving a power tile in a direction.
 * @param t The tile.
 * @param dir The direction.
 * @param length 0 for no wires, else the distance in tiles to the next pylon (1..#POWER_MAX_SPAN).
 * @pre IsPowerTile(t)
 */
inline void SetPowerSpan(Tile t, Direction dir, uint length)
{
	assert(IsPowerTile(t));
	assert(length <= POWER_MAX_SPAN);
	SB(t.m8(), to_underlying(dir) * 2, 2, length);
}

/**
 * Does any wire leave a power tile?
 * @param t The tile.
 * @pre IsPowerTile(t)
 * @return True if at least one span is attached.
 */
inline bool HasPowerSpans(Tile t)
{
	assert(IsPowerTile(t));
	return t.m8() != 0;
}

/**
 * Make a power line tile.
 * @param t The tile.
 * @param owner The company that builds it.
 * @param kind What stands on it.
 * @param water The water it stands in, or WaterClass::Invalid on land.
 */
inline void MakePowerTile(Tile t, Owner owner, PowerTileKind kind, WaterClass water = WaterClass::Invalid)
{
	SetTileType(t, TileType::Power);
	SetTileOwner(t, owner);
	SetWaterClass(t, water);
	t.m2() = 0;
	t.m3() = 0;
	t.m4() = 0;
	t.m5() = to_underlying(kind);
	SB(t.m6(), 2, 6, 0);
	t.m7() = 0;
	t.m8() = 0;
}

#endif /* POWER_MAP_H */
