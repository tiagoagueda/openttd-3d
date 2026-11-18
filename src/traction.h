/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file traction.h Traction power: trams and trolleybuses that draw power from the energy network.
 *
 * Fork-only. The real code (traction_power.cpp) is built only together with the energy transport
 * (power lines), when CMake finds power_economy.cpp and OPTION_TRACTION_POWER is on; it then defines
 * WITH_TRACTION_POWER. Without it, these functions say the wires are always live and nothing changes.
 */

#ifndef TRACTION_H
#define TRACTION_H

#include "economy_type.h"
#include "road_type.h"
#include "tile_type.h"

struct RoadVehicle;

#ifdef WITH_TRACTION_POWER

/** Whether this build has traction power (the energy transport is built in). */
static constexpr bool TRACTION_POWER = true;

uint GetTractionSpeedFactor(const RoadVehicle *v);
Money GetTractionFixedRunningCost(RoadType rt, Money cost);
int GetTractionSupplyPercent(TileIndex tile, RoadTramType rtt);

/* Used by the balancing of the energy networks (power_economy.cpp). */
std::vector<uint> PrepareTractionDemand(std::span<const TileIndex> substations);
void ApplyTractionSupply(std::span<const uint> delivered);

#else /* WITH_TRACTION_POWER */

/** Whether this build has traction power (the energy transport is built in). */
static constexpr bool TRACTION_POWER = false;

/**
 * How much of its speed an electric vehicle may use, for the power it gets.
 * @return 256: without traction power the wires are always live.
 */
inline uint GetTractionSpeedFactor(const RoadVehicle *) { return 256; }

/**
 * The fixed part of the running cost of a road vehicle.
 * @param cost The running cost of the vehicle.
 * @return The same cost: without traction power nobody buys energy for trams and trolleybuses.
 */
inline Money GetTractionFixedRunningCost(RoadType, Money cost) { return cost; }

/**
 * The share of power the wire over a tile gets.
 * @return -1: no figure without traction power.
 */
inline int GetTractionSupplyPercent(TileIndex, RoadTramType) { return -1; }

#endif /* WITH_TRACTION_POWER */

#endif /* TRACTION_H */
