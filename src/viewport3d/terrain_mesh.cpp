/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file terrain_mesh.cpp Building terrain geometry for the 3D viewport from the map array. */

#include "../stdafx.h"
#include "terrain_mesh.h"
#include "ground_textures.h"
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

static constexpr TerrainColour COLOUR_TREES{0x2e, 0x70, 0x26};
static constexpr TerrainColour COLOUR_COAST{0xb8, 0xb0, 0x84};
static constexpr TerrainColour COLOUR_RAIL{0x76, 0x66, 0x54};
static constexpr TerrainColour COLOUR_ROAD{0x58, 0x58, 0x5c};
static constexpr TerrainColour COLOUR_HOUSE{0xb4, 0x64, 0x50};
static constexpr TerrainColour COLOUR_STATION{0xa4, 0xa4, 0xac};
static constexpr TerrainColour COLOUR_INDUSTRY{0x94, 0x74, 0x94};
static constexpr TerrainColour COLOUR_BRIDGE{0x68, 0x68, 0x68};
static constexpr TerrainColour COLOUR_OBJECT{0x84, 0x94, 0x74};

/** How to draw the ground of a tile. */
struct TileGround {
	GroundTexture texture; ///< The texture.
	TerrainColour colour; ///< Colour to multiply the texture with.
};

static constexpr TerrainColour COLOUR_WHITE{0xff, 0xff, 0xff};
/** Tint for the ground under trees, so forests stand out as long as the trees themselves are not drawn. */
static constexpr TerrainColour COLOUR_TREE_TINT{0xdc, 0xec, 0xd4};

/** Get the texture of rough land, which has a few variants that depend on the position of the tile. */
static GroundTexture GetRoughTexture(uint x, uint y)
{
	static constexpr uint8_t VARIANTS[8] = {0, 1, 2, 3, 4, 0, 1, 2};
	return static_cast<GroundTexture>(GROUND_TEXTURE_ROUGH + VARIANTS[GB(TileHash(x * TILE_SIZE, y * TILE_SIZE), 0, 3)]);
}

/**
 * Get the texture and colour to draw the ground of a tile with.
 * Follows what the 2D game draws as ground sprite. Tiles of which the contents cannot be
 * drawn yet, such as tracks and buildings, get a flat colour as stand-in.
 */
static TileGround GetTileGround(Tile tile, uint x, uint y)
{
	auto textured = [](uint texture, const TerrainColour &colour = COLOUR_WHITE) { return TileGround{static_cast<GroundTexture>(texture), colour}; };
	auto plain = [](const TerrainColour &colour) { return TileGround{GROUND_TEXTURE_NONE, colour}; };

	switch (GetTileType(tile)) {
		case TileType::Clear:
			if (IsSnowTile(tile)) {
				return textured((GetClearGround(tile) == ClearGround::Rocks ? GROUND_TEXTURE_SNOWY_ROCKS : GROUND_TEXTURE_SNOW_DESERT) + GetClearDensity(tile));
			}
			switch (GetClearGround(tile)) {
				case ClearGround::Grass: return textured(GROUND_TEXTURE_GRASS + GetClearDensity(tile));
				case ClearGround::Rough: return textured(GetRoughTexture(x, y));
				case ClearGround::Rocks:
					if (GetTropicZone(tile) == TropicZone::Desert) return textured(GROUND_TEXTURE_DESERT_ROCKS + GetClearDensity(tile));
					return textured(GROUND_TEXTURE_ROCKS);
				case ClearGround::Fields: return textured(GROUND_TEXTURE_FIELDS + std::min<uint>(GetFieldType(tile), 8));
				case ClearGround::Desert: return textured(GROUND_TEXTURE_SNOW_DESERT + GetClearDensity(tile));
				default: return textured(GROUND_TEXTURE_GRASS + 3);
			}

		case TileType::Trees:
			switch (GetTreeGround(tile)) {
				case TreeGround::Shore: return plain(Lerp(COLOUR_TREES, COLOUR_COAST, 0.5f));
				case TreeGround::Grass: return textured(GROUND_TEXTURE_GRASS + GetTreeDensity(tile), COLOUR_TREE_TINT);
				case TreeGround::Rough: return textured(GetRoughTexture(x, y), COLOUR_TREE_TINT);
				default: return textured(GROUND_TEXTURE_SNOW_DESERT + GetTreeDensity(tile), COLOUR_TREE_TINT);
			}

		case TileType::Water:
			if (IsCoast(tile)) return plain(COLOUR_COAST);
			return textured(GROUND_TEXTURE_WATER);

		case TileType::Railway: return plain(COLOUR_RAIL);
		case TileType::Road: return plain(COLOUR_ROAD);
		case TileType::House: return plain(COLOUR_HOUSE);
		case TileType::Station: return plain(COLOUR_STATION);
		case TileType::Industry: return plain(COLOUR_INDUSTRY);
		case TileType::TunnelBridge: return plain(COLOUR_BRIDGE);
		case TileType::Object: return plain(COLOUR_OBJECT);
		default: return textured(GROUND_TEXTURE_GRASS + 3);
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

			TileGround ground = GetTileGround(tile, x, y);
			/* A slight checkerboard keeps individual tiles recognisable where there is no texture. */
			if (ground.texture == GROUND_TEXTURE_NONE && ((x ^ y) & 1) != 0) light *= 0.97f;

			/* The light can be brighter than 1; the shader doubles the colour to make up for the halving here. */
			light *= 0.5f;
			uint8_t r = static_cast<uint8_t>(std::clamp(ground.colour.r * light, 0.0f, 255.0f));
			uint8_t g = static_cast<uint8_t>(std::clamp(ground.colour.g * light, 0.0f, 255.0f));
			uint8_t b = static_cast<uint8_t>(std::clamp(ground.colour.b * light, 0.0f, 255.0f));

			auto vertex = [&](uint vx, uint vy, int h) {
				return TerrainVertex{static_cast<float>(vx), static_cast<float>(vy), static_cast<float>(h), r, g, b, ground.texture};
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
