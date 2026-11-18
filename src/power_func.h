/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file power_func.h Functions related to power lines, used outside the power line code. */

#ifndef POWER_FUNC_H
#define POWER_FUNC_H

#include "command_type.h"
#include "house_type.h"
#include "tile_cmd.h"

struct Window;

/* Checks used by other build commands, so that nothing tall is built under a wire. */
int GetPowerWireHeight(TileIndex tile);
CommandCost CheckPowerWireClearance(TileIndex tile, int wire_z);
CommandCost CheckPowerWireAllowsStructure(TileIndex tile, int height);
bool IsHouseLowForPowerWire(HouseID house);

void DrawPowerWireAbove(const TileInfo *ti);
void AddPowerWireTileDesc(TileIndex tile, TileDesc &td);
void AllocatePowerLayer();

/** Facts about the network a power tile belongs to, for the land information window. */
struct PowerNetworkInfo {
	uint pylons = 0; ///< Pylons, including transition pylons.
	uint substations = 0; ///< Substations.
	uint spans = 0; ///< Spans of overhead wire.
};
PowerNetworkInfo GetPowerNetworkInfo(TileIndex tile);

Window *ShowBuildPowerToolbar();

#endif /* POWER_FUNC_H */
