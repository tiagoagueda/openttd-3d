/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file power_cmd.h Command definitions for power lines. */

#ifndef POWER_CMD_H
#define POWER_CMD_H

#include "command_type.h"
#include "power_map.h"

CommandCost CmdBuildPowerPylon(DoCommandFlags flags, TileIndex tile, PowerTileKind kind);
CommandCost CmdBuildPowerSubstation(DoCommandFlags flags, TileIndex tile, PowerTileKind kind);
CommandCost CmdBuildPowerLine(DoCommandFlags flags, TileIndex end_tile, TileIndex start_tile);
CommandCost CmdRemovePowerLine(DoCommandFlags flags, TileIndex end_tile, TileIndex start_tile);
CommandCost CmdBuildPowerCable(DoCommandFlags flags, TileIndex end_tile, TileIndex start_tile);
CommandCost CmdRemovePowerCable(DoCommandFlags flags, TileIndex end_tile, TileIndex start_tile);
CommandCost CmdSetPowerSharing(DoCommandFlags flags, bool share);

DEF_CMD_TRAIT(Commands::BuildPowerPylon, CmdBuildPowerPylon, CommandFlags({CommandFlag::NoWater, CommandFlag::Auto}), CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(Commands::BuildPowerSubstation, CmdBuildPowerSubstation, CommandFlags({CommandFlag::NoWater, CommandFlag::Auto}), CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(Commands::BuildPowerLine, CmdBuildPowerLine, CommandFlags({CommandFlag::NoWater, CommandFlag::Auto}), CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(Commands::RemovePowerLine, CmdRemovePowerLine, CommandFlags({CommandFlag::Auto}), CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(Commands::BuildPowerCable, CmdBuildPowerCable, CommandFlags({CommandFlag::Auto}), CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(Commands::RemovePowerCable, CmdRemovePowerCable, CommandFlags({CommandFlag::Auto}), CommandType::LandscapeConstruction)
DEF_CMD_TRAIT(Commands::SetPowerSharing, CmdSetPowerSharing, {}, CommandType::CompanySetting)

#endif /* POWER_CMD_H */
