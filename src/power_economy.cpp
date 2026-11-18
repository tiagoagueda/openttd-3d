/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file power_economy.cpp The energy economy: power plants, consumers, networks of power lines and the balancing between them. */

#include "stdafx.h"
#include "power_func.h"
#include "power_map.h"
#include "industry.h"
#include "town.h"
#include "town_map.h"
#include "house.h"
#include "cargotype.h"
#include "company_base.h"
#include "company_func.h"
#include "economy_func.h"
#include "timer/timer_game_tick.h"
#include "settings_type.h"
#include "window_func.h"

#include <map>
#include <queue>

#include "safeguards.h"

/*
 * Units: power is counted in tenths of a MW. Energy (fuel in a power plant) is counted in tenths of a MW for one
 * balancing step of POWER_BALANCE_TICKS ticks; a month is about POWER_STEPS_PER_MONTH steps.
 */

/** Ticks between two balancing steps. */
static constexpr uint POWER_BALANCE_TICKS = 256;
/** Balancing steps in a month (30 days of 74 ticks), times 100. */
static constexpr uint POWER_STEPS_PER_MONTH_100 = 30 * 74 * 100 / POWER_BALANCE_TICKS;

/** Version of the power lines on the map; changes whenever a power tile or span changes, so the networks are found again. */
uint32_t _power_network_version = 0;

/** What an industry does with energy. */
struct PowerIndustryRole {
	bool plant = false; ///< A fuel power plant rather than a consumer.
	uint capacity = 0; ///< For plants: highest output, in tenths of MW.
	uint idle = 0; ///< For consumers: demand when making nothing, in tenths of MW.
	uint per_100 = 0; ///< For consumers: extra demand for every 100 units of cargo made last month, in tenths of MW.
};

/**
 * Energy in a unit of fuel delivered to a power plant.
 * @param cargo The cargo.
 * @return Energy units (tenths of MW for one step) in one unit of the cargo, or 0 if it is no fuel.
 */
static uint GetPowerFuelYield(CargoType cargo)
{
	if (!IsValidCargoType(cargo)) return 0;
	const CargoSpec *cs = CargoSpec::Get(cargo);
	/* A tonne of coal gives 2 MW for a month; a thousand litres of oil 3 MW. */
	if (cs->label == CT_COAL) return 20 * POWER_STEPS_PER_MONTH_100 / 100;
	if (cs->label == CT_OIL) return 30 * POWER_STEPS_PER_MONTH_100 / 100;
	return 0;
}

/**
 * Find what an industry does with energy.
 * An industry that accepts coal or oil and produces nothing is a fuel power plant; every other industry is a consumer.
 * @param i The industry.
 * @return Its role.
 */
static PowerIndustryRole GetPowerIndustryRole(const Industry *i)
{
	PowerIndustryRole role;
	bool produces = std::ranges::any_of(i->produced, [](const auto &p) { return IsValidCargoType(p.cargo); });
	if (!produces) {
		for (const auto &a : i->accepted) {
			if (!IsValidCargoType(a.cargo)) continue;
			const CargoSpec *cs = CargoSpec::Get(a.cargo);
			if (cs->label == CT_COAL) role.capacity = std::max<uint>(role.capacity, 6000);
			if (cs->label == CT_OIL) role.capacity = std::max<uint>(role.capacity, 4500);
		}
		if (role.capacity != 0) {
			role.plant = true;
			return role;
		}
	}

	const IndustrySpec *indspec = GetIndustrySpec(i->type);
	if (!produces) {
		role.idle = 30;
	} else if (indspec->life_type.Test(IndustryLifeType::Processing)) {
		role.idle = 100;
		role.per_100 = 250;
	} else if (indspec->life_type.Test(IndustryLifeType::Extractive)) {
		role.idle = 80;
		role.per_100 = 180;
	} else {
		role.idle = 20;
		role.per_100 = 40;
	}
	return role;
}

/**
 * Get the extra output of an industry or town from its energy supply.
 * @param demand Its demand.
 * @param supplied What it got.
 * @return The extra share, in 1/256.
 */
static uint GetPowerBoost(uint demand, uint supplied)
{
	if (demand == 0 || supplied == 0) return 0;
	uint share = std::min<uint>(256, supplied * 256 / demand);
	return _settings_game.economy.power_bonus * 256 / 100 * share / 256;
}

/**
 * Get the extra output of an industry or town from its energy supply, for windows.
 * @param demand Its demand.
 * @param supplied What it got.
 * @return The extra output, in percent.
 */
uint GetPowerBonusPercent(uint demand, uint supplied)
{
	return GetPowerBoost(demand, supplied) * 100 / 256;
}

/**
 * Add the energy bonus to an amount of cargo an industry produces.
 * @param i The industry.
 * @param amount The amount without the bonus.
 * @return The amount with the bonus.
 */
uint ApplyIndustryPowerBonus(Industry *i, uint amount)
{
	uint boost = GetPowerBoost(i->power_demand, i->power_supplied);
	if (boost == 0 || amount == 0) return amount;
	uint extra = amount * boost + i->power_carry;
	i->power_carry = extra & 0xFF;
	return amount + (extra >> 8);
}

/**
 * Add the energy bonus to an amount of cargo (passengers, mail) a town produces.
 * @param t The town.
 * @param amount The amount without the bonus.
 * @return The amount with the bonus.
 */
uint ApplyTownPowerBonus(Town *t, uint amount)
{
	uint boost = GetPowerBoost(t->power_demand, t->power_supplied);
	if (boost == 0 || amount == 0) return amount;
	uint extra = amount * boost + t->power_carry;
	t->power_carry = extra & 0xFF;
	return amount + (extra >> 8);
}

/**
 * Shorten the time between two new buildings of a town by its energy bonus.
 * @param t The town.
 * @param ticks Time between two new buildings without the bonus.
 * @return The time with the bonus.
 */
uint ApplyTownPowerGrowthBonus(const Town *t, uint ticks)
{
	uint boost = GetPowerBoost(t->power_demand, t->power_supplied);
	return ticks * 256 / (256 + boost);
}

/**
 * Store fuel delivered to an industry, if it is a fuel power plant.
 * @param i The industry.
 * @param cargo The cargo delivered.
 * @param amount How much of it.
 */
void AddPowerPlantFuel(Industry *i, CargoType cargo, uint amount)
{
	uint yield = GetPowerFuelYield(cargo);
	if (yield == 0 || amount == 0) return;
	PowerIndustryRole role = GetPowerIndustryRole(i);
	if (!role.plant) return;
	/* The store holds two months of full output. */
	uint64_t limit = static_cast<uint64_t>(role.capacity) * POWER_STEPS_PER_MONTH_100 * 2 / 100;
	i->power_fuel = static_cast<uint32_t>(std::min<uint64_t>(limit, i->power_fuel + static_cast<uint64_t>(amount) * yield));
}

/** Whether an industry is a fuel power plant. */
bool IsPowerPlant(const Industry *i)
{
	return GetPowerIndustryRole(i).plant;
}

/**
 * Days of full output left in the fuel store of a power plant.
 * @param i The power plant.
 * @return Days.
 */
uint GetPowerPlantFuelDays(const Industry *i)
{
	PowerIndustryRole role = GetPowerIndustryRole(i);
	if (role.capacity == 0) return 0;
	return static_cast<uint>(static_cast<uint64_t>(i->power_fuel) * POWER_BALANCE_TICKS / role.capacity / 74);
}

/*
 * The networks: every power tile joined by spans. They are found again from the map whenever it changes;
 * nothing of them is saved.
 */

/** A substation and what the balancing knows about it. */
struct PowerSubstation {
	TileIndex tile; ///< Where it is.
	uint network; ///< Which network it belongs to.
};

/** A pair of substations of one network, with the length of line between them. */
struct PowerPair {
	uint32_t distance; ///< Length of the shortest line, overhead 24 per straight tile and 34 per diagonal tile, underground half that.
	uint16_t from; ///< Index into the substations.
	uint16_t to; ///< Index into the substations.
};

static uint32_t _power_cache_version = UINT32_MAX; ///< #_power_network_version the cache below was made for.
static std::vector<PowerSubstation> _power_substations; ///< All substations, by tile index.
static std::vector<std::vector<PowerPair>> _power_pairs; ///< For each network, all pairs of its substations by distance.

/**
 * Get the tile at the other end of a span.
 * @param tile A power tile.
 * @param dir Direction of the span.
 * @param length Length of the span.
 * @return The other end, or INVALID_TILE.
 */
static TileIndex GetPowerSpanEnd(TileIndex tile, Direction dir, uint length)
{
	TileIndexDiffC d = TileIndexDiffCByDir(dir);
	return TileAddWrap(tile, d.x * static_cast<int>(length), d.y * static_cast<int>(length));
}

/** Find the substations, the networks and the distances between substations again. */
static void UpdatePowerNetworks()
{
	_power_cache_version = _power_network_version;
	_power_substations.clear();
	_power_pairs.clear();

	std::vector<TileIndex> nodes;
	for (TileIndex tile : Map::Iterate()) {
		if (IsPowerTile(tile)) nodes.push_back(tile);
	}
	if (nodes.empty()) return;

	auto node_of = [&](TileIndex tile) -> int {
		auto it = std::ranges::lower_bound(nodes, tile);
		return (it != nodes.end() && *it == tile) ? static_cast<int>(it - nodes.begin()) : -1;
	};

	/* Edges, with their weights. */
	std::vector<std::vector<std::pair<uint, uint32_t>>> edges(nodes.size());
	for (uint n = 0; n < nodes.size(); n++) {
		for (Direction dir : EnumRange(Direction::End)) {
			uint length = GetPowerSpan(nodes[n], dir);
			if (length == 0) continue;
			int other = node_of(GetPowerSpanEnd(nodes[n], dir, length));
			if (other < 0) continue;
			/* Lines join only lines of the same owner, until sharing comes. */
			if (GetTileOwner(nodes[other]) != GetTileOwner(nodes[n])) continue;
			bool diagonal = to_underlying(dir) % 2 == 0;
			edges[n].emplace_back(other, length * (diagonal ? 34 : 24));
		}

		/* Underground cables, from an underground substation or a transition pylon to every other one the cables of the
		 * same owner reach; cables join wherever they meet on a tile. They lose half as much as overhead lines. */
		if (!CanAttachPowerCables(nodes[n]) || !HasPowerCable(nodes[n])) continue;
		Owner owner = GetTileOwner(nodes[n]);
		std::map<TileIndex, uint32_t> dist{{nodes[n], 0}};
		using Item = std::pair<uint32_t, TileIndex>;
		std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
		queue.emplace(0, nodes[n]);
		while (!queue.empty()) {
			auto [d, tile] = queue.top();
			queue.pop();
			if (d != dist[tile]) continue;
			if (tile != nodes[n] && CanAttachPowerCables(tile)) {
				/* Another substation or transition pylon: an edge, and the cable network goes on from there by its own edges. */
				int other = node_of(tile);
				if (other >= 0 && GetTileOwner(tile) == owner) edges[n].emplace_back(other, d);
				continue;
			}
			uint8_t dirs = GetPowerCableDirs(tile);
			if (GetPowerCableOwner(tile) != owner) continue;
			for (Direction dir : EnumRange(Direction::End)) {
				if (!HasBit(dirs, to_underlying(dir))) continue;
				TileIndex next = GetPowerSpanEnd(tile, dir, 1);
				if (next == INVALID_TILE || !HasBit(GetPowerCableDirs(next), to_underlying(ReverseDir(dir)))) continue;
				uint32_t nd = d + ((to_underlying(dir) % 2 == 0) ? 17 : 12);
				auto it = dist.find(next);
				if (it != dist.end() && it->second <= nd) continue;
				dist[next] = nd;
				queue.emplace(nd, next);
			}
		}
	}

	/* Networks, by flood fill. */
	std::vector<int> network(nodes.size(), -1);
	uint networks = 0;
	for (uint n = 0; n < nodes.size(); n++) {
		if (network[n] >= 0) continue;
		std::vector<uint> stack{n};
		network[n] = networks;
		while (!stack.empty()) {
			uint cur = stack.back();
			stack.pop_back();
			for (auto [next, weight] : edges[cur]) {
				if (network[next] >= 0) continue;
				network[next] = networks;
				stack.push_back(next);
			}
		}
		networks++;
	}

	std::vector<int> substation_of(nodes.size(), -1);
	for (uint n = 0; n < nodes.size(); n++) {
		if (!IsPowerSubstation(nodes[n])) continue;
		substation_of[n] = static_cast<int>(_power_substations.size());
		_power_substations.push_back({nodes[n], static_cast<uint>(network[n])});
	}
	_power_pairs.resize(networks);

	/* Distances from every substation, along the lines (Dijkstra). */
	for (uint n = 0; n < nodes.size(); n++) {
		if (substation_of[n] < 0) continue;
		std::vector<uint32_t> dist(nodes.size(), UINT32_MAX);
		using Item = std::pair<uint32_t, uint>;
		std::priority_queue<Item, std::vector<Item>, std::greater<Item>> queue;
		dist[n] = 0;
		queue.emplace(0, n);
		while (!queue.empty()) {
			auto [d, cur] = queue.top();
			queue.pop();
			if (d != dist[cur]) continue;
			for (auto [next, weight] : edges[cur]) {
				if (d + weight >= dist[next]) continue;
				dist[next] = d + weight;
				queue.emplace(dist[next], next);
			}
		}
		for (uint m = 0; m < nodes.size(); m++) {
			if (substation_of[m] < 0 || dist[m] == UINT32_MAX) continue;
			_power_pairs[network[n]].push_back({dist[m], static_cast<uint16_t>(substation_of[n]), static_cast<uint16_t>(substation_of[m])});
		}
	}
	for (auto &pairs : _power_pairs) {
		std::ranges::sort(pairs, [](const PowerPair &a, const PowerPair &b) {
			return std::tie(a.distance, a.from, a.to) < std::tie(b.distance, b.from, b.to);
		});
	}
}

/**
 * Get the northern tile of a building, where its demand is counted.
 * @param tile A tile of the building.
 * @return Its northern tile.
 */
static TileIndex GetPowerHouseNorth(TileIndex tile)
{
	HouseID house = GetHouseType(tile);
	return tile + GetHouseNorthPart(house);
}

/** Demand of the buildings of one town covered by a substation. */
struct PowerTownPart {
	TownID town; ///< The town.
	uint demand; ///< Demand, in tenths of MW.
};

/** What a substation covers, and the results of the balancing. */
struct PowerSubstationState {
	std::vector<IndustryID> industries; ///< Industries with a tile in the catchment.
	std::vector<TileIndex> houses; ///< Northern tiles of buildings in the catchment.
	std::vector<PowerTownPart> towns; ///< Demand of the buildings, by town.
	uint supply = 0; ///< Energy offered by the covered plants, in tenths of MW.
	uint demand = 0; ///< Demand of the covered consumers, in tenths of MW.
	uint delivered = 0; ///< Energy that arrived for the consumers.
	uint sold = 0; ///< Energy the plants gave up, including what was lost on the way.
	uint income = 0; ///< Energy that arrived at consumers from the plants of this substation; paid to its owner.
};

/**
 * Demand of a building, in tenths of MW: 0.1 MW idle, 1 MW per 100 inhabitants with up to 10% more while the
 * town grows, and 0.5 MW while it is being built.
 * @param tile Northern tile of the building.
 * @return The demand.
 */
static uint GetHousePowerDemand(TileIndex tile)
{
	if (!IsHouseCompleted(tile)) return 5;
	const HouseSpec *hs = HouseSpec::Get(GetHouseType(tile));
	uint population = hs->population;
	if (Town::GetByTile(tile)->flags.Test(TownFlag::IsGrowing)) population = population * 11 / 10;
	return 1 + population / 10;
}

/**
 * Demand or offer of an industry for this balancing step.
 * @param i The industry.
 * @param role Its role.
 * @return Demand of a consumer, or what a plant can give, in tenths of MW.
 */
static uint GetIndustryPowerAmount(const Industry *i, const PowerIndustryRole &role)
{
	if (role.plant) return std::min<uint>(role.capacity, i->power_fuel);
	uint made = 0;
	for (const auto &p : i->produced) {
		if (IsValidCargoType(p.cargo)) made += p.history[LAST_MONTH].production;
	}
	/* Count what it made without the bonus, so the bonus does not push its own demand up. */
	uint boost = GetPowerBoost(i->power_demand, i->power_supplied);
	made = made * 256 / (256 + boost);
	return std::min<uint>(UINT16_MAX, role.idle + role.per_100 * made / 100);
}

/** One balancing step: share out the energy of all networks, nearest plants first, and pay for what arrives. */
static void BalancePower()
{
	if (_power_cache_version != _power_network_version) UpdatePowerNetworks();

	/* Start from nothing; uncovered consumers still show what they would need. */
	std::vector<PowerIndustryRole> roles(Industry::GetPoolSize());
	std::vector<uint> amount(Industry::GetPoolSize(), 0);
	for (Industry *i : Industry::Iterate()) {
		roles[i->index.base()] = GetPowerIndustryRole(i);
		amount[i->index.base()] = GetIndustryPowerAmount(i, roles[i->index.base()]);
		i->power_demand = static_cast<uint16_t>(std::min<uint>(UINT16_MAX, amount[i->index.base()]));
		i->power_supplied = 0;
	}
	for (Town *t : Town::Iterate()) {
		t->power_demand = t->cache.num_houses + t->cache.population / 10;
		t->power_supplied = 0;
	}
	if (_power_substations.empty()) return;

	/* What each substation covers; consumers and plants covered by several substations are shared evenly between them. */
	uint radius = _settings_game.economy.power_catchment;
	std::vector<PowerSubstationState> state(_power_substations.size());
	std::vector<uint8_t> industry_count(Industry::GetPoolSize(), 0);
	std::map<TileIndex, uint8_t> house_count;
	for (uint s = 0; s < _power_substations.size(); s++) {
		TileIndex centre = _power_substations[s].tile;
		PowerSubstationState &st = state[s];
		for (int dy = -static_cast<int>(radius); dy <= static_cast<int>(radius); dy++) {
			for (int dx = -static_cast<int>(radius); dx <= static_cast<int>(radius); dx++) {
				TileIndex tile = TileAddWrap(centre, dx, dy);
				if (tile == INVALID_TILE) continue;
				if (IsTileType(tile, TileType::Industry)) {
					st.industries.push_back(GetIndustryIndex(tile));
				} else if (IsTileType(tile, TileType::House)) {
					st.houses.push_back(GetPowerHouseNorth(tile));
				}
			}
		}
		std::ranges::sort(st.industries);
		st.industries.erase(std::unique(st.industries.begin(), st.industries.end()), st.industries.end());
		std::ranges::sort(st.houses);
		st.houses.erase(std::unique(st.houses.begin(), st.houses.end()), st.houses.end());
		for (IndustryID id : st.industries) industry_count[id.base()]++;
		for (TileIndex house : st.houses) house_count[house]++;
	}
	for (PowerSubstationState &st : state) {
		for (IndustryID id : st.industries) {
			uint part = amount[id.base()] / industry_count[id.base()];
			if (roles[id.base()].plant) {
				st.supply += part;
			} else {
				st.demand += part;
			}
		}
		for (TileIndex house : st.houses) {
			uint part = GetHousePowerDemand(house) / house_count[house];
			TownID town = GetTownIndex(house);
			auto it = std::ranges::find(st.towns, town, &PowerTownPart::town);
			if (it == st.towns.end()) {
				st.towns.push_back({town, part});
			} else {
				it->demand += part;
			}
			st.demand += part;
		}
	}

	/* Share out, nearest first. Loss: 1% at each substation on the way, and 1% for every 16 tiles of line. */
	std::vector<uint> supply_left(state.size()), need_left(state.size());
	for (uint s = 0; s < state.size(); s++) {
		supply_left[s] = state[s].supply;
		need_left[s] = state[s].demand;
	}
	for (const auto &pairs : _power_pairs) {
		for (const PowerPair &pair : pairs) {
			if (supply_left[pair.from] == 0 || need_left[pair.to] == 0) continue;
			uint loss = std::min<uint>(600, (pair.from == pair.to ? 10 : 20) + pair.distance * 10 / 384);
			uint deliverable = static_cast<uint>(static_cast<uint64_t>(supply_left[pair.from]) * (1000 - loss) / 1000);
			uint delivered = std::min(need_left[pair.to], deliverable);
			if (delivered == 0) continue;
			uint taken = std::min<uint>(supply_left[pair.from], static_cast<uint>((static_cast<uint64_t>(delivered) * 1000 + 999 - loss) / (1000 - loss)));
			supply_left[pair.from] -= taken;
			need_left[pair.to] -= delivered;
			state[pair.to].delivered += delivered;
			state[pair.from].sold += taken;
			state[pair.from].income += delivered;
		}
	}

	/* Tell the consumers and plants what they got. */
	for (PowerSubstationState &st : state) {
		for (IndustryID id : st.industries) {
			Industry *i = Industry::Get(id);
			uint part = amount[id.base()] / industry_count[id.base()];
			if (roles[id.base()].plant) {
				uint sold = st.supply == 0 ? 0 : static_cast<uint>(static_cast<uint64_t>(st.sold) * part / st.supply);
				i->power_supplied = static_cast<uint16_t>(std::min<uint>(UINT16_MAX, i->power_supplied + sold));
				i->power_fuel -= std::min<uint32_t>(i->power_fuel, sold);
			} else if (st.demand != 0) {
				uint got = static_cast<uint>(static_cast<uint64_t>(st.delivered) * part / st.demand);
				i->power_supplied = static_cast<uint16_t>(std::min<uint>(UINT16_MAX, i->power_supplied + got));
			}
		}
		if (st.demand == 0) continue;
		for (const PowerTownPart &part : st.towns) {
			Town::Get(part.town)->power_supplied += static_cast<uint>(static_cast<uint64_t>(st.delivered) * part.demand / st.demand);
		}
	}

	/* Pay the owners of the substations that took the energy from the plants. */
	std::map<Owner, uint64_t> income;
	for (uint s = 0; s < state.size(); s++) {
		if (state[s].income != 0) income[GetTileOwner(_power_substations[s].tile)] += state[s].income;
	}
	for (auto [owner, energy] : income) {
		if (!Company::IsValidID(owner)) continue;
		Money money = _price[Price::PowerSale] * static_cast<int64_t>(energy) / 100;
		if (money == 0) continue;
		SubtractMoneyFromCompany(owner, CommandCost(ExpensesType::EnergySales, -money));
	}
}

/** Called every tick; balances the power networks every #POWER_BALANCE_TICKS ticks. */
void OnTick_Power()
{
	if ((TimerGameTick::counter % POWER_BALANCE_TICKS) != 37) return;
	BalancePower();
	SetWindowClassesDirty(WindowClass::IndustryView);
	SetWindowClassesDirty(WindowClass::TownView);
}

/**
 * Get what a substation covers and does, for the land info window.
 * @param tile The substation.
 * @return Supply of its plants and demand of its consumers, in tenths of MW.
 */
PowerSubstationFigures GetPowerSubstationFigures(TileIndex tile)
{
	PowerSubstationFigures figures;
	uint radius = _settings_game.economy.power_catchment;
	std::vector<IndustryID> industries;
	std::vector<TileIndex> houses;
	for (int dy = -static_cast<int>(radius); dy <= static_cast<int>(radius); dy++) {
		for (int dx = -static_cast<int>(radius); dx <= static_cast<int>(radius); dx++) {
			TileIndex t = TileAddWrap(tile, dx, dy);
			if (t == INVALID_TILE) continue;
			if (IsTileType(t, TileType::Industry)) industries.push_back(GetIndustryIndex(t));
			if (IsTileType(t, TileType::House)) houses.push_back(GetPowerHouseNorth(t));
		}
	}
	std::ranges::sort(industries);
	industries.erase(std::unique(industries.begin(), industries.end()), industries.end());
	std::ranges::sort(houses);
	houses.erase(std::unique(houses.begin(), houses.end()), houses.end());
	for (IndustryID id : industries) {
		const Industry *i = Industry::Get(id);
		if (IsPowerPlant(i)) {
			figures.plants++;
			figures.supply += i->power_demand;
		} else {
			figures.industries++;
			figures.demand += i->power_demand;
		}
	}
	figures.houses = static_cast<uint>(houses.size());
	for (TileIndex house : houses) figures.demand += GetHousePowerDemand(house);
	return figures;
}
