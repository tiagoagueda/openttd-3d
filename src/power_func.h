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
#include "cargo_type.h"

struct Window;

/* Checks used by other build commands, so that nothing tall is built under a wire. */
int GetPowerWireHeight(TileIndex tile);
CommandCost CheckPowerWireClearance(TileIndex tile, int wire_z);
CommandCost CheckPowerWireAllowsStructure(TileIndex tile, int height);
bool IsHouseLowForPowerWire(HouseID house);

/** File with the sprites of power lines, in the base set folder. */
static constexpr std::string_view POWER_GRF = "power.grf";
/** Whether the sprites of power.grf are loaded. */
extern bool _power_sprites_loaded;
/** Whether DrawPowerWireAbove() draws; the 3D view turns it off where it draws the wires with models. */
extern bool _power_wire_sprites;
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

/* The energy economy (power_economy.cpp). */
struct Industry;
struct Town;
extern uint32_t _power_network_version;
void OnTick_Power();
void AddPowerPlantFuel(Industry *i, CargoType cargo, uint amount);
bool IsPowerPlant(const Industry *i);
uint GetPowerPlantFuelDays(const Industry *i);
uint ApplyIndustryPowerBonus(Industry *i, uint amount);
uint ApplyTownPowerBonus(Town *t, uint amount);
uint ApplyTownPowerGrowthBonus(const Town *t, uint ticks);
uint GetPowerBonusPercent(uint demand, uint supplied);

/** What a substation covers, for the land information window. Power in tenths of MW. */
struct PowerSubstationFigures {
	uint plants = 0; ///< Power plants in its catchment.
	uint industries = 0; ///< Other industries in its catchment.
	uint houses = 0; ///< Buildings in its catchment.
	uint supply = 0; ///< What the plants sold in the last step.
	uint demand = 0; ///< What the consumers need.
};
PowerSubstationFigures GetPowerSubstationFigures(TileIndex tile);

#endif /* POWER_FUNC_H */
