/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file traction_power.cpp Traction power: tram and trolleybus wire as a consumer of the energy networks.
 *
 * Fork-only, built only with the energy transport (see traction.h).
 *
 * - A substation feeds the street wire around it: every wired road or tram tile within its catchment, and
 *   from there along continuous wire of the same circuit up to economy.traction_feed_distance tiles.
 *   Road-half wire (trolleybuses) and tram-half wire (trams) are separate circuits.
 * - At each balancing step a fed tile asks 0.02 MW, and every tram or trolleybus asks power for where it is:
 *   while moving under power about a quarter of a MW per 300 hp, a tenth of that while standing.
 *   What a tile or vehicle asks is split evenly between the substations that feed its tile.
 * - The energy balancing serves this demand like any other consumer. The share each substation gets is written
 *   to the power layer for its tiles (bits 20..23 road-half wire, 24..27 tram-half wire; 0 = not fed,
 *   1..15 = fed, with 0% to 100%), so it is saved with the layer and every computer sees the same.
 * - With economy.electric_vehicles_need_power on, a vehicle gets the top speed of the share its wire gets,
 *   at least a quarter.
 * - The owners of the vehicles pay for the energy their vehicles took, as road vehicle running costs; their
 *   fixed running cost is a third of what it would be. The owner of the plant-side substation is paid for it by
 *   the energy balancing, as for any consumer.
 */

#include "stdafx.h"
#include "traction.h"
#include "power_func.h"
#include "power_map.h"
#include "roadveh.h"
#include "road_map.h"
#include "road_func.h"
#include "road.h"
#include "core/backup_type.hpp"
#include "tunnelbridge_map.h"
#include "company_base.h"
#include "company_func.h"
#include "economy_func.h"
#include "news_func.h"
#include "strings_func.h"
#include "settings_type.h"
#include "core/math_func.hpp"

#include "table/strings.h"

#include <map>
#include <set>
#include <unordered_map>

#include "safeguards.h"

/** First bit of the share in the power layer, for road-half and tram-half wire. */
static constexpr uint8_t TRACTION_LAYER_BIT[2] = {20, 24};
/** Demand of a fed tile of wire, in hundredths of a MW. */
static constexpr uint TRACTION_TILE_DEMAND = 2;

/**
 * Get the share of power a tile's wire gets, as stored in the power layer.
 * @param tile The tile.
 * @param rtt The circuit: road half or tram half.
 * @return 0 if not fed, otherwise 1 (nothing) to 15 (everything it asks).
 */
static uint GetTractionLayerShare(TileIndex tile, RoadTramType rtt)
{
	if (_power_layer == nullptr) return 0;
	return GB(_power_layer[tile.base()], TRACTION_LAYER_BIT[to_underlying(rtt)], 4);
}

/**
 * Store the share of power a tile's wire gets.
 * @param tile The tile.
 * @param rtt The circuit.
 * @param share 0 if not fed, otherwise 1 to 15.
 */
static void SetTractionLayerShare(TileIndex tile, RoadTramType rtt, uint share)
{
	SB(_power_layer[tile.base()], TRACTION_LAYER_BIT[to_underlying(rtt)], 4, share);
}

/**
 * Whether a tile has wire of a circuit.
 * @param tile The tile.
 * @param rtt The circuit.
 * @return True if the road type of that half of the tile has overhead wire.
 */
static bool HasTractionWire(TileIndex tile, RoadTramType rtt)
{
	if (!MayHaveRoad(tile)) return false;
	RoadType rt = GetRoadType(tile, rtt);
	return rt != INVALID_ROADTYPE && HasRoadCatenary(rt);
}

/**
 * Find the tile a piece of wire leads to.
 * @param tile A tile with wire.
 * @param rtt The circuit.
 * @param dir The direction to go.
 * @return The next tile with wire joined to this one in that direction, or INVALID_TILE.
 */
static TileIndex GetNextTractionTile(TileIndex tile, RoadTramType rtt, DiagDirection dir)
{
	if (!GetAnyRoadBits(tile, rtt, true).Any(DiagDirToRoadBits(dir))) return INVALID_TILE;
	if (IsTileType(tile, TileType::TunnelBridge) && GetTunnelBridgeDirection(tile) == dir) {
		/* Through the tunnel or over the bridge. */
		TileIndex end = GetOtherTunnelBridgeEnd(tile);
		return HasTractionWire(end, rtt) ? end : INVALID_TILE;
	}
	TileIndex next = TileAddByDiagDir(tile, dir);
	if (!IsValidTile(next) || !HasTractionWire(next, rtt)) return INVALID_TILE;
	/* A tunnel or bridge is entered only through its mouth, not from the tile under the bridge. */
	if (IsTileType(next, TileType::TunnelBridge) && GetTunnelBridgeDirection(next) != dir) return INVALID_TILE;
	if (!GetAnyRoadBits(next, rtt, true).Any(DiagDirToRoadBits(ReverseDiagDir(dir)))) return INVALID_TILE;
	return next;
}

/** A tile of one circuit, as a key: the tile index shifted left by one, plus the circuit. */
using TractionKey = uint32_t;

static TractionKey MakeTractionKey(TileIndex tile, RoadTramType rtt) { return (tile.base() << 1) | to_underlying(rtt); }
static TileIndex GetTractionKeyTile(TractionKey key) { return TileIndex{key >> 1}; }
static RoadTramType GetTractionKeyCircuit(TractionKey key) { return static_cast<RoadTramType>(key & 1); }

/** What the balancing step in progress knows; filled by PrepareTractionDemand(), used by ApplyTractionSupply(). */
struct TractionStep {
	std::unordered_map<TractionKey, std::vector<uint16_t>> fed; ///< The substations feeding each fed tile.
	std::vector<uint> demand; ///< For each substation, what its wire asks, in hundredths of MW.
	std::vector<std::map<CompanyID, uint>> draw; ///< For each substation, what the vehicles of each company ask, in hundredths of MW.
	std::vector<std::vector<VehicleID>> vehicles; ///< For each substation, the vehicles it feeds.
};
static TractionStep _traction_step;

/** The tiles that had a share written in the power layer, so it is taken off again when they are no longer fed. */
static std::vector<TractionKey> _traction_fed_tiles;
/** The power layer #_traction_fed_tiles was made for; another layer means a new game or a loaded one. */
static const uint32_t *_traction_fed_layer = nullptr;

/** Make #_traction_fed_tiles again from the power layer, after a game was started or loaded. */
static void FindTractionFedTiles()
{
	_traction_fed_layer = _power_layer.get();
	_traction_fed_tiles.clear();
	for (TileIndex tile : Map::Iterate()) {
		for (RoadTramType rtt : {RoadTramType::Road, RoadTramType::Tram}) {
			if (GetTractionLayerShare(tile, rtt) != 0) _traction_fed_tiles.push_back(MakeTractionKey(tile, rtt));
		}
	}
}

/**
 * Power a vehicle asks for, in hundredths of a MW.
 * @param v The front of a tram or trolleybus.
 * @return Its demand.
 */
static uint GetTractionVehicleDraw(const RoadVehicle *v)
{
	uint hp = v->gcache.cached_power;
	/* About a quarter of a MW per 300 hp while moving under power (0.75 kW per hp), a tenth of that standing. */
	uint moving = 1 + hp * 75 / 1000;
	if (v->cur_speed == 0 || v->vehstatus.Test(VehState::Stopped)) return 1 + moving / 10;
	return moving;
}

/**
 * Find what the wire fed by each substation asks for this balancing step.
 * @param substations All substations of all networks.
 * @return For each substation, the demand of its wire and vehicles, in tenths of MW.
 */
std::vector<uint> PrepareTractionDemand(std::span<const TileIndex> substations)
{
	TractionStep &step = _traction_step;
	step = {};
	step.demand.resize(substations.size());
	step.draw.resize(substations.size());
	step.vehicles.resize(substations.size());

	/* Feed: from the wired tiles in the catchment, along the wire. */
	int radius = _settings_game.economy.power_catchment;
	uint feed = _settings_game.economy.traction_feed_distance;
	for (uint s = 0; s < substations.size(); s++) {
		for (RoadTramType rtt : {RoadTramType::Road, RoadTramType::Tram}) {
			std::map<TractionKey, uint> depth;
			std::vector<TileIndex> queue;
			for (int dy = -radius; dy <= radius; dy++) {
				for (int dx = -radius; dx <= radius; dx++) {
					TileIndex tile = TileAddWrap(substations[s], dx, dy);
					if (tile == INVALID_TILE || !HasTractionWire(tile, rtt)) continue;
					depth[MakeTractionKey(tile, rtt)] = 0;
					queue.push_back(tile);
				}
			}
			for (size_t i = 0; i < queue.size(); i++) {
				TileIndex tile = queue[i];
				uint d = depth[MakeTractionKey(tile, rtt)];
				if (d >= feed) continue;
				for (DiagDirection dir : EnumRange(DiagDirection::End)) {
					TileIndex next = GetNextTractionTile(tile, rtt, dir);
					if (next == INVALID_TILE) continue;
					if (depth.emplace(MakeTractionKey(next, rtt), d + 1).second) queue.push_back(next);
				}
			}
			for (const auto &[key, d] : depth) step.fed[key].push_back(static_cast<uint16_t>(s));
		}
	}

	/* Demand of the wire itself. */
	for (const auto &[key, feeders] : step.fed) {
		for (uint16_t s : feeders) step.demand[s] += TRACTION_TILE_DEMAND;
	}

	/* Demand of the vehicles, where they are. */
	for (const RoadVehicle *v : RoadVehicle::Iterate()) {
		if (!v->IsFrontEngine() || !HasRoadCatenary(v->roadtype) || v->IsInDepot() || v->vehstatus.Test(VehState::Crashed)) continue;
		auto it = step.fed.find(MakeTractionKey(v->tile, GetRoadTramType(v->roadtype)));
		if (it == step.fed.end()) continue;
		uint draw = GetTractionVehicleDraw(v);
		uint part = CeilDiv(draw, static_cast<uint>(it->second.size()));
		for (uint16_t s : it->second) {
			step.demand[s] += part;
			step.draw[s][v->owner] += part;
			step.vehicles[s].push_back(v->index);
		}
	}

	std::vector<uint> tenths(substations.size());
	for (uint s = 0; s < substations.size(); s++) tenths[s] = CeilDiv(step.demand[s], 10U);
	return tenths;
}

/**
 * Write the shares of the balancing step to the power layer, bill the owners of the vehicles and tell them of short supply.
 * @param delivered For each substation, what arrived for its wire, in tenths of MW.
 */
void ApplyTractionSupply(std::span<const uint> delivered)
{
	TractionStep &step = _traction_step;
	if (_traction_fed_layer != _power_layer.get()) FindTractionFedTiles();

	/* Share of each substation, 0..255. */
	std::vector<uint> share(step.demand.size(), 255);
	for (uint s = 0; s < step.demand.size() && s < delivered.size(); s++) {
		uint asked = CeilDiv(step.demand[s], 10U);
		if (asked != 0) share[s] = std::min<uint>(255, delivered[s] * 255 / asked);
	}

	/* Take the share off tiles that are no longer fed, then write the new ones. */
	for (TractionKey key : _traction_fed_tiles) {
		if (!step.fed.contains(key)) SetTractionLayerShare(GetTractionKeyTile(key), GetTractionKeyCircuit(key), 0);
	}
	_traction_fed_tiles.clear();
	for (const auto &[key, feeders] : step.fed) {
		uint sum = 0;
		for (uint16_t s : feeders) sum += share[s];
		uint avg = sum / static_cast<uint>(feeders.size());
		SetTractionLayerShare(GetTractionKeyTile(key), GetTractionKeyCircuit(key), 1 + avg * 14 / 255);
		_traction_fed_tiles.push_back(key);
	}
	/* Keep the list in a fixed order, so it does not depend on the hash map. */
	std::ranges::sort(_traction_fed_tiles);

	/* Bill the owners of the vehicles for what their vehicles took, at the price the energy is sold for. The
	 * amounts are small, so they are charged in 1/256 pound, like running costs. */
	std::map<CompanyID, uint64_t> bill; // hundredths of MW
	for (uint s = 0; s < step.demand.size() && s < delivered.size(); s++) {
		if (step.demand[s] == 0) continue;
		for (const auto &[owner, draw] : step.draw[s]) {
			bill[owner] += static_cast<uint64_t>(delivered[s]) * 10 * draw / step.demand[s];
		}
	}
	for (const auto &[owner, energy] : bill) {
		if (!Company::IsValidID(owner) || energy == 0) continue;
		/* The price is for 10 MW, that is 1000 hundredths. */
		Money cost = _price[Price::PowerSale] * static_cast<int64_t>(energy) * 256 / 1000;
		if (cost == 0) continue;
		Backup<CompanyID> cur_company(_current_company, owner);
		SubtractMoneyFromCompanyFract(owner, CommandCost(ExpensesType::RoadVehRun, cost));
		cur_company.Restore();
	}

	/* Tell the local company, once per vehicle, when one of its vehicles is short of power. Only a message, so it
	 * may differ between computers. */
	if (_settings_game.economy.electric_vehicles_need_power) {
		static std::set<VehicleID> warned;
		for (uint s = 0; s < step.vehicles.size(); s++) {
			if (share[s] >= 230) continue;
			for (VehicleID id : step.vehicles[s]) {
				const RoadVehicle *v = RoadVehicle::GetIfValid(id);
				if (v == nullptr || v->owner != _local_company || !warned.insert(id).second) continue;
				AddVehicleAdviceNewsItem(AdviceType::VehicleUnprofitable, GetEncodedString(STR_NEWS_VEHICLE_SHORT_OF_POWER, v->index, share[s] * 100 / 255), v->index);
			}
		}
		/* Running on wire that no substation feeds. */
		for (const RoadVehicle *v : RoadVehicle::Iterate()) {
			if (v->owner != _local_company || !v->IsFrontEngine() || !HasRoadCatenary(v->roadtype) || v->IsInDepot()) continue;
			if (step.fed.contains(MakeTractionKey(v->tile, GetRoadTramType(v->roadtype))) || !warned.insert(v->index).second) continue;
			AddVehicleAdviceNewsItem(AdviceType::VehicleUnprofitable, GetEncodedString(STR_NEWS_VEHICLE_NOT_FED, v->index), v->index);
		}
	}
	step = {};
}

/**
 * Get the share of power the wire over a tile gets.
 * @param tile The tile.
 * @param rtt The circuit.
 * @return 0 to 100 percent, or -1 if no substation feeds it.
 */
int GetTractionSupplyPercent(TileIndex tile, RoadTramType rtt)
{
	uint v = GetTractionLayerShare(tile, rtt);
	if (v == 0) return -1;
	return static_cast<int>((v - 1) * 100 / 14);
}

/**
 * How much of its speed an electric vehicle may use, for the power its wire gets.
 * @param v The vehicle.
 * @return 64 (a quarter) to 256 (all); always 256 unless economy.electric_vehicles_need_power is on.
 */
uint GetTractionSpeedFactor(const RoadVehicle *v)
{
	if (!_settings_game.economy.electric_vehicles_need_power || _power_layer == nullptr) return 256;
	const RoadVehicle *front = v->First();
	if (!HasRoadCatenary(front->roadtype)) return 256;
	uint stored = GetTractionLayerShare(front->tile, GetRoadTramType(front->roadtype));
	uint share = stored == 0 ? 0 : (stored - 1) * 256 / 14;
	return std::clamp<uint>(share, 64, 256);
}

/**
 * The fixed part of the running cost of a road vehicle: a third for trams and trolleybuses, which buy their energy.
 * @param rt The road type of the vehicle.
 * @param cost Its running cost.
 * @return The fixed part.
 */
Money GetTractionFixedRunningCost(RoadType rt, Money cost)
{
	if (rt == INVALID_ROADTYPE || !HasRoadCatenary(rt)) return cost;
	return cost / 3;
}
