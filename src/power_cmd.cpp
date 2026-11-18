/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file power_cmd.cpp Handling of power line tiles: pylons, substations and the overhead wires between them. */

#include "stdafx.h"
#include "power_cmd.h"
#include "power_func.h"
#include "power_map.h"
#include "landscape.h"
#include "landscape_cmd.h"
#include "command_func.h"
#include "company_func.h"
#include "company_base.h"
#include "economy_func.h"
#include "viewport_func.h"
#ifdef WITH_VIEWPORT3D
#include "viewport3d/view3d_gui.h"
#endif
#include "clear_func.h"
#include "bridge_map.h"
#include "tunnelbridge_map.h"
#include "station_map.h"
#include "rail_map.h"
#include "road_map.h"
#include "water_map.h"
#include "town_map.h"
#include "house.h"
#include "object_map.h"
#include "newgrf_object.h"
#include "transparency.h"
#include "sprite.h"
#include "settings_type.h"

#include "table/strings.h"
#include "table/sprites.h"

#include <set>

#include "safeguards.h"

std::unique_ptr<uint32_t[]> _power_layer;

bool _power_wire_sprites = true;
bool _power_cable_sprites = true;
bool _power_network_view = false;
bool _power_sprites_loaded = false;

/** Allocate an empty power layer for the current map size. Called whenever the map is allocated. */
void AllocatePowerLayer()
{
	_power_layer = std::make_unique<uint32_t[]>(Map::Size());
	_power_network_version++;
}

/** Sprite of the substation placeholder: the transformer building of the base power station. */
static const SpriteID SPR_POWER_SUBSTATION = SPR_IT_POWER_PLANT_TRANSFORMERS;
/** Base sprite of the placeholder wires: the default catenary wires. */
static const SpriteID SPR_POWER_WIRE_BASE = SPR_WIRE_BASE;

/**
 * Length of a step in a direction, in tenths of a tile, for prices and later for losses.
 * @param dir The direction.
 * @return 10 for straight steps, 14 for diagonal ones.
 */
static uint GetStepLength(Direction dir)
{
	return IsDiagonalDirection(dir) ? 10 : 14;
}

/**
 * Find the direction of a step between two neighbouring tiles.
 * @param dx Step along X, -1, 0 or 1.
 * @param dy Step along Y, -1, 0 or 1.
 * @return The direction, or Direction::Invalid for no step.
 */
static Direction GetDirectionOfStep(int dx, int dy)
{
	for (Direction dir : EnumRange(Direction::End)) {
		TileIndexDiffC diff = TileIndexDiffCByDir(dir);
		if (diff.x == dx && diff.y == dy) return dir;
	}
	return Direction::Invalid;
}

/**
 * Step from a tile a number of tiles in a direction.
 * @param tile Start tile.
 * @param dir Direction.
 * @param steps Number of steps.
 * @return The tile reached, or INVALID_TILE when it would leave the map.
 */
static TileIndex StepTile(TileIndex tile, Direction dir, int steps)
{
	TileIndexDiffC diff = TileIndexDiffCByDir(dir);
	return TileAddXY(tile, diff.x * steps, diff.y * steps);
}

/**
 * Height at which the wires of a pylon or substation are attached.
 * @param tile The power tile.
 * @return Height in height levels.
 */
static int GetPylonBaseZ(TileIndex tile)
{
	return GetTileMaxZ(tile);
}

/**
 * Height of the wires of a span between two power tiles.
 * @param a One end.
 * @param b The other end.
 * @return Height in height levels.
 */
static int GetSpanWireZ(TileIndex a, TileIndex b)
{
	return std::min(GetPylonBaseZ(a), GetPylonBaseZ(b)) + POWER_WIRE_HEIGHT;
}

/**
 * Find the power tile a wire over a tile leads to.
 * @param tile Tile with a wire above it.
 * @param dir Direction to look in.
 * @return The pylon or substation at that end, or INVALID_TILE.
 */
static TileIndex FindSpanEnd(TileIndex tile, Direction dir)
{
	for (uint i = 1; i < POWER_MAX_SPAN; i++) {
		TileIndex t = StepTile(tile, dir, i);
		if (t == INVALID_TILE) return INVALID_TILE;
		if (IsPowerTile(t)) return t;
	}
	return INVALID_TILE;
}

/**
 * Height of the overhead wire passing over a tile.
 * @param tile A tile with a wire above it, or a pylon.
 * @return Height in height levels.
 */
int GetPowerWireHeight(TileIndex tile)
{
	if (IsPowerTile(tile)) return GetPylonBaseZ(tile) + POWER_WIRE_HEIGHT;

	PowerWireKind kind = GetPowerWireKind(tile);
	if (kind == PowerWireKind::None) return GetTileMaxZ(tile) + POWER_WIRE_HEIGHT;

	Direction dir = GetPowerWireDir(kind);
	TileIndex a = FindSpanEnd(tile, dir);
	TileIndex b = FindSpanEnd(tile, ReverseDir(dir));
	if (a == INVALID_TILE || b == INVALID_TILE) return GetTileMaxZ(tile) + POWER_WIRE_HEIGHT;
	return GetSpanWireZ(a, b);
}

/** Names of the base game houses that are low enough to stand under an overhead wire. */
static const StringID _low_house_names[] = {
	STR_TOWN_BUILDING_NAME_TOWN_HOUSES_1,
	STR_TOWN_BUILDING_NAME_STATUE_1,
	STR_TOWN_BUILDING_NAME_FOUNTAIN_1,
	STR_TOWN_BUILDING_NAME_PARK_1,
	STR_TOWN_BUILDING_NAME_SHOPS_AND_OFFICES_1,
	STR_TOWN_BUILDING_NAME_SHOPS_AND_OFFICES_2,
	STR_TOWN_BUILDING_NAME_SHOPS_AND_OFFICES_3,
	STR_TOWN_BUILDING_NAME_WAREHOUSE_1,
	STR_TOWN_BUILDING_NAME_OLD_HOUSES_1,
	STR_TOWN_BUILDING_NAME_COTTAGES_1,
	STR_TOWN_BUILDING_NAME_HOUSES_1,
	STR_TOWN_BUILDING_NAME_HOUSES_2,
	STR_TOWN_BUILDING_NAME_CINEMA_1,
	STR_TOWN_BUILDING_NAME_IGLOO_1,
	STR_TOWN_BUILDING_NAME_TEPEES_1,
	STR_TOWN_BUILDING_NAME_TEAPOT_HOUSE_1,
	STR_TOWN_BUILDING_NAME_PIGGY_BANK_1,
};

/**
 * Is a house low enough to stand under an overhead wire?
 * This must be the same on every computer in a game, so it cannot come from the graphics:
 * base game houses are listed by name, houses from NewGRFs count as tall.
 * @param house The house type.
 * @return True if it is low.
 */
bool IsHouseLowForPowerWire(HouseID house)
{
	const HouseSpec *hs = HouseSpec::Get(house);
	if (hs->grf_prop.HasGrfFile()) return false;
	return std::ranges::find(_low_house_names, hs->building_name) != std::end(_low_house_names);
}

/**
 * Check whether an overhead wire at a height may pass over a tile.
 * Wires pass over all low structures, but not over big buildings, industries, airports or bridges.
 * @param tile The tile under the wire.
 * @param wire_z Height of the wire in height levels.
 * @return Success, or the reason why not.
 */
CommandCost CheckPowerWireClearance(TileIndex tile, int wire_z)
{
	if (!IsValidTile(tile)) return CommandCost(STR_ERROR_TOO_CLOSE_TO_EDGE_OF_MAP);
	if (IsBridgeAbove(tile)) return CommandCost(STR_ERROR_POWER_LINE_BRIDGE_IN_THE_WAY);
	if (HasPowerWire(tile)) return CommandCost(STR_ERROR_POWER_LINES_CANNOT_CROSS);

	int needed = 1;
	switch (GetTileType(tile)) {
		case TileType::Clear:
		case TileType::Trees:
			break;

		case TileType::Railway:
			if (IsRailDepot(tile)) needed = 2;
			break;

		case TileType::Road:
			if (IsRoadDepot(tile)) needed = 2;
			break;

		case TileType::Water:
			if (IsLock(tile) || IsShipDepot(tile)) needed = 2;
			break;

		case TileType::Station:
			if (IsAirport(tile) || IsOilRig(tile)) return CommandCost(STR_ERROR_POWER_LINE_OVER_AIRPORT);
			needed = 2;
			break;

		case TileType::TunnelBridge:
			if (IsBridge(tile)) return CommandCost(STR_ERROR_POWER_LINE_BRIDGE_IN_THE_WAY);
			break;

		case TileType::Object: {
			const ObjectSpec *spec = ObjectSpec::GetByTile(tile);
			if (!spec->flags.Test(ObjectFlag::AllowUnderBridge)) return CommandCost(STR_ERROR_POWER_LINE_TALL_STRUCTURE);
			needed = std::max<int>(1, spec->height);
			break;
		}

		case TileType::House:
			if (!IsHouseLowForPowerWire(GetHouseType(tile))) return CommandCost(STR_ERROR_POWER_LINE_TALL_STRUCTURE);
			needed = 2;
			break;

		case TileType::Industry:
			return CommandCost(STR_ERROR_POWER_LINE_OVER_INDUSTRY);

		case TileType::Power:
			return CommandCost(STR_ERROR_POWER_LINES_JOIN_ONLY_AT_PYLONS);

		default:
			return CommandCost(STR_ERROR_SITE_UNSUITABLE);
	}

	if (GetTileMaxZ(tile) + needed > wire_z) return CommandCost(STR_ERROR_POWER_LINE_TOO_LOW);
	return CommandCost();
}

/**
 * Check whether a structure of some height may be built on a tile, given the wire that may pass over it.
 * Used by other build commands.
 * @param tile The tile.
 * @param height Height of the structure in height levels, counted from the highest corner of the tile; negative for a structure that may never be under a wire.
 * @return Success, or the reason why not.
 */
CommandCost CheckPowerWireAllowsStructure(TileIndex tile, int height)
{
	if (!HasPowerWire(tile)) return CommandCost();
	if (height < 0) return CommandCost(STR_ERROR_MUST_REMOVE_POWER_LINE_FIRST);
	if (GetTileMaxZ(tile) + height > GetPowerWireHeight(tile)) return CommandCost(STR_ERROR_POWER_LINE_TOO_LOW);
	return CommandCost();
}

/**
 * Check whether a diagonal step of a wire would cross another diagonal wire over a tile corner.
 * @param from Tile the step starts at.
 * @param dir Direction of the step.
 * @return True if another wire crosses there.
 */
static bool DiagonalStepCrossesWire(TileIndex from, Direction dir)
{
	if (IsDiagonalDirection(dir)) return false; // Straight steps cross no corners.

	TileIndexDiffC diff = TileIndexDiffCByDir(dir);
	/* The other diagonal through the same corner joins these two tiles. */
	TileIndex side = TileAddXY(from, diff.x, 0);
	if (side == INVALID_TILE) return false;
	Direction cross = GetDirectionOfStep(-diff.x, diff.y);

	if (GetPowerWireKind(side) == GetPowerWireKindForDir(cross)) return true;
	if (IsPowerTile(side) && GetPowerSpan(side, cross) != 0) return true;
	return false;
}

/**
 * Check whether a new pylon or substation can be built on a tile, and what it costs to make room.
 * @param tile The tile.
 * @param flags Command flags; the tile is only cleared when executing.
 * @return Cost of clearing the tile and of a foundation, or an error.
 */
static CommandCost CheckPowerTileSite(TileIndex tile, DoCommandFlags flags)
{
	if (!IsValidTile(tile)) return CommandCost(STR_ERROR_TOO_CLOSE_TO_EDGE_OF_MAP);
	if (IsBridgeAbove(tile)) return CommandCost(STR_ERROR_MUST_DEMOLISH_BRIDGE_FIRST);
	if (HasPowerWire(tile)) return CommandCost(STR_ERROR_POWER_PYLON_UNDER_WIRE);
	if (!IsTileType(tile, TileType::Clear) && !IsTileType(tile, TileType::Trees)) return CommandCost(STR_ERROR_POWER_PYLON_NEEDS_OPEN_LAND);

	CommandCost cost(ExpensesType::Construction);

	Slope tileh = GetTileSlope(tile);
	if (IsSteepSlope(tileh)) return CommandCost(STR_ERROR_LAND_SLOPED_IN_WRONG_DIRECTION);
	if (tileh != SLOPE_FLAT) {
		if (!_settings_game.construction.build_on_slopes) return CommandCost(STR_ERROR_FLAT_LAND_REQUIRED);
		cost.AddCost(_price[Price::BuildFoundation]);
	}

	CommandCost ret = Command<Commands::LandscapeClear>::Do(flags, tile);
	if (ret.Failed()) return ret;
	cost.AddCost(ret.GetCost());
	return cost;
}

/**
 * Mark a tile dirty including the wires high above it.
 * @param tile The tile.
 */
static void MarkPowerTileDirty(TileIndex tile)
{
	_power_network_version++;
	MarkTileDirtyByTile(tile, (POWER_WIRE_HEIGHT + 1) * TILE_HEIGHT);
	if (!IsPowerTile(tile)) return;
	/* The 3D view draws each span from one of its ends, and the wires follow how the pylons at both ends turn. */
	for (Direction dir : EnumRange(Direction::End)) {
		uint span = GetPowerSpan(tile, dir);
		if (span == 0) continue;
		TileIndex other = StepTile(tile, dir, span);
		if (other != INVALID_TILE) MarkTileDirtyByTile(other, (POWER_WIRE_HEIGHT + 1) * TILE_HEIGHT);
	}
}

/**
 * Remove the span of wires leaving a power tile in a direction, from both of its ends and the tiles in between.
 * @param tile The power tile.
 * @param dir The direction.
 */
static void RemovePowerSpan(TileIndex tile, Direction dir)
{
	uint length = GetPowerSpan(tile, dir);
	if (length == 0) return;

	for (uint i = 1; i < length; i++) {
		TileIndex t = StepTile(tile, dir, i);
		SetPowerWire(t, PowerWireKind::None, OWNER_NONE);
		MarkPowerTileDirty(t);
	}
	SetPowerSpan(tile, dir, 0);
	MarkPowerTileDirty(tile);

	TileIndex other = StepTile(tile, dir, length);
	if (other != INVALID_TILE && IsPowerTile(other)) {
		SetPowerSpan(other, ReverseDir(dir), 0);
		MarkPowerTileDirty(other);
	}
}

/**
 * Remove a pylon or substation with all its wires, leaving bare land.
 * @param tile The power tile.
 */
static void RemovePowerTile(TileIndex tile)
{
	for (Direction dir : EnumRange(Direction::End)) RemovePowerSpan(tile, dir);
	DoClearSquare(tile);
	MarkPowerTileDirty(tile);
}

/**
 * Price of the wire of a span.
 * @param dir Direction of the span.
 * @param length Length in tiles.
 * @param build True for building, false for removing.
 * @return The price.
 */
static Money GetSpanPrice(Direction dir, uint length, bool build)
{
	return _price[build ? Price::BuildPowerWire : Price::ClearPowerWire] * (length * GetStepLength(dir)) / 10;
}

/**
 * Build a single pylon, without wires: a normal pylon, or a transition pylon where an overhead line goes underground.
 * A normal pylon of the company can be turned into a transition pylon, keeping its wires.
 * @param flags Type of operation.
 * @param tile Where to build it.
 * @param kind PowerTileKind::Pylon or PowerTileKind::TransitionPylon.
 * @return The cost or an error.
 */
CommandCost CmdBuildPowerPylon(DoCommandFlags flags, TileIndex tile, PowerTileKind kind)
{
	if (kind != PowerTileKind::Pylon && kind != PowerTileKind::TransitionPylon) return CMD_ERROR;
	/* A transition pylon costs three pylons. */
	Money price = _price[Price::BuildPowerPylon] * (kind == PowerTileKind::TransitionPylon ? 3 : 1);

	if (IsValidTile(tile) && IsPowerTile(tile)) {
		if (kind != PowerTileKind::TransitionPylon || GetPowerTileKind(tile) != PowerTileKind::Pylon) return CommandCost(STR_ERROR_ALREADY_BUILT);
		CommandCost owner = CheckTileOwnership(tile);
		if (owner.Failed()) return owner;
		if (flags.Test(DoCommandFlag::Execute)) {
			SB(Tile(tile).m5(), 0, 2, to_underlying(kind));
			MarkPowerTileDirty(tile);
		}
		return CommandCost(ExpensesType::Construction, price - _price[Price::BuildPowerPylon]);
	}

	CommandCost cost = CheckPowerTileSite(tile, flags);
	if (cost.Failed()) return cost;
	cost.AddCost(price);

	if (flags.Test(DoCommandFlag::Execute)) {
		MakePowerTile(tile, _current_company, kind);
		MarkPowerTileDirty(tile);
	}
	return cost;
}

/**
 * Build a substation.
 * @param flags Type of operation.
 * @param tile Where to build it.
 * @param kind Which kind: overhead or underground.
 * @return The cost or an error.
 */
CommandCost CmdBuildPowerSubstation(DoCommandFlags flags, TileIndex tile, PowerTileKind kind)
{
	if (kind != PowerTileKind::OverheadSubstation && kind != PowerTileKind::UndergroundSubstation) return CMD_ERROR;
	if (IsValidTile(tile) && IsPowerTile(tile)) return CommandCost(STR_ERROR_ALREADY_BUILT);

	CommandCost cost = CheckPowerTileSite(tile, flags);
	if (cost.Failed()) return cost;
	/* An underground substation costs twice as much. */
	cost.AddCost(_price[Price::BuildPowerSubstation] * (kind == PowerTileKind::UndergroundSubstation ? 2 : 1));

	if (flags.Test(DoCommandFlag::Execute)) {
		MakePowerTile(tile, _current_company, kind);
		MarkPowerTileDirty(tile);
	}
	return cost;
}

/** One step of a planned line: where the next pylon goes and how far it is. */
struct PlannedSpan {
	uint from; ///< Index along the line of the pylon the span starts at.
	uint to; ///< Index along the line of the pylon the span ends at.
};

/**
 * Work out the direction and length of a line between two tiles.
 * @param start_tile Start of the line.
 * @param end_tile End of the line.
 * @param[out] dir Direction from start to end.
 * @param[out] length Number of steps from start to end.
 * @return Success, or an error if the tiles are not on one of the 8 directions.
 */
static CommandCost GetLineDirection(TileIndex start_tile, TileIndex end_tile, Direction &dir, uint &length)
{
	int dx = static_cast<int>(TileX(end_tile)) - static_cast<int>(TileX(start_tile));
	int dy = static_cast<int>(TileY(end_tile)) - static_cast<int>(TileY(start_tile));
	if (dx != 0 && dy != 0 && std::abs(dx) != std::abs(dy)) return CommandCost(STR_ERROR_POWER_LINE_NOT_STRAIGHT);

	dir = GetDirectionOfStep(Clamp(dx, -1, 1), Clamp(dy, -1, 1));
	length = std::max(std::abs(dx), std::abs(dy));
	return CommandCost();
}

/**
 * Build an overhead power line between two tiles, placing pylons where they are needed.
 * The line runs along an axis or a map diagonal. Pylons are placed as far apart as allowed;
 * existing pylons on the way are used, because wires join only at pylons.
 * @param flags Type of operation.
 * @param end_tile End of the line.
 * @param start_tile Start of the line.
 * @return The cost or an error.
 */
CommandCost CmdBuildPowerLine(DoCommandFlags flags, TileIndex end_tile, TileIndex start_tile)
{
	if (!IsValidTile(start_tile) || !IsValidTile(end_tile)) return CMD_ERROR;
	if (start_tile == end_tile) return CmdBuildPowerPylon(flags, start_tile, PowerTileKind::Pylon);

	Direction dir;
	uint length;
	CommandCost ret = GetLineDirection(start_tile, end_tile, dir, length);
	if (ret.Failed()) return ret;

	DoCommandFlags test_flags = flags;
	test_flags.Reset(DoCommandFlag::Execute);

	auto tile_at = [&](uint i) { return StepTile(start_tile, dir, i); };

	/* Whether a tile can carry the end of a span: an own pylon or substation, or open land for a new pylon. */
	std::vector<CommandCost> site_costs(length + 1, CommandCost(ExpensesType::Construction));
	std::vector<bool> is_new(length + 1, false);
	auto usable = [&](uint i) -> CommandCost {
		TileIndex t = tile_at(i);
		if (IsPowerTile(t)) {
			if (!CanAttachPowerWires(t)) return CommandCost(STR_ERROR_POWER_LINES_JOIN_ONLY_AT_PYLONS);
			CommandCost owner = CheckTileOwnership(t);
			if (owner.Failed()) return owner;
			return CommandCost();
		}
		CommandCost site = CheckPowerTileSite(t, test_flags);
		if (site.Succeeded()) {
			site_costs[i] = site;
			is_new[i] = true;
		}
		return site;
	};

	CommandCost start_ok = usable(0);
	if (start_ok.Failed()) return start_ok;

	/* Plan the pylons, greedily as far apart as possible. */
	std::vector<PlannedSpan> spans;
	uint cur = 0;
	while (cur < length) {
		/* A wire cannot pass over a power tile, so the next one on the way is as far as a span can go. */
		uint max_step = std::min(POWER_MAX_SPAN, length - cur);
		for (uint i = 1; i < max_step; i++) {
			if (IsPowerTile(tile_at(cur + i))) {
				max_step = i;
				break;
			}
		}

		CommandCost first_error;
		bool found = false;
		for (uint step = max_step; step > 0; step--) {
			uint next = cur + step;
			CommandCost ok = usable(next);
			if (ok.Succeeded()) {
				TileIndex a = tile_at(cur);
				TileIndex b = tile_at(next);
				/* Already joined by a span? Nothing to build. */
				if (IsPowerTile(a) && IsPowerTile(b) && GetPowerSpan(a, dir) == step) {
					found = true;
				} else {
					if (IsPowerTile(a) && GetPowerSpan(a, dir) != 0) {
						ok = CommandCost(STR_ERROR_POWER_LINES_CANNOT_CROSS);
					} else if (std::abs(GetPylonBaseZ(a) - GetPylonBaseZ(b)) > 1) {
						ok = CommandCost(STR_ERROR_POWER_PYLONS_HEIGHT_DIFFERENCE);
					} else {
						int wire_z = GetSpanWireZ(a, b);
						for (uint i = 1; i < step && ok.Succeeded(); i++) {
							ok = CheckPowerWireClearance(tile_at(cur + i), wire_z);
						}
						for (uint i = 0; i < step && ok.Succeeded(); i++) {
							if (DiagonalStepCrossesWire(tile_at(cur + i), dir)) ok = CommandCost(STR_ERROR_POWER_LINES_CANNOT_CROSS);
						}
					}
					if (ok.Succeeded()) {
						spans.push_back({cur, next});
						found = true;
					}
				}
			}
			if (found) {
				cur = next;
				break;
			}
			if (first_error.Succeeded()) first_error = ok;
			is_new[next] = false;
		}
		if (!found) {
			if (first_error.Succeeded()) first_error = CommandCost(STR_ERROR_SITE_UNSUITABLE);
			return first_error;
		}
	}

	if (spans.empty()) return CommandCost(STR_ERROR_ALREADY_BUILT);

	/* Only pylons that end up in the plan are built. */
	std::vector<bool> needed(length + 1, false);
	for (const PlannedSpan &s : spans) {
		needed[s.from] = true;
		needed[s.to] = true;
	}

	CommandCost cost(ExpensesType::Construction);
	for (uint i = 0; i <= length; i++) {
		if (!needed[i] || !is_new[i]) continue;
		cost.AddCost(site_costs[i].GetCost());
		cost.AddCost(_price[Price::BuildPowerPylon]);
	}
	for (const PlannedSpan &s : spans) cost.AddCost(GetSpanPrice(dir, s.to - s.from, true));

	if (flags.Test(DoCommandFlag::Execute)) {
		for (uint i = 0; i <= length; i++) {
			if (!needed[i] || !is_new[i]) continue;
			TileIndex t = tile_at(i);
			Command<Commands::LandscapeClear>::Do(flags, t);
			MakePowerTile(t, _current_company, PowerTileKind::Pylon);
			MarkPowerTileDirty(t);
		}
		PowerWireKind kind = GetPowerWireKindForDir(dir);
		for (const PlannedSpan &s : spans) {
			uint step = s.to - s.from;
			TileIndex a = tile_at(s.from);
			TileIndex b = tile_at(s.to);
			SetPowerSpan(a, dir, step);
			SetPowerSpan(b, ReverseDir(dir), step);
			MarkPowerTileDirty(a);
			MarkPowerTileDirty(b);
			for (uint i = 1; i < step; i++) {
				TileIndex t = tile_at(s.from + i);
				SetPowerWire(t, kind, _current_company);
				MarkPowerTileDirty(t);
			}
		}
	}
	return cost;
}

/**
 * Remove the overhead wires between two tiles, and the pylons on the way that are left without wires.
 * @param flags Type of operation.
 * @param end_tile End of the stretch.
 * @param start_tile Start of the stretch.
 * @return The cost or an error.
 */
CommandCost CmdRemovePowerLine(DoCommandFlags flags, TileIndex end_tile, TileIndex start_tile)
{
	if (!IsValidTile(start_tile) || !IsValidTile(end_tile)) return CMD_ERROR;

	Direction dir = Direction::NE;
	uint length = 0;
	if (start_tile != end_tile) {
		CommandCost ret = GetLineDirection(start_tile, end_tile, dir, length);
		if (ret.Failed()) return ret;
	}

	auto tile_at = [&](uint i) { return StepTile(start_tile, dir, i); };

	CommandCost cost(ExpensesType::Construction);
	CommandCost last_error = CommandCost(STR_ERROR_THERE_IS_NO_POWER_LINE);
	bool removed = false;

	/* Spans with both ends on the stretch. */
	std::vector<std::pair<TileIndex, uint>> spans;
	for (uint i = 0; i <= length; i++) {
		TileIndex t = tile_at(i);
		if (!IsPowerTile(t)) continue;
		uint span = GetPowerSpan(t, dir);
		if (span == 0 || i + span > length) continue;
		CommandCost owner = CheckTileOwnership(t);
		if (owner.Failed()) {
			last_error = owner;
			continue;
		}
		spans.emplace_back(t, span);
		cost.AddCost(GetSpanPrice(dir, span, false));
		removed = true;
	}

	/* Pylons on the stretch that have no wires left afterwards. */
	std::vector<TileIndex> pylons;
	for (uint i = 0; i <= length; i++) {
		TileIndex t = tile_at(i);
		if (!IsPowerTile(t) || GetPowerTileKind(t) != PowerTileKind::Pylon) continue;
		if (CheckTileOwnership(t).Failed()) continue;
		bool keeps_wires = false;
		for (Direction d : EnumRange(Direction::End)) {
			uint span = GetPowerSpan(t, d);
			if (span == 0) continue;
			bool goes = (d == dir && i + span <= length) || (d == ReverseDir(dir) && i >= span);
			if (!goes) keeps_wires = true;
		}
		if (keeps_wires) continue;
		pylons.push_back(t);
		cost.AddCost(_price[Price::ClearPowerPylon]);
		removed = true;
	}

	if (!removed) return last_error;

	if (flags.Test(DoCommandFlag::Execute)) {
		for (const auto &[t, span] : spans) RemovePowerSpan(t, dir);
		for (TileIndex t : pylons) RemovePowerTile(t);
	}
	return cost;
}

/**
 * Whether a diagonal step of a cable would cross another cable between the same four tiles.
 * @param from Tile the step starts at.
 * @param dir Direction of the step.
 * @return True if the step crosses a cable.
 */
static bool DiagonalStepCrossesCable(TileIndex from, Direction dir)
{
	if (IsDiagonalDirection(dir)) return false; // Straight steps cross no corners.

	TileIndexDiffC diff = TileIndexDiffCByDir(dir);
	TileIndex side = TileAddXY(from, diff.x, 0);
	if (side == INVALID_TILE) return false;
	Direction cross = GetDirectionOfStep(-diff.x, diff.y);
	return HasBit(GetPowerCableDirs(side), to_underlying(cross));
}

/**
 * Price of underground cable for a step.
 * @param from Tile the step starts at.
 * @param to Tile the step ends at.
 * @param dir Direction of the step.
 * @param build True for building, false for removing.
 * @return The price; under water it is doubled when building.
 */
static Money GetCableStepPrice(TileIndex from, TileIndex to, Direction dir, bool build)
{
	Money price = _price[build ? Price::BuildPowerCable : Price::ClearPowerCable] * GetStepLength(dir) / 10;
	if (build && (IsTileType(from, TileType::Water) || IsTileType(to, TileType::Water))) price *= 2;
	return price;
}

/**
 * Number of directions a cable may leave a tile in: any at an own underground substation or transition pylon,
 * else two, as a cable only continues or bends.
 * @param tile The tile.
 * @return Highest number of directions.
 */
static uint GetMaxCableDirs(TileIndex tile)
{
	return (CanAttachPowerCables(tile) && GetTileOwner(tile) == _current_company) ? 8 : 2;
}

/**
 * Build an underground cable between two tiles, along an axis or a map diagonal.
 * A cable runs under anything. It joins other cables only by continuing or bending them, and connects
 * to the network only at underground substations and transition pylons.
 * @param flags Type of operation.
 * @param end_tile End of the cable.
 * @param start_tile Start of the cable.
 * @return The cost or an error.
 */
CommandCost CmdBuildPowerCable(DoCommandFlags flags, TileIndex end_tile, TileIndex start_tile)
{
	if (!IsValidTile(start_tile) || !IsValidTile(end_tile)) return CMD_ERROR;
	if (start_tile == end_tile) return CommandCost(STR_ERROR_POWER_CABLE_TOO_SHORT);

	Direction dir;
	uint length;
	CommandCost ret = GetLineDirection(start_tile, end_tile, dir, length);
	if (ret.Failed()) return ret;

	auto tile_at = [&](uint i) { return StepTile(start_tile, dir, i); };

	/* Directions every tile of the cable gets. */
	std::vector<uint8_t> dirs(length + 1);
	CommandCost cost(ExpensesType::Construction);
	bool any = false;
	for (uint i = 0; i <= length; i++) {
		TileIndex t = tile_at(i);
		if (!IsValidTile(t)) return CommandCost(STR_ERROR_TOO_CLOSE_TO_EDGE_OF_MAP);
		if (HasPowerCable(t) && GetPowerCableOwner(t) != _current_company) return CommandCost(STR_ERROR_POWER_CABLE_OWNED_BY_OTHER);
		if (IsPowerTile(t) && CanAttachPowerCables(t)) {
			CommandCost owner = CheckTileOwnership(t);
			if (owner.Failed()) return owner;
		}
		dirs[i] = GetPowerCableDirs(t);
		if (i > 0) SetBit(dirs[i], to_underlying(ReverseDir(dir)));
		if (i < length) SetBit(dirs[i], to_underlying(dir));
		if (static_cast<uint>(CountBits(dirs[i])) > GetMaxCableDirs(t)) return CommandCost(STR_ERROR_POWER_CABLE_BRANCH_NEEDS_SUBSTATION);
	}
	for (uint i = 0; i < length; i++) {
		TileIndex a = tile_at(i);
		if (HasBit(GetPowerCableDirs(a), to_underlying(dir))) continue;
		if (DiagonalStepCrossesCable(a, dir)) return CommandCost(STR_ERROR_POWER_CABLES_CANNOT_CROSS);
		cost.AddCost(GetCableStepPrice(a, tile_at(i + 1), dir, true));
		any = true;
	}
	if (!any) return CommandCost(STR_ERROR_ALREADY_BUILT);

	if (flags.Test(DoCommandFlag::Execute)) {
		for (uint i = 0; i <= length; i++) {
			TileIndex t = tile_at(i);
			SetPowerCable(t, dirs[i], _current_company);
			MarkPowerTileDirty(t);
		}
	}
	return cost;
}

/**
 * Remove the underground cable between two tiles.
 * @param flags Type of operation.
 * @param end_tile End of the stretch.
 * @param start_tile Start of the stretch.
 * @return The cost or an error.
 */
CommandCost CmdRemovePowerCable(DoCommandFlags flags, TileIndex end_tile, TileIndex start_tile)
{
	if (!IsValidTile(start_tile) || !IsValidTile(end_tile)) return CMD_ERROR;
	if (start_tile == end_tile) return CommandCost(STR_ERROR_THERE_IS_NO_POWER_CABLE);

	Direction dir;
	uint length;
	CommandCost ret = GetLineDirection(start_tile, end_tile, dir, length);
	if (ret.Failed()) return ret;

	auto tile_at = [&](uint i) { return StepTile(start_tile, dir, i); };

	CommandCost cost(ExpensesType::Construction);
	std::vector<uint> steps;
	for (uint i = 0; i < length; i++) {
		TileIndex a = tile_at(i);
		if (!HasBit(GetPowerCableDirs(a), to_underlying(dir))) continue;
		if (_current_company != OWNER_DEITY && GetPowerCableOwner(a) != _current_company) return CommandCost(STR_ERROR_POWER_CABLE_OWNED_BY_OTHER);
		steps.push_back(i);
		cost.AddCost(GetCableStepPrice(a, tile_at(i + 1), dir, false));
	}
	if (steps.empty()) return CommandCost(STR_ERROR_THERE_IS_NO_POWER_CABLE);

	if (flags.Test(DoCommandFlag::Execute)) {
		for (uint i : steps) {
			TileIndex a = tile_at(i);
			TileIndex b = tile_at(i + 1);
			SetPowerCable(a, GetPowerCableDirs(a) & ~(1 << to_underlying(dir)), GetPowerCableOwner(a));
			SetPowerCable(b, GetPowerCableDirs(b) & ~(1 << to_underlying(ReverseDir(dir))), GetPowerCableOwner(b));
			MarkPowerTileDirty(a);
			MarkPowerTileDirty(b);
		}
	}
	return cost;
}

/**
 * Turn the power network view on or off.
 * @param on True to show underground cables.
 */
void SetPowerNetworkView(bool on)
{
	if (_power_network_view == on) return;
	_power_network_view = on;
	MarkWholeScreenDirty();
#ifdef WITH_VIEWPORT3D
	MarkAreaDirtyForView3D(0, 0, Map::MaxX(), Map::MaxY());
#endif
}

/**
 * Hand all underground cables of a company to another, or remove them.
 * Called when a company goes bankrupt or is bought; overhead wires follow their pylons instead.
 * @param old_owner The company that goes.
 * @param new_owner The company taking over, or INVALID_OWNER to remove the cables.
 */
void ChangePowerCableOwner(Owner old_owner, Owner new_owner)
{
	if (_power_layer == nullptr) return;
	for (TileIndex t : Map::Iterate()) {
		if (!HasPowerCable(t) || GetPowerCableOwner(t) != old_owner) continue;
		SetPowerCable(t, new_owner == INVALID_OWNER ? 0 : GetPowerCableDirs(t), new_owner);
	}
	_power_network_version++;
}

/**
 * Find out which pylons and substations are joined to a power tile by wires.
 * @param tile A power tile, or a tile with a wire above.
 * @return Counts for the network.
 */
PowerNetworkInfo GetPowerNetworkInfo(TileIndex tile)
{
	PowerNetworkInfo info;
	if (!IsPowerTile(tile)) {
		if (!HasPowerWire(tile)) return info;
		tile = FindSpanEnd(tile, GetPowerWireDir(GetPowerWireKind(tile)));
		if (tile == INVALID_TILE) return info;
	}

	std::vector<TileIndex> todo{tile};
	std::set<TileIndex> seen{tile};
	while (!todo.empty()) {
		TileIndex t = todo.back();
		todo.pop_back();
		if (IsPowerSubstation(t)) {
			info.substations++;
		} else {
			info.pylons++;
		}
		for (Direction dir : EnumRange(Direction::End)) {
			uint span = GetPowerSpan(t, dir);
			if (span == 0) continue;
			info.spans++;
			TileIndex next = StepTile(t, dir, span);
			if (next == INVALID_TILE || !IsPowerTile(next)) continue;
			if (seen.insert(next).second) todo.push_back(next);
		}
	}
	info.spans /= 2; // Every span was counted from both ends.
	return info;
}

/**
 * Draw the wires of some kind over a tile.
 * @param ti The tile.
 * @param kind The kind of wire.
 * @param z Height of the wire in pixels.
 */
static void DrawWirePieces(const TileInfo *ti, PowerWireKind kind, int z)
{
	bool transparent = IsTransparencySet(TransparencyOption::Catenary);
	if (_power_sprites_loaded) {
		/* One sprite with all conductors over the whole tile, its origin at the north corner of the tile at the height of the wire. */
		static const SpriteBounds BOUNDS[4] = {
			{{0, 5, 0}, {16, 6, 8}, {0, -5, 0}},
			{{5, 0, 0}, {6, 16, 8}, {-5, 0, 0}},
			{{0, 0, 0}, {16, 16, 8}, {0, 0, 0}},
			{{0, 0, 0}, {16, 16, 8}, {0, 0, 0}},
		};
		uint index = to_underlying(kind) - to_underlying(PowerWireKind::AxisX);
		if (index < 4) AddSortableSpriteToDraw(SPR_POWER_WIRES + index, PAL_NONE, ti->x, ti->y, z, BOUNDS[index], transparent);
		return;
	}

	/* Stand-ins from the catenary of electrified rail; the diagonals are drawn twice, as two conductors. */
	static const SpriteBounds X_BOUNDS{{0, 7, 0}, {15, 1, 1}, {}};
	static const SpriteBounds Y_BOUNDS{{7, 0, 0}, {1, 15, 1}, {}};
	static const SpriteBounds NS_BOUNDS[2] = {{{8, 0, 0}, {8, 8, 1}, {}}, {{0, 8, 0}, {8, 8, 1}, {}}};
	static const SpriteBounds EW_BOUNDS[2] = {{{7, 0, 0}, {1, 1, 1}, {}}, {{15, 8, 0}, {3, 3, 1}, {}}};
	/* Offsets of the "both ends" wire sprites from the base wire sprite: X, Y, EW, NS. */
	static const uint8_t WIRE_X = 0, WIRE_Y = 1, WIRE_EW = 2, WIRE_NS = 3;

	switch (kind) {
		case PowerWireKind::AxisX:
			AddSortableSpriteToDraw(SPR_POWER_WIRE_BASE + WIRE_X, PAL_NONE, ti->x, ti->y, z, X_BOUNDS, transparent);
			break;
		case PowerWireKind::AxisY:
			AddSortableSpriteToDraw(SPR_POWER_WIRE_BASE + WIRE_Y, PAL_NONE, ti->x, ti->y, z, Y_BOUNDS, transparent);
			break;
		case PowerWireKind::DiagNS:
			for (const SpriteBounds &b : NS_BOUNDS) AddSortableSpriteToDraw(SPR_POWER_WIRE_BASE + WIRE_NS, PAL_NONE, ti->x, ti->y, z, b, transparent);
			break;
		case PowerWireKind::DiagEW:
			for (const SpriteBounds &b : EW_BOUNDS) AddSortableSpriteToDraw(SPR_POWER_WIRE_BASE + WIRE_EW, PAL_NONE, ti->x, ti->y, z, b, transparent);
			break;
		default:
			break;
	}
}

/**
 * Which way a pylon or substation turns in the 2D view, following the line through it; on a bend it turns halfway.
 * @param tile The power tile.
 * @return 0 along x, 1 along y, 2 along the N-S diagonal, 3 along the E-W diagonal.
 */
static uint GetPowerSpriteDirection(TileIndex tile)
{
	int x = 0, y = 0;
	uint count = 0;
	for (Direction dir : EnumRange(Direction::End)) {
		if (GetPowerSpan(tile, dir) == 0) continue;
		TileIndexDiffC d = TileIndexDiffCByDir(dir);
		if (count == 0) {
			x = d.x;
			y = d.y;
		} else if (count == 1) {
			x -= d.x;
			y -= d.y;
		}
		count++;
	}
	if (count > 2) {
		/* A junction faces along its first line. */
		for (Direction dir : EnumRange(Direction::End)) {
			if (GetPowerSpan(tile, dir) == 0) continue;
			TileIndexDiffC d = TileIndexDiffCByDir(dir);
			x = d.x;
			y = d.y;
			break;
		}
	}
	if (std::abs(x) >= 2 * std::abs(y)) return 0;
	if (std::abs(y) >= 2 * std::abs(x)) return 1;
	return (x > 0) == (y > 0) ? 2 : 3;
}

/**
 * Draw the overhead wire above a tile that is not a power tile.
 * Called for every tile after its own contents are drawn.
 * @param ti The tile.
 */
/**
 * Draw what shows of the underground cable under a tile: marker posts on open land, and with the power network view on,
 * the cable itself.
 * @param ti The tile.
 */
static void DrawPowerCable(const TileInfo *ti)
{
	if (!_power_cable_sprites || !_power_sprites_loaded) return;
	uint8_t dirs = GetPowerCableDirs(ti->tile);
	if (dirs == 0) return;
	int z = GetSlopePixelZ(ti->x + TILE_SIZE / 2, ti->y + TILE_SIZE / 2);
	if (_power_network_view) {
		static const SpriteBounds CABLE_BOUNDS{{0, 0, 0}, {16, 16, 1}, {0, 0, 0}};
		for (Direction dir : EnumRange(Direction::End)) {
			if (HasBit(dirs, to_underlying(dir))) AddSortableSpriteToDraw(SPR_POWER_CABLE_VIEW + to_underlying(dir), PAL_NONE, ti->x, ti->y, z + 1, CABLE_BOUNDS);
		}
	}
	if (IsInvisibilitySet(TransparencyOption::Structures)) return;
	if (IsTileType(ti->tile, TileType::Clear) || IsTileType(ti->tile, TileType::Trees)) {
		static const SpriteBounds MARKER_BOUNDS{{7, 7, 0}, {2, 2, 8}, {-7, -7, 0}};
		AddSortableSpriteToDraw(SPR_POWER_CABLE_MARKER, PAL_NONE, ti->x, ti->y, z, MARKER_BOUNDS, IsTransparencySet(TransparencyOption::Structures));
	}
}

void DrawPowerWireAbove(const TileInfo *ti)
{
	DrawPowerCable(ti);
	if (!_power_wire_sprites) return;
	if (IsInvisibilitySet(TransparencyOption::Catenary)) return;
	PowerWireKind kind = GetPowerWireKind(ti->tile);
	if (kind == PowerWireKind::None) return;
	DrawWirePieces(ti, kind, GetPowerWireHeight(ti->tile) * TILE_HEIGHT);
}

/**
 * Add the owner of an overhead wire above a tile to its description.
 * @param tile The tile.
 * @param td The description.
 */
void AddPowerWireTileDesc(TileIndex tile, TileDesc &td)
{
	if (!HasPowerWire(tile)) return;
	for (uint i = 1; i < td.owner_type.size(); i++) {
		if (td.owner_type[i] != STR_NULL) continue;
		td.owner_type[i] = STR_LAND_AREA_INFORMATION_POWER_LINE_OWNER;
		td.owner[i] = GetPowerWireOwner(tile);
		return;
	}
}

/** @copydoc DrawTileProc */
static void DrawTile_Power(TileInfo *ti)
{
	DrawFoundation(ti, FlatteningFoundation(ti->tileh));
	DrawClearLandTile(ti, 3);

	PowerTileKind kind = GetPowerTileKind(ti->tile);
	if (!IsInvisibilitySet(TransparencyOption::Structures)) {
		bool transparent = IsTransparencySet(TransparencyOption::Structures);
		if (_power_sprites_loaded) {
			/* The sprites have their origin at the north corner of the tile. */
			static const SpriteBounds PYLON_BOUNDS{{6, 6, 0}, {4, 4, 40}, {-6, -6, 0}};
			static const SpriteBounds YARD_BOUNDS{{1, 1, 0}, {14, 14, 30}, {-1, -1, 0}};
			uint dir = GetPowerSpriteDirection(ti->tile);
			switch (kind) {
				case PowerTileKind::Pylon:
					AddSortableSpriteToDraw(SPR_POWER_PYLON + dir, PAL_NONE, ti->x, ti->y, ti->z, PYLON_BOUNDS, transparent);
					break;
				case PowerTileKind::TransitionPylon:
					AddSortableSpriteToDraw(SPR_POWER_TRANSITION_PYLON + dir, PAL_NONE, ti->x, ti->y, ti->z, PYLON_BOUNDS, transparent);
					break;
				case PowerTileKind::OverheadSubstation:
					AddSortableSpriteToDraw(SPR_POWER_SUBSTATION_OVERHEAD + (dir == 1 ? 1 : 0), PAL_NONE, ti->x, ti->y, ti->z, YARD_BOUNDS, transparent);
					break;
				case PowerTileKind::UndergroundSubstation:
					AddSortableSpriteToDraw(SPR_POWER_SUBSTATION_UNDERGROUND, PAL_NONE, ti->x, ti->y, ti->z, YARD_BOUNDS, transparent);
					break;
			}
		} else if (kind == PowerTileKind::OverheadSubstation || kind == PowerTileKind::UndergroundSubstation) {
			static const SpriteBounds SUBSTATION_BOUNDS{{1, 2, 0}, {14, 11, 32}, {}};
			AddSortableSpriteToDraw(SPR_POWER_SUBSTATION, PAL_NONE, ti->x, ti->y, ti->z, SUBSTATION_BOUNDS, transparent);
		} else {
			static const SpriteBounds PYLON_BOUNDS{{7, 7, 0}, {2, 2, 70}, {}};
			AddSortableSpriteToDraw(SPR_TRANSMITTER, PAL_NONE, ti->x, ti->y, ti->z, PYLON_BOUNDS, transparent);
		}
	}

	if (!IsInvisibilitySet(TransparencyOption::Catenary)) {
		/* Draw one piece of wire for every kind of wire leaving the tile; with power.grf, from the pylon to the edge of the tile. */
		uint8_t drawn = 0;
		for (Direction dir : EnumRange(Direction::End)) {
			uint span = GetPowerSpan(ti->tile, dir);
			if (span == 0) continue;
			if (_power_sprites_loaded) {
				static const SpriteBounds HALF_BOUNDS{{0, 0, 0}, {16, 16, 8}, {0, 0, 0}};
				TileIndex other = StepTile(ti->tile, dir, span);
				int z = (other != INVALID_TILE && IsPowerTile(other)) ? GetSpanWireZ(ti->tile, other) : GetPowerWireHeight(ti->tile);
				AddSortableSpriteToDraw(SPR_POWER_HALF_WIRES + to_underlying(dir), PAL_NONE, ti->x, ti->y, z * TILE_HEIGHT, HALF_BOUNDS, IsTransparencySet(TransparencyOption::Catenary));
				continue;
			}
			PowerWireKind wire = GetPowerWireKindForDir(dir);
			if (HasBit(drawn, to_underlying(wire))) continue;
			SetBit(drawn, to_underlying(wire));

			TileIndex other = StepTile(ti->tile, dir, span);
			int z = (other != INVALID_TILE && IsPowerTile(other)) ? GetSpanWireZ(ti->tile, other) : GetPowerWireHeight(ti->tile);
			DrawWirePieces(ti, wire, z * TILE_HEIGHT);
		}
	}
}

/** @copydoc GetSlopePixelZProc */
static int GetSlopePixelZ_Power(TileIndex tile, uint, uint, bool)
{
	return GetTileMaxPixelZ(tile);
}

/** @copydoc GetFoundationProc */
static Foundation GetFoundation_Power(TileIndex, Slope tileh)
{
	return FlatteningFoundation(tileh);
}

/** @copydoc ClearTileProc */
static CommandCost ClearTile_Power(TileIndex tile, DoCommandFlags flags)
{
	/* No automatic removal by building something else over it. */
	if (flags.Test(DoCommandFlag::Auto)) return CommandCost(STR_ERROR_MUST_REMOVE_POWER_LINE_FIRST);

	if (_current_company != OWNER_WATER) {
		CommandCost ret = CheckTileOwnership(tile);
		if (ret.Failed()) return ret;
	}

	PowerTileKind kind = GetPowerTileKind(tile);
	bool substation = kind == PowerTileKind::OverheadSubstation || kind == PowerTileKind::UndergroundSubstation;
	CommandCost cost(ExpensesType::Construction, _price[substation ? Price::ClearPowerSubstation : Price::ClearPowerPylon]);
	for (Direction dir : EnumRange(Direction::End)) {
		uint span = GetPowerSpan(tile, dir);
		if (span != 0) cost.AddCost(GetSpanPrice(dir, span, false));
	}

	if (flags.Test(DoCommandFlag::Execute)) RemovePowerTile(tile);
	return cost;
}

/** @copydoc GetTileDescProc */
static void GetTileDesc_Power(TileIndex tile, TileDesc &td)
{
	PowerNetworkInfo info = GetPowerNetworkInfo(tile);
	switch (GetPowerTileKind(tile)) {
		case PowerTileKind::OverheadSubstation: td.str = STR_LAI_POWER_DESCRIPTION_OVERHEAD_SUBSTATION; break;
		case PowerTileKind::UndergroundSubstation: td.str = STR_LAI_POWER_DESCRIPTION_UNDERGROUND_SUBSTATION; break;
		case PowerTileKind::TransitionPylon: td.str = STR_LAI_POWER_DESCRIPTION_TRANSITION_PYLON; break;
		default: td.str = STR_LAI_POWER_DESCRIPTION_PYLON; break;
	}
	td.dparam = info.substations;
	td.owner[0] = GetTileOwner(tile);
}

/** @copydoc ChangeTileOwnerProc */
static void ChangeTileOwner_Power(TileIndex tile, Owner old_owner, Owner new_owner)
{
	if (!IsTileOwner(tile, old_owner)) return;
	_power_network_version++;

	if (new_owner == INVALID_OWNER) {
		RemovePowerTile(tile);
		return;
	}

	SetTileOwner(tile, new_owner);
	for (Direction dir : EnumRange(Direction::End)) {
		uint span = GetPowerSpan(tile, dir);
		for (uint i = 1; i < span; i++) {
			TileIndex t = StepTile(tile, dir, i);
			SetPowerWire(t, GetPowerWireKind(t), new_owner);
		}
	}
}

/** @copydoc TerraformTileProc */
static CommandCost TerraformTile_Power(TileIndex, DoCommandFlags, int, Slope)
{
	return CommandCost(STR_ERROR_MUST_REMOVE_POWER_LINE_FIRST);
}

/** @copydoc CheckBuildAboveProc */
static CommandCost CheckBuildAbove_Power(TileIndex, DoCommandFlags, Axis, int)
{
	return CommandCost(STR_ERROR_MUST_REMOVE_POWER_LINE_FIRST);
}

/** TileTypeProcs definitions for TileType::Power tiles. */
extern const TileTypeProcs _tile_type_power_procs = {
	.draw_tile_proc = DrawTile_Power,
	.get_slope_pixel_z_proc = GetSlopePixelZ_Power,
	.clear_tile_proc = ClearTile_Power,
	.get_tile_desc_proc = GetTileDesc_Power,
	.tile_loop_proc = [](TileIndex) {},
	.change_tile_owner_proc = ChangeTileOwner_Power,
	.get_foundation_proc = GetFoundation_Power,
	.terraform_tile_proc = TerraformTile_Power,
	.check_build_above_proc = CheckBuildAbove_Power,
};
