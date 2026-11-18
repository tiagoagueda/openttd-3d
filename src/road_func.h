/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file road_func.h Functions related to roads. */

#ifndef ROAD_FUNC_H
#define ROAD_FUNC_H

#include "core/bitmath_func.hpp"
#include "road.h"
#include "economy_func.h"
#include "transparency.h"

/**
 * Whether the given roadtype is valid.
 * @param r the roadtype to check for validness
 * @return true if and only if valid
 */
inline bool IsValidRoadBits(RoadBits r)
{
	return r.Reset(ROAD_ALL).None();
}

/**
 * Calculate the complement of a RoadBits value
 *
 * Simply flips all bits in the RoadBits value to get the complement
 * of the RoadBits.
 *
 * @param r The given RoadBits value
 * @return the complement
 */
inline RoadBits ComplementRoadBits(RoadBits r)
{
	assert(IsValidRoadBits(r));
	return r.Flip(ROAD_ALL);
}

/**
 * Calculate the mirrored RoadBits
 *
 * Simply move the bits to their new position.
 *
 * @param r The given RoadBits value
 * @return the mirrored
 */
inline RoadBits MirrorRoadBits(RoadBits r)
{
	assert(IsValidRoadBits(r));
	return static_cast<RoadBits>(GB(r.base(), 0, 2) << 2 | GB(r.base(), 2, 2));
}

/**
 * Check if we've got a straight road
 *
 * @param r The given RoadBits
 * @return true if we've got a straight road
 */
inline bool IsStraightRoad(RoadBits r)
{
	assert(IsValidRoadBits(r));
	return (r == ROAD_X || r == ROAD_Y);
}

/**
 * Create the road-part which belongs to the given DiagDirection
 *
 * This function returns a RoadBits value which belongs to
 * the given DiagDirection.
 *
 * @param d The DiagDirection
 * @return The result RoadBits which the selected road-part set
 */
inline RoadBits DiagDirToRoadBits(DiagDirection d)
{
	assert(IsValidDiagDirection(d));
	return static_cast<RoadBits>(RoadBits{RoadBit::NW}.base() << (3 ^ to_underlying(d)));
}

/**
 * Create the road-part which belongs to the given Axis
 *
 * This function returns a RoadBits value which belongs to
 * the given Axis.
 *
 * @param a The Axis
 * @return The result RoadBits which the selected road-part set
 */
inline RoadBits AxisToRoadBits(Axis a)
{
	assert(IsValidAxis(a));
	return a == Axis::X ? ROAD_X : ROAD_Y;
}


/**
 * Calculates the maintenance cost of a number of road bits.
 * @param roadtype Road type to get the cost for.
 * @param num Number of road bits.
 * @param total_num Total number of road bits of all road/tram-types.
 * @return Total cost.
 */
inline Money RoadMaintenanceCost(RoadType roadtype, uint32_t num, uint32_t total_num)
{
	assert(roadtype < ROADTYPE_END);
	return (_price[Price::InfrastructureRoad] * GetRoadTypeInfo(roadtype)->maintenance_multiplier * num * (1 + IntSqrt(total_num))) >> 12;
}

/**
 * Test if a road type has catenary
 * @param roadtype Road type to test
 * @return \c true iff the road should have catenary.
 */
inline bool HasRoadCatenary(RoadType roadtype)
{
	assert(roadtype < ROADTYPE_END);
	return GetRoadTypeInfo(roadtype)->flags.Test(RoadTypeFlag::Catenary);
}

/** How the overhead wires of a road type look and work. */
enum class CatenaryStyle : uint8_t {
	None,   ///< No overhead wires.
	Single, ///< One contact wire over each track; trams, with the rails as the return path.
	Pair,   ///< A pair of wires over each lane; trolleybuses, which pick up and return power through two poles.
};

/**
 * Get the style of the overhead wires of a road type.
 * Electric tram types have a single contact wire, electric road types (such as electrified road for
 * trolleybuses) a pair of wires. This also holds for road types from NewGRF.
 * @param roadtype Road type to test.
 * @return The style.
 */
inline CatenaryStyle GetRoadCatenaryStyle(RoadType roadtype)
{
	if (!HasRoadCatenary(roadtype)) return CatenaryStyle::None;
	return RoadTypeIsTram(roadtype) ? CatenaryStyle::Single : CatenaryStyle::Pair;
}

/**
 * Whether a road type carries trolleybus wires.
 * @param roadtype Road type to test.
 * @return \c true for electrified road.
 */
inline bool IsTrolleyRoadType(RoadType roadtype)
{
	return GetRoadCatenaryStyle(roadtype) == CatenaryStyle::Pair;
}

/**
 * Whether a road depot of one road type builds and services vehicles of another.
 * A road depot on electrified road is a trolleybus depot: it takes trolleybuses only, and a road depot
 * on other road takes no trolleybuses. Tram depots take every tram that can reach them.
 * @param depot_type The road type of the depot, of the vehicle's half of the road.
 * @param vehicle_type The road type of the vehicle.
 * @return \c true if the depot takes the vehicle.
 */
inline bool RoadDepotTakesRoadType(RoadType depot_type, RoadType vehicle_type)
{
	if (RoadTypeIsTram(vehicle_type)) return true;
	return IsTrolleyRoadType(depot_type) == IsTrolleyRoadType(vehicle_type);
}

bool RoadDepotTakesVehicle(TileIndex depot, RoadType vehicle_type);

/**
 * Test if we should draw road catenary
 * @param roadtype Road type to test
 * @return \c true iff the road should have catenary and catenary is visible.
 */
inline bool HasRoadCatenaryDrawn(RoadType roadtype)
{
	return HasRoadCatenary(roadtype) && !IsInvisibilitySet(TransparencyOption::Catenary);
}

bool HasRoadTypeAvail(CompanyID company, RoadType roadtype);
bool ValParamRoadType(RoadType roadtype);
RoadTypes GetCompanyRoadTypes(CompanyID company, bool introduces = true);
RoadTypes GetRoadTypes(bool introduces);
RoadTypes AddDateIntroducedRoadTypes(RoadTypes current, TimerGameCalendar::Date date);

void UpdateLevelCrossing(TileIndex tile, bool sound = true, bool force_bar = false);
void MarkDirtyAdjacentLevelCrossingTiles(TileIndex tile, Axis road_axis);
void UpdateAdjacentLevelCrossingTilesOnLevelCrossingRemoval(TileIndex tile, Axis road_axis);
void UpdateCompanyRoadInfrastructure(RoadType rt, Owner o, int count);
bool UpdateNearestTownForRoadTiles(bool invalidate);

struct TileInfo;
enum class Roadside : uint8_t;
void DrawRoadOverlays(const TileInfo *ti, PaletteID pal, const RoadTypeInfo *road_rti, const RoadTypeInfo *tram_rit, uint road_offset, uint tram_offset, bool draw_underlay = true);
void DrawRoadGroundSprites(const TileInfo *ti, RoadBits road, RoadBits tram, const RoadTypeInfo *road_rti, const RoadTypeInfo *tram_rti, Roadside roadside, bool snow_or_desert);
void DrawRoadDepotSprite(int x, int y, DiagDirection dir, RoadType rt);

#endif /* ROAD_FUNC_H */
