/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file power_sl.cpp Code handling saving and loading of the power layer (overhead wires and underground cables). */

#include "../stdafx.h"

#include "saveload.h"
#include "saveload_error.hpp"

#include "../map_func.h"
#include "../power_func.h"
#include "../power_map.h"
#include "../industry.h"
#include "../town.h"

#include "../safeguards.h"

/** Number of tiles handled at once. */
static constexpr size_t POWER_SL_BUF_SIZE = 4096;

/**
 * The power layer: 16 bits for every tile.
 * Savegames without this chunk (all savegames from before power lines) load with an empty layer,
 * because the layer is cleared whenever the map is allocated.
 */
struct PWRLChunkHandler : ChunkHandler {
	PWRLChunkHandler() : ChunkHandler("PWRL", ChunkType::Riff) {}

	void Load() const override
	{
		size_t size = Map::Size();
		if (SlGetFieldLength() != size * sizeof(uint16_t)) SlErrorCorrupt("Power layer size does not match the map");

		for (size_t i = 0; i < size; i += POWER_SL_BUF_SIZE) {
			std::span<uint16_t> part(_power_layer.get() + i, std::min(POWER_SL_BUF_SIZE, size - i));
			SlCopy<VarFileType::U16>(part);
		}
	}

	void Save() const override
	{
		size_t size = Map::Size();

		SlSetLength(size * sizeof(uint16_t));
		for (size_t i = 0; i < size; i += POWER_SL_BUF_SIZE) {
			std::span<uint16_t> part(_power_layer.get() + i, std::min(POWER_SL_BUF_SIZE, size - i));
			SlCopy<VarFileType::U16>(part);
		}
	}
};

/**
 * The energy state of industries and towns: the fuel in power plants, and what each got in the last balancing
 * step, which sets their bonus until the next one. Stored as plain numbers, an industry or town in each record.
 * Savegames without this chunk load with no fuel and no power anywhere.
 */
struct PWREChunkHandler : ChunkHandler {
	PWREChunkHandler() : ChunkHandler("PWRE", ChunkType::Riff) {}

	/** Fields of one record: the index, fuel (0 for towns), demand, supplied and carry. */
	static constexpr size_t FIELDS = 5;

	void Load() const override
	{
		size_t length = SlGetFieldLength();
		if (length % (FIELDS * sizeof(uint32_t)) != 0) SlErrorCorrupt("Invalid size of the energy chunk");
		std::vector<uint32_t> data(length / sizeof(uint32_t));
		std::span<uint32_t> all{data};
		SlCopy<VarFileType::U32>(all);

		/* The first record holds only the number of industry records. */
		uint32_t industries = data.empty() ? 0 : data[0];
		size_t pos = FIELDS;
		for (uint32_t n = 0; n < industries && pos + FIELDS <= data.size(); n++, pos += FIELDS) {
			Industry *i = Industry::GetIfValid(data[pos]);
			if (i == nullptr) continue;
			i->power_fuel = data[pos + 1];
			i->power_demand = static_cast<uint16_t>(data[pos + 2]);
			i->power_supplied = static_cast<uint16_t>(data[pos + 3]);
			i->power_carry = static_cast<uint16_t>(data[pos + 4]);
		}
		for (; pos + FIELDS <= data.size(); pos += FIELDS) {
			Town *t = Town::GetIfValid(data[pos]);
			if (t == nullptr) continue;
			t->power_demand = data[pos + 2];
			t->power_supplied = data[pos + 3];
			t->power_carry = static_cast<uint16_t>(data[pos + 4]);
		}
	}

	void Save() const override
	{
		std::vector<uint32_t> data(FIELDS, 0);
		data[0] = static_cast<uint32_t>(Industry::GetNumItems());
		for (const Industry *i : Industry::Iterate()) {
			data.insert(data.end(), {i->index.base(), i->power_fuel, i->power_demand, i->power_supplied, i->power_carry});
		}
		for (const Town *t : Town::Iterate()) {
			data.insert(data.end(), {t->index.base(), 0, t->power_demand, t->power_supplied, t->power_carry});
		}

		SlSetLength(data.size() * sizeof(uint32_t));
		std::span<uint32_t> all{data};
		SlCopy<VarFileType::U32>(all);
	}
};

static const PWRLChunkHandler PWRL;
static const PWREChunkHandler PWRE;
static const ChunkHandlerRef power_chunk_handlers[] = {
	PWRL,
	PWRE,
};

extern const ChunkHandlerTable _power_chunk_handlers(power_chunk_handlers);
