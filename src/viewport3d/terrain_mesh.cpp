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
#include "sprite_atlas.h"
#include "tile_sprites.h"
#include "../bridge_map.h"
#include "../clear_map.h"
#include "../landscape.h"
#include "../map_func.h"
#include "../settings_type.h"
#include "../slope_func.h"
#include "../tile_map.h"
#include "../tree_map.h"
#include "../water_map.h"
#include "../zoom_type.h"

#include "../table/sprites.h"
#include "../table/clear_land.h"
#include "../table/tree_land.h"

#include "../safeguards.h"

/** Simple RGB colour with floating point channels in the range 0..255. */
struct TerrainColour {
	float r, g, b;
};

static constexpr TerrainColour Lerp(const TerrainColour &a, const TerrainColour &b, float t)
{
	return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}


/** How to draw the ground of a tile. */
struct TileGround {
	uint texture; ///< The layer of the texture.
	TerrainColour colour; ///< Colour to multiply the texture with.
	bool shaded = false; ///< Whether the texture already has the shading of the slope in it.
};

static constexpr TerrainColour COLOUR_WHITE{0xff, 0xff, 0xff};

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
	/* The textures of coasts are made from the shore sprites, of which there is one per slope. Same table as in DrawShoreTile(). */
	static constexpr SlopeIndexArray<uint8_t> SLOPE_TO_SHORE = {
		0, 1, 2, 3, 4, 16, 6, 7, 8, 9, 17, 11, 12, 13, 14, 0,
		0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0,  5,  0, 10, 15,
	};
	auto shore = [&]() {
		Slope slope = GetTileSlope(TileXY(x, y));
		if (slope == SLOPE_FLAT || IsHalftileSlope(slope)) return TileGround{GROUND_TEXTURE_WATER, COLOUR_WHITE};
		return TileGround{static_cast<uint>(GROUND_TEXTURE_SHORE + SLOPE_TO_SHORE[slope]), COLOUR_WHITE, true};
	};

	auto textured = [](uint texture, const TerrainColour &colour = COLOUR_WHITE) { return TileGround{texture, colour}; };

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
				case TreeGround::Shore: return shore();
				case TreeGround::Grass: return textured(GROUND_TEXTURE_GRASS + GetTreeDensity(tile));
				case TreeGround::Rough: return textured(GetRoughTexture(x, y));
				default: return textured(GROUND_TEXTURE_SNOW_DESERT + GetTreeDensity(tile));
			}

		case TileType::Water:
			if (IsCoast(tile)) return shore();
			return textured(GROUND_TEXTURE_WATER);

		default: return textured(GROUND_TEXTURE_GRASS + 3);
	}
}

/**
 * Add a sprite that stands upright on the map and always faces the camera.
 * @param sprite The sprite.
 * @param x Position along the x axis in world units.
 * @param y Position along the y axis in world units.
 * @param z Height in the z units of the game.
 * @param nearer How much nearer to the camera the sprite counts as being when deciding what is in front.
 *               A sprite that stands in the middle of what it shows needs this, or the ground would hide its lower part.
 * @param mesh The mesh to add to.
 */
static void AddUprightSprite(const AtlasSprite &sprite, float x, float y, float z, float nearer, TerrainChunkMesh &mesh)
{
	if (!sprite.valid || mesh.sprites.size() >= MAX_CHUNK_QUADS * 4) return;

	x /= TILE_SIZE;
	y /= TILE_SIZE;
	z /= TILE_HEIGHT;
	float left = sprite.left;
	float right = sprite.left + sprite.width;
	float top = -sprite.top;
	float bottom = -(sprite.top + sprite.height);
	mesh.sprites.insert(mesh.sprites.end(), {
		{x, y, z, left, top, sprite.u0, sprite.v0, nearer},
		{x, y, z, right, top, sprite.u1, sprite.v0, nearer},
		{x, y, z, left, bottom, sprite.u0, sprite.v1, nearer},
		{x, y, z, right, bottom, sprite.u1, sprite.v1, nearer},
	});
}

/**
 * Add the trees of a tile, the same ones at the same places as DrawTile_Trees() draws.
 * @param tile The tile, which must have trees.
 * @param x X coordinate of the tile.
 * @param y Y coordinate of the tile.
 * @param atlas The atlas that provides the sprites.
 * @param mesh The mesh to add to.
 */
static void AddTrees(Tile tile, uint x, uint y, SpriteAtlas &atlas, TerrainChunkMesh &mesh)
{
	uint tmp = CountBits(TileXY(x, y).base() + x * TILE_SIZE + y * TILE_SIZE);
	uint index = GB(tmp, 0, 2) + (GetTreeType(tile) << 2);

	/* Different tree styles above the snow line. */
	if ((GetTreeGround(tile) == TreeGround::SnowOrDesert || GetTreeGround(tile) == TreeGround::RoughSnow) &&
			GetTreeDensity(tile) >= 2 &&
			IsInsideMM(index, TREE_SUB_ARCTIC << 2, TREE_RAINFOREST << 2)) {
		index += 164 - (TREE_SUB_ARCTIC << 2);
	}
	if (index >= std::size(_tree_layout_sprite)) return;

	const PalSpriteID *sprites = _tree_layout_sprite[index];
	const Coord2D<uint8_t> *positions = _tree_layout_xy[GB(tmp, 2, 2)];
	auto [slope, base_z] = GetTilePixelSlope(TileXY(x, y));

	uint trees = GetTreeCount(tile);
	for (uint i = 0; i < trees; i++) {
		/* Only the last tree of a tile is still growing or dying. */
		SpriteID sprite = sprites[i].sprite + to_underlying(i == trees - 1 ? GetTreeGrowth(tile) : TreeGrowthStage::Grown);
		int z = base_z + GetPartialPixelZ(positions[i].x, positions[i].y, slope);
		AddUprightSprite(atlas.Get(sprite, sprites[i].pal), static_cast<float>(x * TILE_SIZE + positions[i].x), static_cast<float>(y * TILE_SIZE + positions[i].y), static_cast<float>(z), 0, mesh);
	}
}

/**
 * Add a sprite that stands upright along a line on the map, like a fence along the edge of a tile.
 * The sprite is laid on a wall from the ground between two points upwards, such that from the
 * viewing angle of the 2D game it looks exactly like the 2D game draws it.
 * All positions are in world units, with heights in the z units of the game.
 * @param sprite The sprite.
 * @param anchor The position the 2D game draws the sprite at.
 * @param a One end of the line, on the ground.
 * @param b Other end of the line, on the ground.
 * @param mesh The mesh to add to.
 */
static void AddWallSprite(const AtlasSprite &sprite, const std::array<float, 3> &anchor, const std::array<float, 3> &a, const std::array<float, 3> &b, TerrainChunkMesh &mesh)
{
	if (!sprite.valid || mesh.sprites.size() >= MAX_CHUNK_QUADS * 4) return;

	/* Where the 2D game draws a point, in pixels relative to the anchor. */
	auto project = [&](const std::array<float, 3> &p, float &px, float &py) {
		float dx = p[0] - anchor[0];
		float dy = p[1] - anchor[1];
		px = (dy - dx) * 2;
		py = dx + dy - (p[2] - anchor[2]);
	};
	float ax, ay, bx, by;
	project(a, ax, ay);
	project(b, bx, by);

	/* Make the wall as high as the sprite reaches above the line. */
	float height = std::min(ay, by) - sprite.top;
	if (height <= 0) return;

	auto vertex = [&](const std::array<float, 3> &p, float px, float py, float up) {
		float u = std::clamp((px - sprite.left) / sprite.width, 0.0f, 1.0f);
		float v = std::clamp((py - up - sprite.top) / sprite.height, 0.0f, 1.0f);
		return SpriteVertex{p[0] / TILE_SIZE, p[1] / TILE_SIZE, (p[2] + up) / TILE_HEIGHT, 0, 0, sprite.u0 + (sprite.u1 - sprite.u0) * u, sprite.v0 + (sprite.v1 - sprite.v0) * v, 0};
	};
	mesh.sprites.insert(mesh.sprites.end(), {vertex(a, ax, ay, height), vertex(b, bx, by, height), vertex(a, ax, ay, 0), vertex(b, bx, by, 0)});
}

/**
 * Add the hedges and fences around a farm field, the same ones as DrawClearLandFence() draws.
 * @param tile The tile, which must be a farm field.
 * @param x X coordinate of the tile.
 * @param y Y coordinate of the tile.
 * @param atlas The atlas that provides the sprites.
 * @param mesh The mesh to add to.
 */
static void AddFences(Tile tile, uint x, uint y, SpriteAtlas &atlas, TerrainChunkMesh &mesh)
{
	auto [slope, base_z] = GetTilePixelSlope(TileXY(x, y));
	if (IsHalftileSlope(slope)) return;

	auto corner = [&](uint dx, uint dy) {
		return std::array<float, 3>{static_cast<float>((x + dx) * TILE_SIZE), static_cast<float>((y + dy) * TILE_SIZE), static_cast<float>(TileHeight(TileXY(x + dx, y + dy)) * TILE_HEIGHT)};
	};
	const std::array<float, 3> north = corner(0, 0), west = corner(1, 0), east = corner(0, 1), south = corner(1, 1);
	const std::array<float, 3> anchor{north[0], north[1], static_cast<float>(base_z)};

	auto fence = [&](DiagDirection side, const SlopeIndexArray<uint8_t> &slope_offsets, const std::array<float, 3> &a, const std::array<float, 3> &b) {
		uint type = GetFence(tile, side);
		if (type == 0 || type > std::size(_clear_land_fence_sprites)) return;
		SpriteID sprite = _clear_land_fence_sprites[type - 1];
		if (sprite == 0) return;
		AddWallSprite(atlas.Get(sprite + slope_offsets[slope], PAL_NONE), anchor, a, b, mesh);
	};
	fence(DiagDirection::NW, _fence_mod_by_tileh_nw, north, west);
	fence(DiagDirection::NE, _fence_mod_by_tileh_ne, north, east);
	fence(DiagDirection::SW, _fence_mod_by_tileh_sw, west, south);
	fence(DiagDirection::SE, _fence_mod_by_tileh_se, east, south);
}

/**
 * Add what the 2D game draws for a tile that has more on it than landscape, such as tracks, roads and buildings.
 * Sprites that the 2D game draws flat on the ground become the ground texture of the tile.
 * Everything else becomes sprites that stand upright at the front of their bounding box.
 * @param x X coordinate of the tile.
 * @param y Y coordinate of the tile.
 * @param heights Heights of the northern, western, eastern and southern corner, in height levels above the lowest.
 * @param textures The ground textures.
 * @param atlas The atlas that provides the sprites.
 * @param mesh The mesh to add to.
 * @return How to draw the ground of the tile.
 */
static TileGround AddTileSprites(uint x, uint y, const std::array<uint8_t, 4> &heights, GroundTextures &textures, SpriteAtlas &atlas, TerrainChunkMesh &mesh)
{
	static TileSprites sprites;
	static std::vector<SpritePart> group_parts;

	TileIndex tile = TileXY(x, y);
	CollectTileSprites(tile, sprites);

	int tile_x = x * TILE_SIZE;
	int tile_y = y * TILE_SIZE;
	int tile_z = std::get<1>(GetTilePixelSlope(tile));

	uint index = 0;
	for (const TileSpriteGroup &group : sprites.groups) {
		if (group.count == 0) continue;

		/* The sprite stands in the middle of its bounding box, so it covers the place it stands on from all sides. */
		int size_x = group.xmax + 1 - group.xmin;
		int size_y = group.ymax + 1 - group.ymin;
		float centre_x = group.xmin + size_x * 0.5f;
		float centre_y = group.ymin + size_y * 0.5f;
		/* Where the 2D game draws that point, relative to the reference point of the collected sprites. */
		int screen_x = static_cast<int>(((centre_y - tile_y) - (centre_x - tile_x)) * 2 * ZOOM_BASE);
		int screen_y = static_cast<int>(((centre_x - tile_x) + (centre_y - tile_y) - (group.zmin - tile_z)) * ZOOM_BASE);

		group_parts.assign(sprites.parts.begin() + group.first, sprites.parts.begin() + group.first + group.count);
		for (SpritePart &part : group_parts) {
			part.x -= screen_x;
			part.y -= screen_y;
		}

		/* The front half of the bounding box is drawn below the middle. That part of the sprite would end up in the ground,
		 * so the sprite counts as that much nearer. A little extra per sprite puts later sprites of a tile in front of
		 * earlier ones at the same place, like in the 2D game. */
		float nearer = (size_x + size_y) * 0.5f + 0.5f * index++;
		AddUprightSprite(atlas.Get(group_parts), centre_x, centre_y, static_cast<float>(group.zmin), nearer, mesh);
	}

	if (sprites.ground.empty()) return TileGround{GROUND_TEXTURE_GRASS, COLOUR_WHITE};
	uint texture = textures.Get(sprites.ground, heights);
	if (texture == GROUND_TEXTURE_NONE) return TileGround{GROUND_TEXTURE_NONE, TerrainColour{0x80, 0x80, 0x80}};
	return TileGround{texture, COLOUR_WHITE, true};
}

/**
 * Build the geometry of one chunk of terrain from the current state of the map.
 * @param chunk_x X coordinate of the chunk, in chunks.
 * @param chunk_y Y coordinate of the chunk, in chunks.
 * @param textures The ground textures.
 * @param atlas The atlas that provides the sprites.
 * @param[out] mesh The mesh to fill; previous contents are discarded.
 */
void BuildTerrainChunkMesh(uint chunk_x, uint chunk_y, GroundTextures &textures, SpriteAtlas &atlas, TerrainChunkMesh &mesh)
{
	/* Direction towards the light. It comes from the east, like in the original graphics. */
	static constexpr float SUN_X = -0.4683f, SUN_Y = 0.5620f, SUN_Z = 0.6817f;
	static constexpr float AMBIENT = 0.45f;
	static constexpr float DIFFUSE = 0.80f;
	/* Change in world height per world unit of distance, for one height level difference across a tile. */
	static constexpr float SLOPE_SCALE = static_cast<float>(TILE_HEIGHT) / TILE_SIZE;

	mesh.vertices.clear();
	mesh.sprites.clear();
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

			/* Tiles with more on them than landscape say themselves what the 2D game draws for them. */
			bool collect = IsBridgeAbove(tile);
			switch (GetTileType(tile)) {
				case TileType::Clear:
				case TileType::Trees:
					break;
				case TileType::Water:
					if (!IsWater(tile) && !IsCoast(tile)) collect = true;
					break;
				default:
					collect = true;
					break;
			}

			TileGround ground;
			if (collect) {
				int low = std::min({hn, hw, he, hs});
				ground = AddTileSprites(x, y, {static_cast<uint8_t>(hn - low), static_cast<uint8_t>(hw - low), static_cast<uint8_t>(he - low), static_cast<uint8_t>(hs - low)}, textures, atlas, mesh);
			} else {
				ground = GetTileGround(tile, x, y);
			}
			if (ground.shaded) light = AMBIENT + DIFFUSE * SUN_Z;
			/* A slight checkerboard keeps individual tiles recognisable where there is no texture. */
			if (ground.texture == GROUND_TEXTURE_NONE && ((x ^ y) & 1) != 0) light *= 0.97f;

			/* The light can be brighter than 1; the shader doubles the colour to make up for the halving here. */
			light *= 0.5f;
			uint8_t r = static_cast<uint8_t>(std::clamp(ground.colour.r * light, 0.0f, 255.0f));
			uint8_t g = static_cast<uint8_t>(std::clamp(ground.colour.g * light, 0.0f, 255.0f));
			uint8_t b = static_cast<uint8_t>(std::clamp(ground.colour.b * light, 0.0f, 255.0f));

			auto vertex = [&](uint vx, uint vy, int h) {
				return TerrainVertex{static_cast<float>(vx), static_cast<float>(vy), static_cast<float>(h), r, g, b, 0, static_cast<float>(ground.texture)};
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

			if (!collect) {
				if (IsTileType(tile, TileType::Trees)) AddTrees(tile, x, y, atlas, mesh);
				if (IsTileType(tile, TileType::Clear) && IsClearGround(tile, ClearGround::Fields)) AddFences(tile, x, y, atlas, mesh);
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
	data = reinterpret_cast<const uint8_t *>(mesh.sprites.data());
	for (size_t i = 0; i < mesh.sprites.size() * sizeof(SpriteVertex); i++) {
		hash = (hash ^ data[i]) * 0x100000001b3ULL;
	}
	mesh.hash = hash;
}
