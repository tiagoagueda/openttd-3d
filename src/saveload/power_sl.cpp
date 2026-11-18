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

static const PWRLChunkHandler PWRL;
static const ChunkHandlerRef power_chunk_handlers[] = {
	PWRL,
};

extern const ChunkHandlerTable _power_chunk_handlers(power_chunk_handlers);
