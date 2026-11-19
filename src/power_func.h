/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file power_func.h Functions related to power lines, used outside the power line code. */

#ifndef POWER_FUNC_H
#define POWER_FUNC_H

#include "industry_type.h"

#include "command_type.h"
#include "economy_type.h"
#include "company_type.h"
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
/** Whether DrawPowerWireAbove() draws cable markers and the network view; the 3D view turns it off where it draws them with models. */
extern bool _power_cable_sprites;
/** Whether the power network view is on: underground cables are shown. Not saved; each player chooses. */
extern bool _power_network_view;
void SetPowerNetworkView(bool on);
void DrawPowerWireAbove(const TileInfo *ti);
void AddPowerWireTileDesc(TileIndex tile, TileDesc &td);
void AllocatePowerLayer();
void ChangePowerCableOwner(Owner old_owner, Owner new_owner);

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

/** What drives a power plant. */
enum class PowerPlantSource : uint8_t {
	Fuel, ///< Coal or oil delivered to it.
	Solar, ///< The sun.
	Wind, ///< The wind.
	Hydro, ///< A river.
};
PowerPlantSource GetPowerPlantSource(IndustryType type);
uint GetPowerPlantWeatherFactor(PowerPlantSource source);
enum class IndustryAvailabilityCallType : uint8_t;
bool IsPowerPlantTypeAvailable(IndustryType type, IndustryAvailabilityCallType creation);
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

/** Status light of a substation, or of a pylon joining a power plant to the grid. */
enum class PowerTileStatus : uint8_t {
	None, ///< No light: a plain pylon, or not balanced yet.
	Flowing, ///< Green: energy flows on its network.
	Idle, ///< Yellow: its network joins plants and consumers, but nothing flows now.
	Unconnected, ///< Red: its network lacks plants or consumers.
};

/** What a substation, or a pylon joining a power plant, did in the last balancing step. Power in tenths of MW. */
struct PowerTileFigures {
	PowerTileStatus status = PowerTileStatus::None; ///< Its status light.
	bool plant_link = false; ///< A pylon joining a power plant, rather than a substation.
	uint network = 0; ///< Number of its network.
	uint plants = 0; ///< Power plants it covers.
	uint consumers = 0; ///< Industries and buildings it covers.
	uint supply = 0; ///< What its plants offered.
	uint demand = 0; ///< What its consumers needed.
	uint sent = 0; ///< What it took from its plants into the network, losses included.
	uint received = 0; ///< What arrived for its consumers.
	uint capacity = 0; ///< Most it can pass each way.
};

/** Totals of a network in the last balancing step. Power in tenths of MW. */
struct PowerNetworkFigures {
	uint substations = 0; ///< Substations, and pylons joining plants.
	uint supply = 0; ///< What all its plants offered.
	uint demand = 0; ///< What all its consumers needed.
	uint sent = 0; ///< What left the plants.
	uint received = 0; ///< What arrived at consumers.
	bool connected = false; ///< Whether it joins at least one plant and one consumer.
};

PowerTileFigures GetPowerTileFigures(TileIndex tile);
PowerNetworkFigures GetPowerNetworkFigures(TileIndex tile);
PowerTileStatus GetPowerTileStatus(TileIndex tile);
bool IsPowerPlantLink(TileIndex tile);
bool IsIndustryOnPowerGrid(const Industry *i);
void InvalidatePowerNetworks();
void ShowPowerTileWindow(TileIndex tile);
bool IsPowerLineOpenTo(Owner owner, Owner user);

/** Upkeep of one piece of power infrastructure compared to a piece of rail, in eighths. */
static constexpr uint POWER_UPKEEP_WIRE = 8;
static constexpr uint POWER_UPKEEP_PYLON = 16;
static constexpr uint POWER_UPKEEP_CABLE = 32;
static constexpr uint POWER_UPKEEP_SUBSTATION = 96;

/** The power lines a company owns. */
struct PowerInfrastructure {
	uint32_t pylons = 0; ///< Pylons and transition pylons.
	uint32_t substations = 0; ///< Overhead and underground substations.
	uint32_t wire = 0; ///< Tiles with its overhead wire above.
	uint32_t cable = 0; ///< Tiles with its underground cable, joints not counted.

	/** @return All pieces, weighted by their upkeep, for how fast the upkeep grows. */
	uint32_t Units() const { return (this->wire * POWER_UPKEEP_WIRE + this->pylons * POWER_UPKEEP_PYLON + this->cable * POWER_UPKEEP_CABLE + this->substations * POWER_UPKEEP_SUBSTATION) / 8; }
};

const PowerInfrastructure &GetPowerInfrastructure(CompanyID owner);
Money PowerMaintenanceCost(const PowerInfrastructure &infra, uint32_t count, uint weight);
Money PowerMaintenanceCost(const PowerInfrastructure &infra);
bool GetPowerLinePreviewPlan(TileIndex start_tile, TileIndex end_tile, std::vector<TileIndex> &pylons, StringID &error);
bool IsPowerLineDragActive();
bool GetPowerLinePlanPylons(TileIndex start, TileIndex end, std::vector<TileIndex> &pylons);
bool GetPowerLineDragHighlight(const struct TileInfo *ti, std::optional<PaletteID> &highlight);

#endif /* POWER_FUNC_H */
