/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file terrain_mesh.cpp Building terrain geometry for the 3D viewport from the map array. */

#include "../stdafx.h"
#include "terrain_mesh.h"
#include "../clear_map.h"
#include "../landscape.h"
#include "../map_func.h"
#include "../settings_type.h"
#include "../tile_map.h"
#include "../tree_map.h"
#include "../water_map.h"

#include "../safeguards.h"

/** Simple RGB colour with floating point channels in the range 0..255. */
struct TerrainColour {
	float r, g, b;
};

static constexpr TerrainColour Lerp(const TerrainColour &a, const TerrainColour &b, float t)
{
	return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

static constexpr TerrainColour COLOUR_GRASS{0x4c, 0x9a, 0x2a};
static constexpr TerrainColour COLOUR_DIRT{0x8a, 0x6b, 0x3d};
static constexpr TerrainColour COLOUR_ROUGH{0x5c, 0x86, 0x32};
static constexpr TerrainColour COLOUR_ROCKS{0x84, 0x82, 0x78};
static constexpr TerrainColour COLOUR_FIELDS{0xb4, 0x9c, 0x44};
static constexpr TerrainColour COLOUR_DESERT{0xd8, 0xc0, 0x74};
static constexpr TerrainColour COLOUR_SNOW{0xee, 0xf0, 0xf6};
static constexpr TerrainColour COLOUR_TREES{0x2e, 0x70, 0x26};
static constexpr TerrainColour COLOUR_SEA{0x2a, 0x5c, 0xb0};
static constexpr TerrainColour COLOUR_RIVER{0x3c, 0x78, 0xc8};
static constexpr TerrainColour COLOUR_COAST{0xb8, 0xb0, 0x84};
static constexpr TerrainColour COLOUR_RAIL{0x76, 0x66, 0x54};
static constexpr TerrainColour COLOUR_ROAD{0x58, 0x58, 0x5c};
static constexpr TerrainColour COLOUR_HOUSE{0xb4, 0x64, 0x50};
static constexpr TerrainColour COLOUR_STATION{0xa4, 0xa4, 0xac};
static constexpr TerrainColour COLOUR_INDUSTRY{0x94, 0x74, 0x94};
static constexpr TerrainColour COLOUR_BRIDGE{0x68, 0x68, 0x68};
static constexpr TerrainColour COLOUR_OBJECT{0x84, 0x94, 0x74};

/**
 * Get the unlit colour to draw a tile with.
 * This is a stand-in until tiles get real ground textures and models.
 */
static TerrainColour GetTileColour(Tile tile)
{
	switch (GetTileType(tile)) {
		case TileType::Clear:
			if (IsSnowTile(tile)) return Lerp(COLOUR_ROCKS, COLOUR_SNOW, (GetClearDensity(tile) + 1) / 4.0f);
			switch (GetClearGround(tile)) {
				case ClearGround::Grass: return Lerp(COLOUR_DIRT, COLOUR_GRASS, GetClearDensity(tile) / 3.0f);
				case ClearGround::Rough: return COLOUR_ROUGH;
				case ClearGround::Rocks: return COLOUR_ROCKS;
				case ClearGround::Fields: return Lerp(COLOUR_FIELDS, COLOUR_GRASS, (GetFieldType(tile) % 4) * 0.12f);
				case ClearGround::Desert: return Lerp(COLOUR_GRASS, COLOUR_DESERT, (GetClearDensity(tile) + 1) / 4.0f);
				default: return COLOUR_GRASS;
			}

		case TileType::Trees:
			switch (GetTreeGround(tile)) {
				case TreeGround::SnowOrDesert:
				case TreeGround::RoughSnow:
					return Lerp(COLOUR_TREES, _settings_game.game_creation.landscape == LandscapeType::Tropic ? COLOUR_DESERT : COLOUR_SNOW, 0.6f);
				case TreeGround::Shore: return Lerp(COLOUR_TREES, COLOUR_COAST, 0.5f);
				default: return COLOUR_TREES;
			}

		case TileType::Water:
			if (IsCoast(tile)) return COLOUR_COAST;
			if (IsWater(tile) && !IsSea(tile)) return COLOUR_RIVER;
			return COLOUR_SEA;

		case TileType::Railway: return COLOUR_RAIL;
		case TileType::Road: return COLOUR_ROAD;
		case TileType::House: return COLOUR_HOUSE;
		case TileType::Station: return COLOUR_STATION;
		case TileType::Industry: return COLOUR_INDUSTRY;
		case TileType::TunnelBridge: return COLOUR_BRIDGE;
		case TileType::Object: return COLOUR_OBJECT;
		default: return COLOUR_GRASS;
	}
}

/**
 * Build the geometry of one chunk of terrain from the current state of the map.
 * @param chunk_x X coordinate of the chunk, in chunks.
 * @param chunk_y Y coordinate of the chunk, in chunks.
 * @param[out] mesh The mesh to fill; previous contents are discarded.
 */
void BuildTerrainChunkMesh(uint chunk_x, uint chunk_y, TerrainChunkMesh &mesh)
{
	/* Direction towards the light. It comes from the east, like in the original graphics. */
	static constexpr float SUN_X = -0.4683f, SUN_Y = 0.5620f, SUN_Z = 0.6817f;
	static constexpr float AMBIENT = 0.45f;
	static constexpr float DIFFUSE = 0.80f;
	/* Change in world height per world unit of distance, for one height level difference across a tile. */
	static constexpr float SLOPE_SCALE = static_cast<float>(TILE_HEIGHT) / TILE_SIZE;

	mesh.vertices.clear();
	mesh.vertices.reserve(TERRAIN_CHUNK_SIZE * TERRAIN_CHUNK_SIZE * 4);
	mesh.min_height = UINT_MAX;
	mesh.max_height = 0;

	/* The last row and column of the map only provide the heights for the far corners. */
	uint x_end = std::min(chunk_x * TERRAIN_CHUNK_SIZE + TERRAIN_CHUNK_SIZE, Map::MaxX());
	uint y_end = std::min(chunk_y * TERRAIN_CHUNK_SIZE + TERRAIN_CHUNK_SIZE, Map::MaxY());

	for (uint y = chunk_y * TERRAIN_CHUNK_SIZE; y < y_end; y++) {
		for (uint x = chunk_x * TERRAIN_CHUNK_SIZE; x < x_end; x++) {
			Tile tile(TileXY(x, y));
			if (IsTileType(tile, TileType::Void)) continue;

			/* Heights of the four corners; the map stores the height of the northern corner of each tile. */
			int hn = TileHeight(tile);
			int hw = TileHeight(TileXY(x + 1, y));
			int he = TileHeight(TileXY(x, y + 1));
			int hs = TileHeight(TileXY(x + 1, y + 1));

			mesh.min_height = std::min<uint>(mesh.min_height, std::min({hn, hw, he, hs}));
			mesh.max_height = std::max<uint>(mesh.max_height, std::max({hn, hw, he, hs}));

			/* Light the tile as one flat surface, using the average slope in both directions. */
			float nx = -((hw + hs) - (hn + he)) * 0.5f * SLOPE_SCALE;
			float ny = -((he + hs) - (hn + hw)) * 0.5f * SLOPE_SCALE;
			float light = (nx * SUN_X + ny * SUN_Y + SUN_Z) / std::sqrt(nx * nx + ny * ny + 1.0f);
			light = AMBIENT + DIFFUSE * std::max(0.0f, light);
			/* A slight checkerboard keeps individual tiles recognisable on flat land. */
			if (((x ^ y) & 1) != 0) light *= 0.97f;

			TerrainColour c = GetTileColour(tile);
			uint8_t r = static_cast<uint8_t>(std::clamp(c.r * light, 0.0f, 255.0f));
			uint8_t g = static_cast<uint8_t>(std::clamp(c.g * light, 0.0f, 255.0f));
			uint8_t b = static_cast<uint8_t>(std::clamp(c.b * light, 0.0f, 255.0f));

			auto vertex = [&](uint vx, uint vy, int h) {
				return TerrainVertex{static_cast<int16_t>(vx), static_cast<int16_t>(vy), static_cast<int16_t>(h), 1, r, g, b, 255};
			};
			TerrainVertex vn = vertex(x, y, hn);
			TerrainVertex vw = vertex(x + 1, y, hw);
			TerrainVertex ve = vertex(x, y + 1, he);
			TerrainVertex vs = vertex(x + 1, y + 1, hs);

			/* Split the tile along the diagonal with the smallest height difference,
			 * so a single raised or lowered corner gives one flat and one sloped half. */
			if (abs(hn - hs) <= abs(hw - he)) {
				mesh.vertices.insert(mesh.vertices.end(), {vw, vn, vs, ve});
			} else {
				mesh.vertices.insert(mesh.vertices.end(), {vn, ve, vw, vs});
			}
		}
	}

	if (mesh.min_height > mesh.max_height) mesh.min_height = mesh.max_height;

	/* FNV-1a hash, to cheaply see whether the chunk looks different from last time. */
	uint64_t hash = 0xcbf29ce484222325ULL;
	const uint8_t *data = reinterpret_cast<const uint8_t *>(mesh.vertices.data());
	for (size_t i = 0; i < mesh.vertices.size() * sizeof(TerrainVertex); i++) {
		hash = (hash ^ data[i]) * 0x100000001b3ULL;
	}
	mesh.hash = hash;
}
