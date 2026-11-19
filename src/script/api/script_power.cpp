/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file script_power.cpp Implementation of ScriptPower. */

#include "../../stdafx.h"
#include "script_power.hpp"
#include "script_error.hpp"
#include "script_map.hpp"
#include "../../company_base.h"
#include "../../power_cmd.h"
#include "../../power_func.h"
#include "../../power_map.h"
#include "../../landscape_cmd.h"

#include "../../safeguards.h"

/* static */ bool ScriptPower::IsPowerTile(TileIndex tile)
{
	if (!::IsValidTile(tile)) return false;
	return ::IsPowerTile(tile);
}

/* static */ ScriptPower::PowerTileType ScriptPower::GetPowerTileType(TileIndex tile)
{
	if (!IsPowerTile(tile)) return PT_INVALID;
	return static_cast<PowerTileType>(::GetPowerTileKind(tile));
}

/* static */ bool ScriptPower::HasWire(TileIndex tile)
{
	if (!::IsValidTile(tile)) return false;
	return ::HasPowerWire(tile);
}

/* static */ bool ScriptPower::HasCable(TileIndex tile)
{
	if (!::IsValidTile(tile)) return false;
	return ::HasPowerCable(tile);
}

/* static */ ScriptCompany::CompanyID ScriptPower::GetWireOwner(TileIndex tile)
{
	if (!HasWire(tile)) return ScriptCompany::COMPANY_INVALID;
	return ScriptCompany::ToScriptCompanyID(::GetPowerWireOwner(tile));
}

/* static */ ScriptCompany::CompanyID ScriptPower::GetCableOwner(TileIndex tile)
{
	if (!HasCable(tile)) return ScriptCompany::COMPANY_INVALID;
	return ScriptCompany::ToScriptCompanyID(::GetPowerCableOwner(tile));
}

/* static */ SQInteger ScriptPower::GetMaxSpan()
{
	return POWER_MAX_SPAN;
}

/* static */ bool ScriptPower::BuildPylon(TileIndex tile)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, ::IsValidTile(tile));
	return ScriptObject::Command<Commands::BuildPowerPylon>::Do(tile, PowerTileKind::Pylon);
}

/* static */ bool ScriptPower::BuildTransitionPylon(TileIndex tile)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, ::IsValidTile(tile));
	return ScriptObject::Command<Commands::BuildPowerPylon>::Do(tile, PowerTileKind::TransitionPylon);
}

/* static */ bool ScriptPower::BuildSubstation(TileIndex tile)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, ::IsValidTile(tile));
	return ScriptObject::Command<Commands::BuildPowerSubstation>::Do(tile, PowerTileKind::OverheadSubstation);
}

/* static */ bool ScriptPower::BuildUndergroundSubstation(TileIndex tile)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, ::IsValidTile(tile));
	return ScriptObject::Command<Commands::BuildPowerSubstation>::Do(tile, PowerTileKind::UndergroundSubstation);
}

/**
 * Whether two tiles lie on a straight or diagonal line.
 * @param start One tile.
 * @param end The other tile.
 * @return True if so.
 */
static bool IsPowerLineDirection(TileIndex start, TileIndex end)
{
	int dx = static_cast<int>(::TileX(end)) - static_cast<int>(::TileX(start));
	int dy = static_cast<int>(::TileY(end)) - static_cast<int>(::TileY(start));
	return dx == 0 || dy == 0 || std::abs(dx) == std::abs(dy);
}

/* static */ bool ScriptPower::BuildLine(TileIndex start, TileIndex end)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, ::IsValidTile(start));
	EnforcePrecondition(false, ::IsValidTile(end));
	EnforcePrecondition(false, IsPowerLineDirection(start, end));
	return ScriptObject::Command<Commands::BuildPowerLine>::Do(end, start);
}

/* static */ bool ScriptPower::RemoveLine(TileIndex start, TileIndex end)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, ::IsValidTile(start));
	EnforcePrecondition(false, ::IsValidTile(end));
	EnforcePrecondition(false, IsPowerLineDirection(start, end));
	return ScriptObject::Command<Commands::RemovePowerLine>::Do(end, start);
}

/* static */ bool ScriptPower::BuildCable(TileIndex start, TileIndex end)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, ::IsValidTile(start));
	EnforcePrecondition(false, ::IsValidTile(end));
	EnforcePrecondition(false, IsPowerLineDirection(start, end));
	return ScriptObject::Command<Commands::BuildPowerCable>::Do(end, start);
}

/* static */ bool ScriptPower::RemoveCable(TileIndex start, TileIndex end)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, ::IsValidTile(start));
	EnforcePrecondition(false, ::IsValidTile(end));
	EnforcePrecondition(false, IsPowerLineDirection(start, end));
	return ScriptObject::Command<Commands::RemovePowerCable>::Do(end, start);
}

/* static */ bool ScriptPower::RemovePowerTile(TileIndex tile)
{
	EnforceCompanyModeValid(false);
	EnforcePrecondition(false, IsPowerTile(tile));
	return ScriptObject::Command<Commands::LandscapeClear>::Do(tile);
}

/* static */ bool ScriptPower::IsPlantLink(TileIndex tile)
{
	if (!::IsValidTile(tile)) return false;
	return ::IsPowerPlantLink(tile);
}

/* static */ ScriptPower::SubstationStatus ScriptPower::GetSubstationStatus(TileIndex tile)
{
	if (!IsPowerTile(tile)) return SS_NONE;
	return static_cast<SubstationStatus>(::GetPowerTileFigures(tile).status);
}

/* static */ SQInteger ScriptPower::GetSubstationSent(TileIndex tile)
{
	if (!IsPowerTile(tile)) return 0;
	return ::GetPowerTileFigures(tile).sent;
}

/* static */ SQInteger ScriptPower::GetSubstationReceived(TileIndex tile)
{
	if (!IsPowerTile(tile)) return 0;
	return ::GetPowerTileFigures(tile).received;
}

/* static */ SQInteger ScriptPower::GetSubstationCapacity(TileIndex tile)
{
	if (!IsPowerTile(tile)) return 0;
	return ::GetPowerTileFigures(tile).capacity;
}

/* static */ SQInteger ScriptPower::GetNetworkSupply(TileIndex tile)
{
	if (!IsPowerTile(tile)) return 0;
	return ::GetPowerNetworkFigures(tile).supply;
}

/* static */ SQInteger ScriptPower::GetNetworkDemand(TileIndex tile)
{
	if (!IsPowerTile(tile)) return 0;
	return ::GetPowerNetworkFigures(tile).demand;
}

/* static */ SQInteger ScriptPower::GetNetworkReceived(TileIndex tile)
{
	if (!IsPowerTile(tile)) return 0;
	return ::GetPowerNetworkFigures(tile).received;
}

/* static */ bool ScriptPower::IsSharing(ScriptCompany::CompanyID company)
{
	company = ScriptCompany::ResolveCompanyID(company);
	if (company == ScriptCompany::COMPANY_INVALID) return false;
	return ::Company::Get(ScriptCompany::FromScriptCompanyID(company))->power_sharing;
}

/* static */ bool ScriptPower::SetSharing(bool share)
{
	EnforceCompanyModeValid(false);
	return ScriptObject::Command<Commands::SetPowerSharing>::Do(share);
}
