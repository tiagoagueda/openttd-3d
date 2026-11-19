/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file script_power.hpp Everything to query and build power lines. */

#ifndef SCRIPT_POWER_HPP
#define SCRIPT_POWER_HPP

#include "script_company.hpp"
#include "script_error.hpp"

/**
 * Class that handles all power line related functions: pylons, overhead lines, underground cables and substations.
 * Energy is measured in tenths of MW. Power plants feed the network through a substation that covers them, or through
 * a pylon right next to them; consumers draw from substations that cover them.
 * @api ai game
 */
class ScriptPower : public ScriptObject {
public:
	/**
	 * All power line related error messages.
	 */
	enum ErrorMessages {
		/** Base for power line related errors */
		ERR_POWER_BASE = ScriptError::ERR_CAT_POWER << ScriptError::ERR_CAT_BIT_SIZE,

		/** A power line is in the way */
		ERR_POWER_LINE_IN_THE_WAY,                   // [STR_ERROR_MUST_REMOVE_POWER_LINE_FIRST]

		/** A bridge is in the way of the power line */
		ERR_POWER_BRIDGE_IN_THE_WAY,                 // [STR_ERROR_POWER_LINE_BRIDGE_IN_THE_WAY]

		/** Power lines cannot cross between pylons */
		ERR_POWER_LINES_CANNOT_CROSS,                // [STR_ERROR_POWER_LINES_CANNOT_CROSS]

		/** Power lines can only join at a pylon */
		ERR_POWER_LINES_JOIN_ONLY_AT_PYLONS,         // [STR_ERROR_POWER_LINES_JOIN_ONLY_AT_PYLONS]

		/** Power lines cannot pass over airports or industries */
		ERR_POWER_LINE_OVER_AIRPORT_OR_INDUSTRY,     // [STR_ERROR_POWER_LINE_OVER_AIRPORT, STR_ERROR_POWER_LINE_OVER_INDUSTRY]

		/** A building is too tall for the power line to pass over */
		ERR_POWER_LINE_STRUCTURE_TOO_TALL,           // [STR_ERROR_POWER_LINE_TALL_STRUCTURE]

		/** The power line would hang too low between its pylons */
		ERR_POWER_LINE_TOO_LOW,                      // [STR_ERROR_POWER_LINE_TOO_LOW]

		/** A pylon cannot stand under a power line */
		ERR_POWER_PYLON_UNDER_WIRE,                  // [STR_ERROR_POWER_PYLON_UNDER_WIRE]

		/** Pylons need open land */
		ERR_POWER_PYLON_NEEDS_OPEN_LAND,             // [STR_ERROR_POWER_PYLON_NEEDS_OPEN_LAND]

		/** The pylons are too far apart in height */
		ERR_POWER_PYLONS_HEIGHT_DIFFERENCE,          // [STR_ERROR_POWER_PYLONS_HEIGHT_DIFFERENCE]

		/** The underground cable belongs to another company */
		ERR_POWER_CABLE_OWNED_BY_OTHER,              // [STR_ERROR_POWER_CABLE_OWNED_BY_OTHER]
	};

	/** Kinds of power tiles. */
	enum PowerTileType {
		/* Note: the values _are_ important as they represent an in-game value */
		PT_PYLON = 0, ///< A pylon carrying overhead wires.
		PT_TRANSITION_PYLON = 1, ///< A pylon where an overhead line goes underground.
		PT_SUBSTATION = 2, ///< A substation that overhead lines connect to.
		PT_UNDERGROUND_SUBSTATION = 3, ///< A substation that underground cables connect to.

		PT_INVALID = -1, ///< Not a power tile.
	};

	/** What a substation, or a pylon joining a power plant to the grid, did in the last balancing step. */
	enum SubstationStatus {
		/* Note: the values _are_ important as they represent an in-game value */
		SS_NONE = 0, ///< Not a substation, or not balanced yet.
		SS_FLOWING = 1, ///< Energy flows through its network.
		SS_IDLE = 2, ///< Its network has plants and consumers, but nothing flows.
		SS_UNCONNECTED = 3, ///< Its network lacks plants or consumers.
	};

	/**
	 * Check whether a tile is a power tile: a pylon, transition pylon or substation.
	 * @param tile The tile to check.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @return True if and only if the tile is a power tile.
	 */
	static bool IsPowerTile(TileIndex tile);

	/**
	 * Get the kind of a power tile.
	 * @param tile The tile to check.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @return The kind of power tile, or PT_INVALID if it is none.
	 */
	static PowerTileType GetPowerTileType(TileIndex tile);

	/**
	 * Check whether an overhead wire passes over a tile, between two pylons.
	 * @param tile The tile to check.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @return True if and only if a wire passes over the tile.
	 */
	static bool HasWire(TileIndex tile);

	/**
	 * Check whether an underground cable runs under a tile.
	 * @param tile The tile to check.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @return True if and only if a cable runs under the tile.
	 */
	static bool HasCable(TileIndex tile);

	/**
	 * Get the owner of the overhead wire passing over a tile.
	 * @param tile The tile to check.
	 * @pre HasWire(tile).
	 * @return The company that owns the wire.
	 */
	static ScriptCompany::CompanyID GetWireOwner(TileIndex tile);

	/**
	 * Get the owner of the underground cable under a tile.
	 * @param tile The tile to check.
	 * @pre HasCable(tile).
	 * @return The company that owns the cable.
	 */
	static ScriptCompany::CompanyID GetCableOwner(TileIndex tile);

	/**
	 * Get the longest span of overhead wire between two pylons.
	 * @return The most tiles from one pylon to the next.
	 */
	static SQInteger GetMaxSpan();

	/**
	 * Build a pylon.
	 * @param tile The tile to build on.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @game @pre ScriptCompanyMode::IsValid().
	 * @return Whether the pylon has been built.
	 */
	static bool BuildPylon(TileIndex tile);

	/**
	 * Build a transition pylon, where an overhead line meets an underground cable. An existing pylon can be turned into one.
	 * @param tile The tile to build on.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @game @pre ScriptCompanyMode::IsValid().
	 * @return Whether the transition pylon has been built.
	 */
	static bool BuildTransitionPylon(TileIndex tile);

	/**
	 * Build a substation for overhead lines.
	 * @param tile The tile to build on.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @game @pre ScriptCompanyMode::IsValid().
	 * @return Whether the substation has been built.
	 */
	static bool BuildSubstation(TileIndex tile);

	/**
	 * Build a substation for underground cables.
	 * @param tile The tile to build on.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @game @pre ScriptCompanyMode::IsValid().
	 * @return Whether the substation has been built.
	 */
	static bool BuildUndergroundSubstation(TileIndex tile);

	/**
	 * Build an overhead line between two tiles, along an axis or a diagonal. Pylons are placed where they are needed,
	 * as far apart as allowed; pylons and substations on the way are used.
	 * @param start The tile where the line starts.
	 * @param end The tile where the line ends.
	 * @pre ScriptMap::IsValidTile(start).
	 * @pre ScriptMap::IsValidTile(end).
	 * @pre 'start' and 'end' are on a straight or diagonal line.
	 * @game @pre ScriptCompanyMode::IsValid().
	 * @return Whether the line has been built.
	 */
	static bool BuildLine(TileIndex start, TileIndex end);

	/**
	 * Remove the overhead wires between two tiles, and the pylons left without wires.
	 * @param start The tile where the stretch starts.
	 * @param end The tile where the stretch ends.
	 * @pre ScriptMap::IsValidTile(start).
	 * @pre ScriptMap::IsValidTile(end).
	 * @pre 'start' and 'end' are on a straight or diagonal line.
	 * @game @pre ScriptCompanyMode::IsValid().
	 * @return Whether wires have been removed.
	 */
	static bool RemoveLine(TileIndex start, TileIndex end);

	/**
	 * Lay an underground cable between two tiles, along an axis or a diagonal.
	 * Cables join wherever they meet; they connect to the grid only at underground substations and transition pylons.
	 * @param start The tile where the cable starts.
	 * @param end The tile where the cable ends.
	 * @pre ScriptMap::IsValidTile(start).
	 * @pre ScriptMap::IsValidTile(end).
	 * @pre 'start' and 'end' are on a straight or diagonal line.
	 * @game @pre ScriptCompanyMode::IsValid().
	 * @return Whether the cable has been laid.
	 */
	static bool BuildCable(TileIndex start, TileIndex end);

	/**
	 * Remove the underground cable between two tiles.
	 * @param start The tile where the stretch starts.
	 * @param end The tile where the stretch ends.
	 * @pre ScriptMap::IsValidTile(start).
	 * @pre ScriptMap::IsValidTile(end).
	 * @pre 'start' and 'end' are on a straight or diagonal line.
	 * @game @pre ScriptCompanyMode::IsValid().
	 * @return Whether cable has been removed.
	 */
	static bool RemoveCable(TileIndex start, TileIndex end);

	/**
	 * Remove a pylon, transition pylon or substation, with the wires that end on it.
	 * @param tile The power tile.
	 * @pre IsPowerTile(tile).
	 * @game @pre ScriptCompanyMode::IsValid().
	 * @return Whether the power tile has been removed.
	 */
	static bool RemovePowerTile(TileIndex tile);

	/**
	 * Check whether a pylon joins a power plant to the grid by standing next to it.
	 * @param tile The tile to check.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @return True if and only if the tile is a pylon or transition pylon next to a power plant.
	 */
	static bool IsPlantLink(TileIndex tile);

	/**
	 * Get the status of a substation, or of a pylon joining a power plant to the grid, in the last balancing step.
	 * @param tile The substation.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @return The status; SS_NONE for other tiles.
	 */
	static SubstationStatus GetSubstationStatus(TileIndex tile);

	/**
	 * Get the energy a substation took from the plants it covers in the last balancing step, losses on the way included.
	 * @param tile The substation.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @return The energy, in tenths of MW; 0 for other tiles.
	 */
	static SQInteger GetSubstationSent(TileIndex tile);

	/**
	 * Get the energy that arrived at a substation for the consumers it covers in the last balancing step.
	 * @param tile The substation.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @return The energy, in tenths of MW; 0 for other tiles.
	 */
	static SQInteger GetSubstationReceived(TileIndex tile);

	/**
	 * Get the most energy a substation can pass each way in a balancing step.
	 * @param tile The substation.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @return The capacity, in tenths of MW; 0 for other tiles.
	 */
	static SQInteger GetSubstationCapacity(TileIndex tile);

	/**
	 * Get what the plants covered by all substations of the network of a substation can give.
	 * @param tile A substation of the network.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @return The supply, in tenths of MW; 0 for other tiles.
	 */
	static SQInteger GetNetworkSupply(TileIndex tile);

	/**
	 * Get what the consumers covered by all substations of the network of a substation need.
	 * @param tile A substation of the network.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @return The demand, in tenths of MW; 0 for other tiles.
	 */
	static SQInteger GetNetworkDemand(TileIndex tile);

	/**
	 * Get what arrived at the consumers of the network of a substation in the last balancing step.
	 * @param tile A substation of the network.
	 * @pre ScriptMap::IsValidTile(tile).
	 * @return The energy, in tenths of MW; 0 for other tiles.
	 */
	static SQInteger GetNetworkReceived(TileIndex tile);

	/**
	 * Check whether a company lets other companies use its power lines, for a fee.
	 * @param company The company.
	 * @pre ScriptCompany::ResolveCompanyID(company) != ScriptCompany::COMPANY_INVALID.
	 * @return True if and only if it shares its power lines.
	 */
	static bool IsSharing(ScriptCompany::CompanyID company);

	/**
	 * Let other companies use the power lines of your company, for a fee, or stop that.
	 * @param share Whether to share.
	 * @game @pre ScriptCompanyMode::IsValid().
	 * @return Whether the switch has been set.
	 */
	static bool SetSharing(bool share);
};

#endif /* SCRIPT_POWER_HPP */
