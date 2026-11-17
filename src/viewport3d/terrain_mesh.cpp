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

#include "model_pack.h"
#include "../debug.h"
#include "../company_func.h"
#include "../house.h"
#include "../industry.h"
#include "../industry_map.h"
#include "../newgrf_object.h"
#include "../newgrf_roadstop.h"
#include "../newgrf_station.h"
#include "../object_base.h"
#include "../object_map.h"
#include "../rail_map.h"
#include "../rail.h"
#include "../newgrf_railtype.h"
#include "../elrail_func.h"
#include "../tunnelbridge_map.h"
#include "../road_map.h"
#include "../road_func.h"
#include "view3d_gui.h"
#include "../newgrf_roadtype.h"
#include "../station_map.h"
#include "../town.h"
#include "../town_map.h"
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
	bool foundation = false; ///< Whether the tile stands on a foundation, so its top is flat at the height of its highest corner, or inclined.
	Slope foundation_slope = SLOPE_FLAT; ///< With a foundation: the slope of its top; inclined foundations lie on the highest corners, the others one lower.
	uint8_t under = 0; ///< For a mixed texture: the layer of the texture of the model package under it.
};

static constexpr TerrainColour COLOUR_WHITE{0xff, 0xff, 0xff};

/** Get the texture of rough land, which has a few variants that depend on the position of the tile. */
static GroundTexture GetRoughTexture(uint x, uint y)
{
	static constexpr uint8_t VARIANTS[8] = {0, 1, 2, 3, 4, 0, 1, 2};
	return static_cast<GroundTexture>(GROUND_TEXTURE_ROUGH + VARIANTS[GB(TileHash(x * TILE_SIZE, y * TILE_SIZE), 0, 3)]);
}

/** How far below the water the corners of a coast that lie in the sea are drawn, in height levels. */
static constexpr float SEABED_DEPTH = 0.75f;

/**
 * Whether a tile slopes into the sea: a coast, or a river that runs down into it.
 * With ground textures from the model package such a tile is drawn as ground that goes down
 * below the water, with see-through water over it, and not with the shore sprites.
 */
static bool IsShoreSlope(Tile tile, uint x, uint y)
{
	if (IsTileType(tile, TileType::Water)) {
		if (!IsCoast(tile) && !IsWater(tile)) return false;
	} else if (IsTileType(tile, TileType::Trees)) {
		if (GetTreeGround(tile) != TreeGround::Shore) return false;
	} else {
		return false;
	}
	auto [slope, z] = GetTileSlopeZ(TileXY(x, y));
	return slope != SLOPE_FLAT && z == 0;
}

/**
 * Whether there is sea on a tile, right up to its edges at sea level: flat water, whatever stands in it, or a coast.
 * Outside the map counts as sea.
 */
static bool IsSeaTile(uint x, uint y)
{
	if (x >= Map::SizeX() || y >= Map::SizeY()) return true;
	Tile tile(TileXY(x, y));
	if (IsTileType(tile, TileType::Void)) return true;
	if (IsShoreSlope(tile, x, y)) return true;
	if (!HasTileWaterClass(tile) || !IsTileOnWater(tile)) return false;
	auto [slope, z] = GetTileSlopeZ(TileXY(x, y));
	return slope == SLOPE_FLAT && z == 0;
}

/**
 * Whether a corner of the map lies in the sea, with nothing but sea around it.
 * Coasts go down below the water at these corners, so that the edge of the water crosses the tile.
 * @param x, y The tile of which it is the northern corner.
 */
static bool IsSeaCorner(uint x, uint y)
{
	if (x < Map::SizeX() && y < Map::SizeY() && TileHeight(TileXY(x, y)) != 0) return false;
	return IsSeaTile(x - 1, y - 1) && IsSeaTile(x, y - 1) && IsSeaTile(x - 1, y) && IsSeaTile(x, y);
}

/**
 * Get the texture and colour to draw the ground of a tile with.
 * Follows what the 2D game draws as ground sprite. Tiles of which the contents cannot be
 * drawn yet, such as tracks and buildings, get a flat colour as stand-in.
 */
static TileGround GetTileGround(Tile tile, uint x, uint y, bool beach)
{
	/* The textures of coasts are made from the shore sprites, of which there is one per slope. Same table as in DrawShoreTile(). */
	static constexpr SlopeIndexArray<uint8_t> SLOPE_TO_SHORE = {
		0, 1, 2, 3, 4, 16, 6, 7, 8, 9, 17, 11, 12, 13, 14, 0,
		0, 0, 0, 0, 0,  0, 0, 0, 0, 0,  0,  5,  0, 10, 15,
	};
	auto shore = [&]() {
		Slope slope = GetTileSlope(TileXY(x, y));
		if (slope == SLOPE_FLAT || IsHalftileSlope(slope)) return TileGround{GROUND_TEXTURE_WATER, COLOUR_WHITE};
		if (beach) return TileGround{GROUND_TEXTURE_BEACH, COLOUR_WHITE};
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
 * @param vertices The vertices to add to.
 */
void AddUprightSprite(const AtlasSprite &sprite, float x, float y, float z, float nearer, std::vector<SpriteVertex> &vertices)
{
	if (!sprite.valid || vertices.size() >= MAX_CHUNK_QUADS * 4) return;

	x /= TILE_SIZE;
	y /= TILE_SIZE;
	z /= TILE_HEIGHT;
	float left = sprite.left;
	float right = sprite.left + sprite.width;
	float top = -sprite.top;
	float bottom = -(sprite.top + sprite.height);
	vertices.insert(vertices.end(), {
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
		AddUprightSprite(atlas.Get(sprite, sprites[i].pal), static_cast<float>(x * TILE_SIZE + positions[i].x), static_cast<float>(y * TILE_SIZE + positions[i].y), static_cast<float>(z), 0, mesh.sprites);
	}
}

/**
 * Add the trees of a tile as 3D models, at the places where DrawTile_Trees() draws their sprites.
 * @param tile The tile, which must have trees.
 * @param x X coordinate of the tile.
 * @param y Y coordinate of the tile.
 * @param pack The package of 3D models.
 * @param mesh The mesh to add to.
 * @return Whether the package has a model for the trees of this tile; if not, nothing was added.
 */
static bool AddTreeModels(Tile tile, uint x, uint y, const ModelPack &pack, TerrainChunkMesh &mesh)
{
	uint type = GetTreeType(tile);
	/* Sub-arctic trees above the snow line look different. */
	if ((GetTreeGround(tile) == TreeGround::SnowOrDesert || GetTreeGround(tile) == TreeGround::RoughSnow) &&
			GetTreeDensity(tile) >= 2 && IsInsideMM(type, TREE_SUB_ARCTIC, TREE_RAINFOREST)) {
		type += 0x40;
	}
	const ModelPack::Entry *entry = pack.Find(ModelKind::Tree, 0, type);
	if (entry == nullptr) return false;

	/* How big a tree is while it grows and dies, by growth stage. */
	static constexpr float GROWTH_SCALE[] = {0.35f, 0.55f, 0.78f, 1.0f, 0.97f, 0.93f, 0.9f};

	uint tmp = CountBits(TileXY(x, y).base() + x * TILE_SIZE + y * TILE_SIZE);
	const Coord2D<uint8_t> *positions = _tree_layout_xy[GB(tmp, 2, 2)];
	auto [slope, base_z] = GetTilePixelSlope(TileXY(x, y));

	uint trees = GetTreeCount(tile);
	for (uint i = 0; i < trees; i++) {
		/* Only the last tree of a tile is still growing or dying. */
		uint stage = to_underlying(i == trees - 1 ? GetTreeGrowth(tile) : TreeGrowthStage::Grown);
		/* Each tree turns its own way and differs a little in size. */
		uint32_t hash = (TileXY(x, y).base() * 4 + i) * 2654435761U;
		float yaw = (hash >> 8 & 0xFFF) / 4096.0f * 6.2831853f;
		float s = entry->scale * GROWTH_SCALE[std::min<uint>(stage, std::size(GROWTH_SCALE) - 1)] * (0.88f + (hash >> 24) / 255.0f * 0.24f);
		float c = std::cos(yaw) * s;
		float n = std::sin(yaw) * s;
		int z = base_z + GetPartialPixelZ(positions[i].x, positions[i].y, slope);

		ModelInstance &instance = mesh.models.emplace_back();
		instance.model = entry->model;
		instance.transform.m = {
			c, n, 0, 0,
			-n, c, 0, 0,
			0, 0, s, 0,
			static_cast<float>(x * TILE_SIZE + positions[i].x), static_cast<float>(y * TILE_SIZE + positions[i].y), z * WORLD_Z_SCALE, 1,
		};
		instance.tint = {1, 1, 1, 1};
	}
	return true;
}

/**
 * Add a 3D model that lies on the ground of a whole tile, like the crop of a field, tilted along the slope of the tile.
 * @param entry The model.
 * @param x X coordinate of the tile.
 * @param y Y coordinate of the tile.
 * @param heights Heights of the northern, western, eastern and southern corner, in height levels.
 * @param quarter_turns How many quarter turns to turn the model by.
 * @param mesh The mesh to add to.
 */
static void AddGroundModel(const ModelPack::Entry &entry, uint x, uint y, const std::array<int, 4> &heights, uint quarter_turns, TerrainChunkMesh &mesh)
{
	auto [hn, hw, he, hs] = heights;
	/* The plane that fits the corners best: its height in the middle, and how much it rises along x and y per world unit. */
	float zc = (hn + hw + he + hs) * 0.25f * HEIGHT_LEVEL_SIZE;
	float ax = ((hw + hs) - (hn + he)) * 0.5f * HEIGHT_LEVEL_SIZE / TILE_SIZE;
	float ay = ((he + hs) - (hn + hw)) * 0.5f * HEIGHT_LEVEL_SIZE / TILE_SIZE;
	static constexpr float TURNS[4][2] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};
	float c = TURNS[quarter_turns % 4][0] * entry.scale;
	float n = TURNS[quarter_turns % 4][1] * entry.scale;

	ModelInstance &instance = mesh.models.emplace_back();
	instance.model = entry.model;
	instance.transform.m = {
		c, n, ax * c + ay * n, 0,
		-n, c, -ax * n + ay * c, 0,
		0, 0, entry.scale, 0,
		(x + 0.5f) * TILE_SIZE, (y + 0.5f) * TILE_SIZE, zc, 1,
	};
	instance.tint = {1, 1, 1, 1};
}

/** Middle and direction of each track on a tile, in the order of enum Track: X, Y, upper, lower, left, right. */
static constexpr float TRACK_PIECES[6][4] = {
	{TILE_SIZE * 0.5f, TILE_SIZE * 0.5f, 1, 0}, {TILE_SIZE * 0.5f, TILE_SIZE * 0.5f, 0, 1},
	{TILE_SIZE * 0.25f, TILE_SIZE * 0.25f, 0.70710678f, -0.70710678f}, {TILE_SIZE * 0.75f, TILE_SIZE * 0.75f, 0.70710678f, -0.70710678f},
	{TILE_SIZE * 0.75f, TILE_SIZE * 0.25f, 0.70710678f, 0.70710678f}, {TILE_SIZE * 0.25f, TILE_SIZE * 0.75f, 0.70710678f, 0.70710678f},
};

/**
 * Add a model that lies along a line on the ground of a tile, like a piece of track, tilted along the slope of the tile.
 * @param entry The model; it runs along its x axis.
 * @param x X coordinate of the tile.
 * @param y Y coordinate of the tile.
 * @param heights Heights of the northern, western, eastern and southern corner, in height levels.
 * @param cx Where the middle of the model goes, in world units within the tile along x.
 * @param cy Where the middle of the model goes, in world units within the tile along y.
 * @param dx Direction of the model along x, of length one together with dy.
 * @param dy Direction of the model along y.
 * @param mesh The mesh to add to.
 */
static void AddPieceModel(const ModelPack::Entry &entry, uint x, uint y, const std::array<int, 4> &heights, float cx, float cy, float dx, float dy, TerrainChunkMesh &mesh)
{
	auto [hn, hw, he, hs] = heights;
	/* The plane that fits the corners best, as in AddGroundModel(). */
	float zc = (hn + hw + he + hs) * 0.25f * HEIGHT_LEVEL_SIZE;
	float ax = ((hw + hs) - (hn + he)) * 0.5f * HEIGHT_LEVEL_SIZE / TILE_SIZE;
	float ay = ((he + hs) - (hn + hw)) * 0.5f * HEIGHT_LEVEL_SIZE / TILE_SIZE;
	float s = entry.scale;
	float z = zc + ax * (cx - TILE_SIZE * 0.5f) + ay * (cy - TILE_SIZE * 0.5f);

	ModelInstance &instance = mesh.models.emplace_back();
	instance.model = entry.model;
	instance.transform.m = {
		dx * s, dy * s, (ax * dx + ay * dy) * s, 0,
		-dy * s, dx * s, (-ax * dy + ay * dx) * s, 0,
		0, 0, s, 0,
		x * TILE_SIZE + cx, y * TILE_SIZE + cy, z, 1,
	};
	instance.tint = {1, 1, 1, 1};
}

/**
 * Get the 3D model of a piece of track.
 * @param rt The rail type.
 * @param diagonal Whether to get the diagonal piece, rather than the straight one.
 * @param pack The package of 3D models.
 * @return The model, or \c nullptr if there is none for this kind of track.
 */
static const ModelPack::Entry *GetTrackPiece(RailType rt, bool diagonal, const ModelPack &pack)
{
	const RailTypeInfo *rti = GetRailTypeInfo(rt);
	uint base;
	if (rti->label == RAILTYPE_LABEL_RAIL || rti->label == RAILTYPE_LABEL_ELECTRIC) {
		base = 0;
	} else if (rti->label == RAILTYPE_LABEL_MONO) {
		base = 2;
	} else if (rti->label == RAILTYPE_LABEL_MAGLEV) {
		base = 4;
	} else {
		return nullptr;
	}
	return pack.Find(ModelKind::Track, 0, base + (diagonal ? 1 : 0));
}

/**
 * Get the 3D model of the short piece of track where straight track meets diagonal track.
 * @param rt The rail type.
 * @param pack The package of 3D models.
 * @return The model, or \c nullptr if there is none for this kind of track.
 */
static const ModelPack::Entry *GetTrackJoint(RailType rt, const ModelPack &pack)
{
	const ModelPack::Entry *straight = GetTrackPiece(rt, false, pack);
	if (straight == nullptr) return nullptr;
	const RailTypeInfo *rti = GetRailTypeInfo(rt);
	uint kind = rti->label == RAILTYPE_LABEL_MONO ? 1 : (rti->label == RAILTYPE_LABEL_MAGLEV ? 2 : 0);
	return pack.Find(ModelKind::Track, 0, 6 + kind);
}

/**
 * Add 3D track on a tile: one piece for each track.
 * @param tile The tile, a plain rail tile.
 * @param x X coordinate of the tile.
 * @param y Y coordinate of the tile.
 * @param heights Heights of the corners, as for AddPieceModel().
 * @param pack The package of 3D models.
 * @param mesh The mesh to add to.
 * @return Whether the package has track of this kind; if not, nothing was added.
 */
static bool AddTrackModels(Tile tile, uint x, uint y, const std::array<int, 4> &heights, const ModelPack &pack, TerrainChunkMesh &mesh)
{
	const ModelPack::Entry *straight = GetTrackPiece(GetRailType(tile), false, pack);
	const ModelPack::Entry *diagonal = GetTrackPiece(GetRailType(tile), true, pack);
	if (straight == nullptr || diagonal == nullptr) return false;

	TrackBits bits = GetTrackBits(tile);
	for (uint track = 0; track < 6; track++) {
		if (!bits.Test(static_cast<Track>(track))) continue;
		const float *p = TRACK_PIECES[track];
		AddPieceModel(track < 2 ? *straight : *diagonal, x, y, heights, p[0], p[1], p[2], p[3], mesh);
	}

	/* Where straight track leaves the tile into diagonal track on the next tile, the rails of the two pieces miss each
	 * other at the outside of the bend; a joint that bends with them closes the gap. */
	const ModelPack::Entry *joint = GetTrackJoint(GetRailType(tile), pack);
	if (joint == nullptr) return true;
	/* For each side of a tile, in the order of DiagDirection (NE, SE, SW, NW): the middle of that side, which way
	 * is out of the tile there, and the diagonal tracks that end there. */
	struct Side { float x, y, ox, oy; Track a, b; };
	static constexpr float H = TILE_SIZE * 0.5f;
	static constexpr Side SIDES[4] = {
		{0, H, -1, 0, Track::Upper, Track::Right}, {H, TILE_SIZE, 0, 1, Track::Lower, Track::Right},
		{TILE_SIZE, H, 1, 0, Track::Lower, Track::Left}, {H, 0, 0, -1, Track::Upper, Track::Left},
	};
	for (DiagDirection dir : {DiagDirection::NE, DiagDirection::SE, DiagDirection::SW, DiagDirection::NW}) {
		/* Straight track along x ends at the north-east and south-west sides, along y at the other two. */
		Track along = DiagDirToAxis(dir) == Axis::X ? Track::X : Track::Y;
		if (!bits.Test(along)) continue;
		TileIndex next = TileAddByDiagDir(tile, dir);
		if (next == INVALID_TILE || !IsPlainRailTile(next)) continue;
		const Side &here = SIDES[to_underlying(dir)];
		const Side &there = SIDES[to_underlying(ReverseDiagDir(dir))];
		TrackBits next_bits = GetTrackBits(next);
		for (Track track : {there.a, there.b}) {
			if (!next_bits.Test(track)) continue;
			/* Into the next tile along its diagonal track: from the side towards the middle of the piece. */
			const float *p = TRACK_PIECES[to_underlying(track)];
			float ix = p[0] - there.x, iy = p[1] - there.y;
			float il = std::sqrt(ix * ix + iy * iy);
			float bx = here.ox + ix / il, by = here.oy + iy / il;
			float bl = std::sqrt(bx * bx + by * by);
			/* The joint turns left; for a turn to the right it lies the other way around. */
			if (here.ox * iy - here.oy * ix < 0) bl = -bl;
			AddPieceModel(*joint, x, y, heights, here.x, here.y, bx / bl, by / bl, mesh);
		}
	}
	return true;
}

/**
 * Whether the overhead wires of a rail tile are drawn with 3D models.
 * @param tile The tile, a plain rail tile.
 * @param pack The package of 3D models.
 * @return True if the tile has wires, the package has models for them and no NewGRF draws them.
 */
static bool HasCatenaryModels(Tile tile, const ModelPack &pack)
{
	RailType rt = GetRailType(tile);
	if (!HasRailCatenaryDrawn(rt)) return false;
	if (GetCustomRailSprite(GetRailTypeInfo(rt), tile, RailSpriteType::Wires) != 0) return false;
	if (GetCustomRailSprite(GetRailTypeInfo(rt), tile, RailSpriteType::Pylons) != 0) return false;
	for (uint id = 0; id < 4; id++) {
		if (pack.Find(ModelKind::Catenary, 0, id) == nullptr) return false;
	}
	return true;
}

/**
 * Add 3D overhead wires above the tracks of a rail tile; the pylons come from AddTileSprites().
 * @param tile The tile, a plain rail tile.
 * @param x X coordinate of the tile.
 * @param y Y coordinate of the tile.
 * @param heights Heights of the corners, as for AddPieceModel().
 * @param pack The package of 3D models.
 * @param mesh The mesh to add to.
 */
static void AddWireModels(Tile tile, uint x, uint y, const std::array<int, 4> &heights, const ModelPack &pack, TerrainChunkMesh &mesh)
{
	if (!HasCatenaryModels(tile, pack)) return;
	const ModelPack::Entry *straight = pack.Find(ModelKind::Catenary, 0, 2);
	const ModelPack::Entry *diagonal = pack.Find(ModelKind::Catenary, 0, 3);
	TrackBits bits = GetTrackBits(tile);
	for (uint track = 0; track < 6; track++) {
		if (!bits.Test(static_cast<Track>(track))) continue;
		const float *p = TRACK_PIECES[track];
		AddPieceModel(track < 2 ? *straight : *diagonal, x, y, heights, p[0], p[1], p[2], p[3], mesh);
	}
}

/**
 * Add a 3D pylon where the 2D game draws a pylon sprite.
 * @param group The sprite group of the pylon.
 * @param offset Which pylon sprite it is, from SPR_PYLON_BASE.
 * @param pack The package of 3D models.
 * @param mesh The mesh to add to.
 */
static void AddPylonModel(const TileSpriteGroup &group, uint offset, const ModelPack &pack, TerrainChunkMesh &mesh)
{
	/* The direction from the pylon to the track, by pylon sprite (PylonSpriteOffset). */
	static constexpr float D = 0.70710678f;
	static constexpr float ARM[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {D, D}, {-D, -D}, {-D, D}, {D, -D}};
	const ModelPack::Entry *entry = pack.Find(ModelKind::Catenary, 0, offset < 4 ? 0 : 1);
	if (entry == nullptr) return;
	auto [dx, dy] = ARM[offset];
	float s = entry->scale;
	ModelInstance &instance = mesh.models.emplace_back();
	instance.model = entry->model;
	instance.transform.m = {
		dx * s, dy * s, 0, 0,
		-dy * s, dx * s, 0, 0,
		0, 0, s, 0,
		group.xmin + 1.0f, group.ymin + 1.0f, group.zmin * WORLD_Z_SCALE, 1,
	};
	instance.tint = {1, 1, 1, 1};
}

static void AddEdgeModel(const ModelPack::Entry &entry, const std::array<float, 3> &a, const std::array<float, 3> &b, TerrainChunkMesh &mesh);

/**
 * Get the road and tram bits of a tile that have overhead wires drawn with 3D models.
 * @param tile The tile, with road or tram on it.
 * @param pack The package of 3D models.
 * @return The bits; none when there are no wires, the package has no models for them or a NewGRF draws them.
 */
static RoadBits GetRoadWireBits(Tile tile, const ModelPack &pack)
{
	if (pack.Find(ModelKind::Catenary, 0, 4) == nullptr || pack.Find(ModelKind::Catenary, 0, 5) == nullptr) return {};
	RoadBits bits{};
	for (RoadTramType rtt : {RoadTramType::Road, RoadTramType::Tram}) {
		RoadType rt = GetRoadType(tile, rtt);
		if (rt == INVALID_ROADTYPE || !HasRoadCatenaryDrawn(rt)) continue;
		const RoadTypeInfo *rti = GetRoadTypeInfo(rt);
		if (GetCustomRoadSprite(rti, tile, RoadSpriteType::CatenaryFront) != 0 || GetCustomRoadSprite(rti, tile, RoadSpriteType::CatenaryRear) != 0) return {};
		bits |= IsNormalRoad(tile) ? GetRoadBits(tile, rtt) : RoadBits{};
	}
	return bits;
}

/**
 * Whether a sprite is one of the overhead wires or poles of trams in the base graphics.
 * @param sprite The sprite.
 * @return True for wires and poles of flat and sloped roads.
 */
static bool IsRoadWireSprite(SpriteID sprite)
{
	return (sprite >= SPR_TRAMWAY_BACK_WIRES_STRAIGHT && sprite < SPR_TRAMWAY_FRONT_WIRES_SLOPED + 4) ||
			(sprite >= SPR_TRAMWAY_BASE + 0x54 && sprite <= SPR_TRAMWAY_BASE + 0x5E);
}

/**
 * Add 3D overhead wires above a road or tram tile, with a pole beside straight roads.
 * @param bits The road bits with wires, from GetRoadWireBits().
 * @param x X coordinate of the tile.
 * @param y Y coordinate of the tile.
 * @param heights Heights of the northern, western, eastern and southern corner, in height levels.
 * @param pack The package of 3D models.
 * @param mesh The mesh to add to.
 */
static void AddRoadWireModels(RoadBits bits, uint x, uint y, const std::array<int, 4> &heights, const ModelPack &pack, TerrainChunkMesh &mesh)
{
	const ModelPack::Entry *wires = pack.Find(ModelKind::Catenary, 0, 4);
	const ModelPack::Entry *pole = pack.Find(ModelKind::Catenary, 0, 5);
	static constexpr std::pair<RoadBit, std::array<float, 2>> BITS[4] = {
		{RoadBit::NW, {0, -1}}, {RoadBit::SW, {1, 0}}, {RoadBit::SE, {0, 1}}, {RoadBit::NE, {-1, 0}},
	};
	for (const auto &[bit, dir] : BITS) {
		if (bits.Test(bit)) AddPieceModel(*wires, x, y, heights, TILE_SIZE * 0.5f, TILE_SIZE * 0.5f, dir[0], dir[1], mesh);
	}

	/* A pole on one side of straight roads, every other tile, reaching over the middle of the road. */
	bool along_x = bits == RoadBits{RoadBit::NE, RoadBit::SW};
	bool along_y = bits == RoadBits{RoadBit::NW, RoadBit::SE};
	if (!along_x && !along_y) return;
	if (((along_x ? x : y) & 1) != 0) return;
	static constexpr float REACH = 6.4f;
	float px = along_x ? TILE_SIZE * 0.5f : TILE_SIZE * 0.5f + REACH;
	float py = along_x ? TILE_SIZE * 0.5f - REACH : TILE_SIZE * 0.5f;
	float dx = along_x ? 0.0f : -1.0f, dy = along_x ? 1.0f : 0.0f;
	auto [hn, hw, he, hs] = heights;
	float zc = (hn + hw + he + hs) * 0.25f * HEIGHT_LEVEL_SIZE;
	float ax = ((hw + hs) - (hn + he)) * 0.5f * HEIGHT_LEVEL_SIZE / TILE_SIZE;
	float ay = ((he + hs) - (hn + hw)) * 0.5f * HEIGHT_LEVEL_SIZE / TILE_SIZE;
	float z = zc + ax * (px - TILE_SIZE * 0.5f) + ay * (py - TILE_SIZE * 0.5f);
	float s = pole->scale;
	ModelInstance &instance = mesh.models.emplace_back();
	instance.model = pole->model;
	instance.transform.m = {dx * s, dy * s, 0, 0, -dy * s, dx * s, 0, 0, 0, 0, s, 0, x * TILE_SIZE + px, y * TILE_SIZE + py, z, 1};
	instance.tint = {1, 1, 1, 1};
}

/**
 * Add 3D pavements and tram rails on a road tile.
 * @param tile The tile, a normal road tile.
 * @param x X coordinate of the tile.
 * @param y Y coordinate of the tile.
 * @param heights Heights of the northern, western, eastern and southern corner, in height levels.
 * @param pack The package of 3D models.
 * @param mesh The mesh to add to.
 */
static void AddRoadModels(Tile tile, uint x, uint y, const std::array<int, 4> &heights, const ModelPack &pack, TerrainChunkMesh &mesh)
{
	enum : uint { N, W, E, S };
	static constexpr float CORNERS[4][2] = {{0, 0}, {TILE_SIZE, 0}, {0, TILE_SIZE}, {TILE_SIZE, TILE_SIZE}};
	RoadBits road = GetRoadBits(tile, RoadTramType::Road);
	RoadBits tram = GetRoadBits(tile, RoadTramType::Tram);
	RoadBits all = road | tram;
	float tx = x * TILE_SIZE, ty = y * TILE_SIZE;
	auto corner = [&](uint c) { return std::array<float, 3>{tx + CORNERS[c][0], ty + CORNERS[c][1], heights[c] * HEIGHT_LEVEL_SIZE}; };

	Roadside roadside = GetRoadside(tile);
	bool paved = roadside == Roadside::Paved || roadside == Roadside::StreetLights || roadside == Roadside::Trees || roadside == Roadside::PavedRoadWorks;
	const ModelPack::Entry *strip = pack.Find(ModelKind::Roadside, 0, 2);
	const ModelPack::Entry *square = pack.Find(ModelKind::Roadside, 0, 3);
	if (paved && road.Any() && strip != nullptr && square != nullptr) {
		static constexpr float HALF = 1.3f;
		/* Each side of the tile: the road bit that leads there, its corners in the order that has the inside of the
		 * tile on the left, and the direction towards the inside. */
		struct Side { RoadBit bit; uint a, b; float ix, iy; };
		static constexpr Side SIDES[4] = {
			{RoadBit::NW, N, W, 0, 1}, {RoadBit::NE, E, N, 1, 0}, {RoadBit::SW, W, S, -1, 0}, {RoadBit::SE, S, E, 0, -1},
		};
		for (const Side &side : SIDES) {
			if (all.Test(side.bit)) continue;
			std::array<float, 3> a = corner(side.a), b = corner(side.b);
			a[0] += side.ix * HALF; a[1] += side.iy * HALF;
			b[0] += side.ix * HALF; b[1] += side.iy * HALF;
			AddEdgeModel(*strip, a, b, mesh);
		}
		/* Corners between two sides that both lead on along the road. */
		struct Corner2 { uint c; RoadBit one, two; float ix, iy; };
		static constexpr Corner2 CORNER_SQUARES[4] = {
			{N, RoadBit::NW, RoadBit::NE, 1, 1}, {W, RoadBit::NW, RoadBit::SW, -1, 1},
			{E, RoadBit::NE, RoadBit::SE, 1, -1}, {S, RoadBit::SW, RoadBit::SE, -1, -1},
		};
		for (const Corner2 &c : CORNER_SQUARES) {
			if (!all.Test(c.one) || !all.Test(c.two)) continue;
			std::array<float, 3> p = corner(c.c);
			float s = square->scale;
			ModelInstance &instance = mesh.models.emplace_back();
			instance.model = square->model;
			instance.transform.m = {s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, p[0] + c.ix * HALF, p[1] + c.iy * HALF, p[2], 1};
			instance.tint = {1, 1, 1, 1};
		}
	}

	const ModelPack::Entry *rails = pack.Find(ModelKind::Roadside, 0, 4);
	if (tram.Any() && rails != nullptr) {
		/* From the middle of the tile towards the edge each tram bit leads to. */
		static constexpr std::pair<RoadBit, std::array<float, 2>> BITS[4] = {
			{RoadBit::NW, {0, -1}}, {RoadBit::SW, {1, 0}}, {RoadBit::SE, {0, 1}}, {RoadBit::NE, {-1, 0}},
		};
		for (const auto &[bit, dir] : BITS) {
			if (tram.Test(bit)) AddPieceModel(*rails, x, y, heights, TILE_SIZE * 0.5f, TILE_SIZE * 0.5f, dir[0], dir[1], mesh);
		}
	}
}

/**
 * Add a 3D street lamp or roadside tree where the 2D game draws its sprite.
 * @param group The sprite group.
 * @param tree Whether it is a tree, rather than a lamp.
 * @param pack The package of 3D models.
 * @param mesh The mesh to add to.
 * @return Whether the package has a model for it.
 */
static bool AddRoadDetailModel(const TileSpriteGroup &group, bool tree, const ModelPack &pack, TerrainChunkMesh &mesh)
{
	const ModelPack::Entry *entry = pack.Find(ModelKind::Roadside, 0, tree ? 1 : 0);
	if (entry == nullptr) return false;
	float px = group.xmin + 1.0f, py = group.ymin + 1.0f;
	/* Lamps reach towards the middle of the tile. */
	float dx = 1, dy = 0;
	int fx = (group.xmin & 0xF) + 1 - 8, fy = (group.ymin & 0xF) + 1 - 8;
	if (std::abs(fx) >= std::abs(fy)) {
		dx = fx > 0 ? -1.0f : 1.0f;
		dy = 0;
	} else {
		dx = 0;
		dy = fy > 0 ? -1.0f : 1.0f;
	}
	float s = entry->scale;
	ModelInstance &instance = mesh.models.emplace_back();
	instance.model = entry->model;
	instance.transform.m = {dx * s, dy * s, 0, 0, -dy * s, dx * s, 0, 0, 0, 0, s, 0, px, py, group.zmin * WORLD_Z_SCALE, 1};
	instance.tint = {1, 1, 1, 1};
	return true;
}

/**
 * Add the 3D model of the landscape of a clear tile: the crop of a field, rough land or rocks.
 * @param tile The tile, which must be a clear tile.
 * @param x X coordinate of the tile.
 * @param y Y coordinate of the tile.
 * @param heights Heights of the northern, western, eastern and southern corner, in height levels.
 * @param pack The package of 3D models.
 * @param mesh The mesh to add to.
 */
static void AddClearModel(Tile tile, uint x, uint y, const std::array<int, 4> &heights, const ModelPack &pack, TerrainChunkMesh &mesh)
{
	if (IsSnowTile(tile)) return;
	uint hash = TileHash2Bit(x, y);
	const ModelPack::Entry *entry = nullptr;
	uint turns = 0;
	switch (GetClearGround(tile)) {
		case ClearGround::Fields:
			entry = pack.Find(ModelKind::Field, 0, GetFieldType(tile));
			break;
		case ClearGround::Rough:
			entry = pack.Find(ModelKind::Rough, 0, hash);
			if (entry == nullptr) entry = pack.Find(ModelKind::Rough, 0, 0);
			turns = TileHash2Bit(y, x);
			break;
		case ClearGround::Rocks:
			entry = pack.Find(ModelKind::Rocks, 0, hash);
			if (entry == nullptr) entry = pack.Find(ModelKind::Rocks, 0, 0);
			turns = TileHash2Bit(y, x);
			break;
		default:
			break;
	}
	if (entry != nullptr) AddGroundModel(*entry, x, y, heights, turns, mesh);
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
static void AddEdgeModel(const ModelPack::Entry &entry, const std::array<float, 3> &a, const std::array<float, 3> &b, TerrainChunkMesh &mesh)
{
	/* The model runs along x over 16 world units; stretch and shear it to run from a to b, and keep it upright. */
	float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
	float length = std::sqrt(dx * dx + dy * dy);
	if (length == 0) return;
	float ux = dx / length, uy = dy / length;
	float s = entry.scale;
	ModelInstance &instance = mesh.models.emplace_back();
	instance.model = entry.model;
	instance.transform.m = {
		dx / TILE_SIZE, dy / TILE_SIZE, dz / TILE_SIZE, 0,
		-uy * s, ux * s, 0, 0,
		0, 0, s, 0,
		(a[0] + b[0]) * 0.5f, (a[1] + b[1]) * 0.5f, (a[2] + b[2]) * 0.5f, 1,
	};
	instance.tint = {1, 1, 1, 1};
}

/**
 * Add 3D fences of a tile along one edge, or along the middle of the tile, a little inside the tile.
 * @param entry The model of the fence.
 * @param x X coordinate of the tile.
 * @param y Y coordinate of the tile.
 * @param heights Heights of the northern, western, eastern and southern corner, in height levels.
 * @param from First corner of the line: 0 north, 1 west, 2 east, 3 south.
 * @param to Second corner of the line.
 * @param mesh The mesh to add to.
 */
static void AddFenceLine(const ModelPack::Entry &entry, uint x, uint y, const std::array<int, 4> &heights, uint from, uint to, TerrainChunkMesh &mesh)
{
	static constexpr float CORNERS[4][2] = {{0, 0}, {TILE_SIZE, 0}, {0, TILE_SIZE}, {TILE_SIZE, TILE_SIZE}};
	static constexpr float INSET = 0.6f;
	std::array<float, 3> a{x * TILE_SIZE + CORNERS[from][0], y * TILE_SIZE + CORNERS[from][1], heights[from] * HEIGHT_LEVEL_SIZE};
	std::array<float, 3> b{x * TILE_SIZE + CORNERS[to][0], y * TILE_SIZE + CORNERS[to][1], heights[to] * HEIGHT_LEVEL_SIZE};
	/* Move edges a little towards the middle of the tile, so fences of neighbouring tiles do not meet. */
	float mx = (x + 0.5f) * TILE_SIZE, my = (y + 0.5f) * TILE_SIZE;
	float ex = (a[0] + b[0]) * 0.5f - mx, ey = (a[1] + b[1]) * 0.5f - my;
	float d = std::sqrt(ex * ex + ey * ey);
	if (d > 1) {
		for (auto *p : {&a, &b}) {
			(*p)[0] -= ex / d * INSET;
			(*p)[1] -= ey / d * INSET;
		}
		/* Reach as far past the corners, so the fence meets the fence of a neighbouring tile that turns the corner. */
		float lx = b[0] - a[0], ly = b[1] - a[1];
		float l = std::sqrt(lx * lx + ly * ly);
		a[0] -= lx / l * INSET;
		a[1] -= ly / l * INSET;
		b[0] += lx / l * INSET;
		b[1] += ly / l * INSET;
	}
	AddEdgeModel(entry, a, b, mesh);
}

/**
 * Add 3D fences along a rail tile, where the 2D game draws its fences.
 * @param tile The tile, a plain rail tile.
 * @param x X coordinate of the tile.
 * @param y Y coordinate of the tile.
 * @param heights Heights of the corners, as for AddFenceLine().
 * @param pack The package of 3D models.
 * @param mesh The mesh to add to.
 */
static void AddRailFenceModels(Tile tile, uint x, uint y, const std::array<int, 4> &heights, const ModelPack &pack, TerrainChunkMesh &mesh)
{
	const ModelPack::Entry *entry = pack.Find(ModelKind::Fence, 0, 0);
	if (entry == nullptr) return;
	if (GetCustomRailSprite(GetRailTypeInfo(GetRailType(tile)), tile, RailSpriteType::Fences) != 0) return;

	enum : uint { N, W, E, S };
	auto line = [&](uint from, uint to) { AddFenceLine(*entry, x, y, heights, from, to, mesh); };
	switch (GetRailGroundType(tile)) {
		case RailGroundType::FenceNW: line(N, W); break;
		case RailGroundType::FenceSE: line(E, S); break;
		case RailGroundType::FenceSENW: line(N, W); line(E, S); break;
		case RailGroundType::FenceNE: line(N, E); break;
		case RailGroundType::FenceSW: line(W, S); break;
		case RailGroundType::FenceNESW: line(N, E); line(W, S); break;
		case RailGroundType::FenceVert1:
		case RailGroundType::FenceVert2: line(N, S); break;
		case RailGroundType::FenceHoriz1:
		case RailGroundType::FenceHoriz2: line(W, E); break;
		case RailGroundType::HalfTileWater: {
			Slope slope = GetTileSlope(tile);
			Corner corner = IsHalftileSlope(slope) ? GetHalftileSlopeCorner(slope) : OppositeCorner(GetHighestSlopeCorner(ComplementSlope(slope)));
			if (corner == Corner::W || corner == Corner::E) {
				line(N, S);
			} else {
				line(W, E);
			}
			break;
		}
		default: break;
	}
}

/**
 * Whether a sprite is one of the fences that the 2D game draws along rail tiles.
 * @param sprite The sprite.
 * @return True for the default rail fences.
 */
static bool IsRailFenceSprite(SpriteID sprite)
{
	sprite &= SPRITE_MASK;
	return sprite >= SPR_TRACK_FENCE_FLAT_X && sprite < SPR_TRACK_FENCE_FLAT_X + 8;
}

static void AddFences(Tile tile, uint x, uint y, SpriteAtlas &atlas, const ModelPack *pack, TerrainChunkMesh &mesh)
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
		if (pack != nullptr && !pack->IsEmpty()) {
			if (const ModelPack::Entry *entry = pack->Find(ModelKind::Fence, 0, type); entry != nullptr) {
				AddEdgeModel(*entry, {a[0], a[1], a[2] * WORLD_Z_SCALE}, {b[0], b[1], b[2] * WORLD_Z_SCALE}, mesh);
				return;
			}
		}
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
 * Place a 3D model on the map.
 * @param entry The model.
 * @param tile The northern tile of what it stands on.
 * @param size_x Size of what it stands on along the x axis, in tiles.
 * @param size_y Size of what it stands on along the y axis, in tiles.
 * @param front The direction the front of the model points to.
 * @param tint The colour of the parts of the model named "company".
 * @param models The models to add to.
 */
static std::vector<AnimatedPart> *_tile_parts = nullptr; ///< Where PlaceModel() puts the moving parts of models, if anywhere.

/**
 * Place a moving part at its place for some moment.
 * @param seconds The moment, in seconds since some start.
 * @return The part.
 */
ModelInstance AnimatedPart::Place(float seconds) const
{
	float t = seconds + this->phase;
	float angle = this->spin * t;
	if (this->swing != 0) angle += this->swing * std::sin(t * 6.2831853f / this->period);

	/* Turn around the axis (Rodrigues), then move to where the part belongs. */
	Vec3 k = Normalise(Vec3{this->axis[0], this->axis[1], this->axis[2]});
	float lift = this->lift == 0 ? 0 : this->lift * 0.5f * (1 - std::cos(t * 6.2831853f / this->period));
	float c = std::cos(angle);
	float s = std::sin(angle);
	float v = 1 - c;
	Matrix4 local;
	local.m = {
		c + k.x * k.x * v, k.y * k.x * v + k.z * s, k.z * k.x * v - k.y * s, 0,
		k.x * k.y * v - k.z * s, c + k.y * k.y * v, k.z * k.y * v + k.x * s, 0,
		k.x * k.z * v + k.y * s, k.y * k.z * v - k.x * s, c + k.z * k.z * v, 0,
		this->at[0] + k.x * lift, this->at[1] + k.y * lift, this->at[2] + k.z * lift, 1,
	};
	ModelInstance instance;
	instance.model = this->model;
	instance.transform = this->transform * local;
	instance.tint = this->tint;
	return instance;
}

static void PlaceModel(const ModelPack::Entry &entry, TileIndex tile, uint size_x, uint size_y, DiagDirection front, const std::array<float, 4> &tint, std::vector<ModelInstance> &models)
{
	/* Which way each direction points on the map. */
	static constexpr float HEADINGS[4][2] = {{-1, 0}, {0, 1}, {1, 0}, {0, -1}};
	const float *heading = HEADINGS[to_underlying(front)];

	/* Buildings on slopes stand on a foundation at the height of the highest corner. */
	int z = 0;
	for (uint dy = 0; dy < size_y; dy++) {
		for (uint dx = 0; dx < size_x; dx++) {
			TileIndex t = TileAddWrap(tile, dx, dy);
			if (t != INVALID_TILE) z = std::max(z, GetTileMaxPixelZ(t));
		}
	}

	ModelInstance &instance = models.emplace_back();
	instance.model = entry.model;
	float s = entry.scale;
	instance.transform.m = {
		heading[0] * s, heading[1] * s, 0, 0,
		-heading[1] * s, heading[0] * s, 0, 0,
		0, 0, s, 0,
		(TileX(tile) + size_x * 0.5f) * TILE_SIZE, (TileY(tile) + size_y * 0.5f) * TILE_SIZE, z * WORLD_Z_SCALE, 1,
	};
	instance.tint = tint;

	if (_tile_parts == nullptr) return;
	/* Parts of neighbouring buildings move a little out of step, as the 2D game does too. */
	float phase = (tile.base() * 2654435761U >> 20) / 4096.0f * 4.0f;
	for (const ModelPack::ModelPart &part : entry.parts) {
		_tile_parts->push_back({instance.transform, tint, part.model, part.at, part.axis, part.spin, part.swing, part.period, part.lift, phase});
	}
}

/**
 * Tint of things owned by a company, or grey for things owned by nobody.
 * @param owner The owner.
 */
static std::array<float, 4> GetOwnerTint(Owner owner)
{
	return ModelPack::GetTint(owner < MAX_COMPANIES ? _company_colours[owner] : Colours::Grey);
}

/**
 * Add what a bridge carries along one of its pieces: track, wires or road.
 * @param head A head of the bridge.
 * @param a Where the piece starts, in world coordinates, at the height vehicles drive on.
 * @param b Where it ends.
 * @param pack The package of 3D models.
 * @param models The models to add to.
 */
/** The bridge type that aqueducts use in the package of 3D models; they have no bridge type of their own. */
static constexpr uint AQUEDUCT_MODEL_TYPE = 17;

/**
 * Get the type of a bridge as the package of 3D models numbers them.
 * @param head A head of the bridge.
 * @return The bridge type, or AQUEDUCT_MODEL_TYPE for aqueducts.
 */
static uint GetBridgeModelType(TileIndex head)
{
	return GetTunnelBridgeTransportType(head) == TransportType::Water ? AQUEDUCT_MODEL_TYPE : GetBridgeType(head);
}

static void AddBridgeLoad(TileIndex head, const std::array<float, 3> &a, const std::array<float, 3> &b, const ModelPack &pack, std::vector<ModelInstance> &models)
{
	auto add = [&](const ModelPack::Entry *entry) {
		if (entry == nullptr) return;
		/* AddEdgeModel() works on a mesh; borrow one for its list of models. */
		static TerrainChunkMesh scratch;
		scratch.models.clear();
		AddEdgeModel(*entry, a, b, scratch);
		models.insert(models.end(), scratch.models.begin(), scratch.models.end());
	};
	switch (GetTunnelBridgeTransportType(head)) {
		case TransportType::Rail: {
			RailType rt = GetRailType(head);
			add(GetTrackPiece(rt, false, pack));
			if (HasRailCatenaryDrawn(rt)) add(pack.Find(ModelKind::Catenary, 0, 2));
			break;
		}
		case TransportType::Road:
			add(pack.Find(ModelKind::Bridge, 0, 64));
			for (RoadTramType rtt : {RoadTramType::Road, RoadTramType::Tram}) {
				RoadType rt = GetRoadType(head, rtt);
				if (rt != INVALID_ROADTYPE && HasRoadCatenaryDrawn(rt)) {
					add(pack.Find(ModelKind::Catenary, 0, 2));
					break;
				}
			}
			break;
		default:
			break;
	}
}

/**
 * Add the 3D model of the ramp of a bridge, and what it carries.
 * @param tile The tile, a bridge head.
 * @param pack The package of 3D models.
 * @param models The models to add to.
 * @return Whether the package has a model for the bridge.
 */
static bool AddBridgeRampModel(TileIndex tile, const ModelPack &pack, std::vector<ModelInstance> &models)
{
	const ModelPack::Entry *entry = pack.Find(ModelKind::Bridge, 0, GetBridgeModelType(tile) * 4 + 1);
	if (entry == nullptr) return false;

	/* From the middle of the outer edge of the tile to the middle of the edge where the bridge starts. */
	static constexpr float DIRECTIONS[4][2] = {{-1, 0}, {0, 1}, {1, 0}, {0, -1}}; // NE, SE, SW, NW
	auto [dx, dy] = DIRECTIONS[to_underlying(GetTunnelBridgeDirection(tile))];
	float cx = TileX(tile) * TILE_SIZE + TILE_SIZE * 0.5f;
	float cy = TileY(tile) * TILE_SIZE + TILE_SIZE * 0.5f;
	/* The height vehicles drive at, just inside the outer edge. */
	int ox = Clamp(static_cast<int>(cx - dx * 8), static_cast<int>(TileX(tile) * TILE_SIZE), static_cast<int>(TileX(tile) * TILE_SIZE + TILE_SIZE - 1));
	int oy = Clamp(static_cast<int>(cy - dy * 8), static_cast<int>(TileY(tile) * TILE_SIZE), static_cast<int>(TileY(tile) * TILE_SIZE + TILE_SIZE - 1));
	float outer_z = GetSlopePixelZ(ox, oy, true) * WORLD_Z_SCALE;
	float inner_z = GetBridgePixelHeight(tile) * WORLD_Z_SCALE;
	std::array<float, 3> a{cx - dx * TILE_SIZE * 0.5f, cy - dy * TILE_SIZE * 0.5f, outer_z};
	std::array<float, 3> b{cx + dx * TILE_SIZE * 0.5f, cy + dy * TILE_SIZE * 0.5f, inner_z};

	static TerrainChunkMesh scratch;
	scratch.models.clear();
	AddEdgeModel(*entry, a, b, scratch);
	models.insert(models.end(), scratch.models.begin(), scratch.models.end());
	AddBridgeLoad(tile, a, b, pack, models);
	return true;
}

/**
 * Add the 3D model of the piece of a bridge above a tile, with its pillar.
 * @param tile The tile under the bridge.
 * @param pack The package of 3D models.
 * @param mesh The mesh to add to.
 * @return Whether the package has a model for the bridge.
 */
static bool AddBridgeMiddleModel(TileIndex tile, const ModelPack &pack, TerrainChunkMesh &mesh)
{
	TileIndex head = GetSouthernBridgeEnd(tile);
	uint type = GetBridgeModelType(head);
	const ModelPack::Entry *deck = pack.Find(ModelKind::Bridge, 0, type * 4);
	if (deck == nullptr) return false;

	bool along_x = GetBridgeAxis(tile) == Axis::X;
	float cx = TileX(tile) * TILE_SIZE + TILE_SIZE * 0.5f;
	float cy = TileY(tile) * TILE_SIZE + TILE_SIZE * 0.5f;
	float z = GetBridgePixelHeight(head) * WORLD_Z_SCALE;
	float hx = along_x ? TILE_SIZE * 0.5f : 0, hy = along_x ? 0 : TILE_SIZE * 0.5f;
	std::array<float, 3> a{cx - hx, cy - hy, z}, b{cx + hx, cy + hy, z};
	AddEdgeModel(*deck, a, b, mesh);
	AddBridgeLoad(head, a, b, pack, mesh.models);

	/* Pillars stand on open land and in water, out of the way of anything below the bridge. */
	Tile below(tile);
	bool open = IsTileType(below, TileType::Clear) || IsTileType(below, TileType::Trees) || (IsTileType(below, TileType::Water) && !IsShipDepot(below) && !IsLock(below));
	const ModelPack::Entry *pillar = pack.Find(ModelKind::Bridge, 0, type * 4 + 2);
	if (!open || pillar == nullptr) return true;
	float ground = TileHeight(tile);
	for (TileIndex t : {TileXY(TileX(tile) + 1, TileY(tile)), TileXY(TileX(tile), TileY(tile) + 1), TileXY(TileX(tile) + 1, TileY(tile) + 1)}) {
		ground = std::min<float>(ground, TileHeight(t));
	}
	ground *= HEIGHT_LEVEL_SIZE;
	float top = z - 0.9f;
	if (top - ground < 0.5f) return true;
	float s = pillar->scale;
	ModelInstance &instance = mesh.models.emplace_back();
	instance.model = pillar->model;
	float ux = along_x ? 1.0f : 0.0f, uy = along_x ? 0.0f : 1.0f;
	instance.transform.m = {
		ux * s, uy * s, 0, 0,
		-uy * s, ux * s, 0, 0,
		0, 0, top - ground, 0,
		cx, cy, ground, 1,
	};
	instance.tint = {1, 1, 1, 1};
	return true;
}

/**
 * Add a model standing on a tile, turned so that its x axis points along a direction.
 * @param entry The model.
 * @param tile The tile.
 * @param ux X part of the direction, of length one together with uy.
 * @param uy Y part of the direction.
 * @param z Height of the ground under the model, in world units.
 * @param models The models to add to.
 */
static void AddTurnedModel(const ModelPack::Entry &entry, TileIndex tile, float ux, float uy, float z, std::vector<ModelInstance> &models)
{
	float s = entry.scale;
	ModelInstance &instance = models.emplace_back();
	instance.model = entry.model;
	instance.transform.m = {
		ux * s, uy * s, 0, 0,
		-uy * s, ux * s, 0, 0,
		0, 0, s, 0,
		TileX(tile) * TILE_SIZE + TILE_SIZE * 0.5f, TileY(tile) * TILE_SIZE + TILE_SIZE * 0.5f, z, 1,
	};
	instance.tint = {1, 1, 1, 1};
}

/**
 * Whether a tile is the entrance of a tunnel that is drawn with a 3D portal; the terrain of such tiles is flat.
 * @param tile The tile.
 * @param pack The package of 3D models.
 * @return True for tunnel entrances with a model.
 */
static bool HasTunnelModel(Tile tile, const ModelPack &pack)
{
	return IsTileType(tile, TileType::TunnelBridge) && IsTunnel(tile) && pack.Find(ModelKind::Tunnel, 0, 0) != nullptr;
}

/**
 * Add the 3D portal of a tunnel entrance, and the track or road leading into it.
 * @param tile The tile, a tunnel entrance.
 * @param pack The package of 3D models.
 * @param models The models to add to.
 * @return Whether the package has a model for tunnels.
 */
static bool AddTunnelModel(TileIndex tile, const ModelPack &pack, std::vector<ModelInstance> &models)
{
	if (!HasTunnelModel(Tile(tile), pack)) return false;
	/* The model faces out of the tunnel, against the direction into it. */
	static constexpr float OUT[4][2] = {{1, 0}, {0, -1}, {-1, 0}, {0, 1}}; // into the tunnel towards NE, SE, SW, NW
	auto [ux, uy] = OUT[to_underlying(GetTunnelBridgeDirection(tile))];
	uint x = TileX(tile), y = TileY(tile);
	float z = std::min({TileHeight(tile), TileHeight(TileXY(x + 1, y)), TileHeight(TileXY(x, y + 1)), TileHeight(TileXY(x + 1, y + 1))}) * HEIGHT_LEVEL_SIZE;
	AddTurnedModel(*pack.Find(ModelKind::Tunnel, 0, 0), tile, ux, uy, z, models);
	float cx = x * TILE_SIZE + TILE_SIZE * 0.5f, cy = y * TILE_SIZE + TILE_SIZE * 0.5f;
	AddBridgeLoad(tile, {cx + ux * TILE_SIZE * 0.5f, cy + uy * TILE_SIZE * 0.5f, z}, {cx - ux * TILE_SIZE * 0.5f, cy - uy * TILE_SIZE * 0.5f, z}, pack, models);
	return true;
}

/**
 * Add the 3D barriers of a level crossing, and its track.
 * @param tile The tile, a level crossing.
 * @param pack The package of 3D models.
 * @param models The models to add to.
 * @return Whether the package has a model for level crossings.
 */
static bool AddCrossingModel(TileIndex tile, const ModelPack &pack, std::vector<ModelInstance> &models);

/**
 * Add the barriers of a level crossing, open or closed.
 * @param tile The tile, a level crossing.
 * @param pack The package of 3D models.
 * @param models The models to add to.
 */
static void AddCrossingBarriers(TileIndex tile, const ModelPack &pack, std::vector<ModelInstance> &models)
{
	const ModelPack::Entry *entry = pack.Find(ModelKind::Crossing, 0, IsCrossingBarred(tile) ? 1 : 0);
	if (entry == nullptr || GetTrackPiece(GetRailType(tile), false, pack) == nullptr) return;
	bool along_x = GetCrossingRailAxis(tile) == Axis::X;
	AddTurnedModel(*entry, tile, along_x ? 1.0f : 0.0f, along_x ? 0.0f : 1.0f, GetTileMaxPixelZ(tile) * WORLD_Z_SCALE, models);
}

static bool AddCrossingModel(TileIndex tile, const ModelPack &pack, std::vector<ModelInstance> &models)
{
	const ModelPack::Entry *entry = pack.Find(ModelKind::Crossing, 0, 0);
	const ModelPack::Entry *track = GetTrackPiece(GetRailType(tile), false, pack);
	if (entry == nullptr || track == nullptr) return false;
	/* The barriers come and go with every train; they are live models (GetLiveTileModels()). */
	bool along_x = GetCrossingRailAxis(tile) == Axis::X;
	float ux = along_x ? 1.0f : 0.0f, uy = along_x ? 0.0f : 1.0f;
	uint x = TileX(tile), y = TileY(tile);
	int px = x * TILE_SIZE, py = y * TILE_SIZE;
	/* The track follows the ground from edge to edge. */
	float za = GetSlopePixelZ(px + (along_x ? 0 : 8), py + (along_x ? 8 : 0), true) * WORLD_Z_SCALE;
	float zb = GetSlopePixelZ(px + (along_x ? 15 : 8), py + (along_x ? 8 : 15), true) * WORLD_Z_SCALE;
	static TerrainChunkMesh scratch;
	scratch.models.clear();
	float cx = px + TILE_SIZE * 0.5f, cy = py + TILE_SIZE * 0.5f;
	AddEdgeModel(*track, {cx - ux * TILE_SIZE * 0.5f, cy - uy * TILE_SIZE * 0.5f, za}, {cx + ux * TILE_SIZE * 0.5f, cy + uy * TILE_SIZE * 0.5f, zb}, scratch);
	models.insert(models.end(), scratch.models.begin(), scratch.models.end());
	return true;
}

/**
 * Add the track through a rail station or waypoint tile, and its overhead wires.
 * @param tile The tile, with rail.
 * @param pack The package of 3D models.
 * @param models The models to add to.
 */
static void AddStationTrack(TileIndex tile, const ModelPack &pack, std::vector<ModelInstance> &models)
{
	RailType rt = GetRailType(tile);
	const ModelPack::Entry *track = GetTrackPiece(rt, false, pack);
	if (track == nullptr || GetRailTypeInfo(rt)->UsesOverlay()) return;
	bool along_x = GetRailStationAxis(tile) == Axis::X;
	float ux = along_x ? 1.0f : 0.0f, uy = along_x ? 0.0f : 1.0f;
	float cx = TileX(tile) * TILE_SIZE + TILE_SIZE * 0.5f, cy = TileY(tile) * TILE_SIZE + TILE_SIZE * 0.5f;
	/* Stations stand on flat foundations. */
	float z = GetTileMaxPixelZ(tile) * WORLD_Z_SCALE;
	static TerrainChunkMesh scratch;
	scratch.models.clear();
	std::array<float, 3> a{cx - ux * TILE_SIZE * 0.5f, cy - uy * TILE_SIZE * 0.5f, z}, b{cx + ux * TILE_SIZE * 0.5f, cy + uy * TILE_SIZE * 0.5f, z};
	AddEdgeModel(*track, a, b, scratch);
	if (HasRailCatenaryDrawn(rt)) {
		const ModelPack::Entry *wires = pack.Find(ModelKind::Catenary, 0, 2);
		if (wires != nullptr) AddEdgeModel(*wires, a, b, scratch);
	}
	models.insert(models.end(), scratch.models.begin(), scratch.models.end());
}

/**
 * Find whether OpenGFX3D has a model for the building on a tile, and add the model when this tile is where it stands.
 * @param tile The tile.
 * @param pack The package of 3D models.
 * @param models The models to add to.
 * @return Whether the building on the tile is drawn with a model instead of its sprites.
 */
static bool AddTileModel(TileIndex tile, const ModelPack &pack, std::vector<ModelInstance> &models)
{
	/* The base game's own colours for houses, which have none of their own. */
	static constexpr Colours HOUSE_COLOURS[4] = {Colours::Cream, Colours::Brown, Colours::Grey, Colours::White};

	switch (GetTileType(tile)) {
		case TileType::House: {
			/* Buildings of several tiles have a model for their northern tile. */
			HouseID house = GetHouseType(tile);
			TileIndex north = tile + GetHouseNorthPart(house);
			const HouseSpec *hs = HouseSpec::Get(house);
			uint size_x = hs->building_flags.Any(BUILDING_2_TILES_X) ? 2 : 1;
			uint size_y = hs->building_flags.Any(BUILDING_2_TILES_Y) ? 2 : 1;
			if (!IsHouseCompleted(tile)) {
				/* Houses of NewGRFs may draw their own building sites. */
				if (hs->grf_prop.HasGrfFile()) return false;
				uint plot = (size_x - 1) + (size_y - 1) * 2;
				const ModelPack::Entry *site = pack.Find(ModelKind::Construction, 0, GetHouseBuildingStage(tile) * 4 + plot);
				if (site == nullptr) return false;
				if (north == tile) PlaceModel(*site, tile, size_x, size_y, DiagDirection::SW, ModelPack::GetTint(Colours::White), models);
				return true;
			}
			const ModelPack::Entry *entry = pack.Find(ModelKind::House, hs->grf_prop, house);
			if (entry == nullptr) return false;
			if (north == tile) {
				uint hash = TileHash2Bit(TileX(tile), TileY(tile));
				Colours colour = hs->grf_prop.HasGrfFile() ? hs->random_colour[hash] : HOUSE_COLOURS[hash];
				PlaceModel(*entry, tile, size_x, size_y, DiagDirection::SW, ModelPack::GetTint(colour), models);
			}
			return true;
		}

		case TileType::Industry: {
			IndustryGfx gfx = GetIndustryGfx(tile);
			if (!IsIndustryCompleted(tile)) {
				if (GetIndustryTileSpec(gfx)->grf_prop.HasGrfFile()) return false;
				const ModelPack::Entry *site = pack.Find(ModelKind::Construction, 0, GetIndustryConstructionStage(tile) * 4);
				if (site == nullptr) return false;
				/* Water industries, like oil rigs, are built without a building site. */
				if (IsTileOnWater(tile)) return false;
				PlaceModel(*site, tile, 1, 1, DiagDirection::SW, ModelPack::GetTint(Colours::White), models);
				return true;
			}
			const ModelPack::Entry *entry = pack.Find(ModelKind::IndustryTile, GetIndustryTileSpec(gfx)->grf_prop, gfx);
			if (entry == nullptr) return false;
			if (entry->snow != UINT_MAX && _settings_game.game_creation.landscape == LandscapeType::Arctic) {
				ModelPack::Entry snowy = *entry;
				snowy.model = entry->snow;
				PlaceModel(snowy, tile, 1, 1, DiagDirection::SW, ModelPack::GetTint(Industry::GetByTile(tile)->random_colour), models);
				return true;
			}
			PlaceModel(*entry, tile, 1, 1, DiagDirection::SW, ModelPack::GetTint(Industry::GetByTile(tile)->random_colour), models);
			return true;
		}

		case TileType::Station: {
			/* Things along a track or road point along it, with the side where the 2D game puts the station building on the right. */
			const ModelPack::Entry *entry = nullptr;
			DiagDirection front;
			if (HasStationRail(tile) && !IsRailWaypoint(tile)) {
				const StationSpec *spec = GetStationSpec(tile);
				entry = spec != nullptr ? pack.Find(ModelKind::RailStation, spec->grf_prop, 0) : pack.Find(ModelKind::RailStation, 0, GetStationGfx(tile) / 2);
				front = GetRailStationAxis(tile) == Axis::X ? DiagDirection::SW : DiagDirection::NW;
			} else if (IsBayRoadStopTile(tile) || (IsDriveThroughStopTile(tile) && !IsRoadWaypoint(tile))) {
				const RoadStopSpec *spec = GetRoadStopSpec(tile);
				bool drive_through = IsDriveThroughStopTile(tile);
				uint id = (drive_through ? 2 : 0) + (IsTruckStop(tile) ? 1 : 0);
				entry = spec != nullptr ? pack.Find(ModelKind::RoadStop, spec->grf_prop, 0) : pack.Find(ModelKind::RoadStop, 0, id);
				/* The front of a bay faces the road. */
				front = drive_through ? (GetDriveThroughStopAxis(tile) == Axis::X ? DiagDirection::SW : DiagDirection::NW) : GetBayRoadStopDir(tile);
			} else if (IsAirport(tile)) {
				entry = pack.Find(ModelKind::AirportTile, 0, GetAirportGfx(tile));
				front = DiagDirection::SW;
			} else if (IsDock(tile)) {
				if (IsDockWaterPart(tile)) {
					/* The water part points away from the shore part next to it. */
					Axis axis = static_cast<Axis>(GetStationGfx(tile) - GFX_DOCK_BASE_WATER_PART);
					front = AxisToDiagDir(axis);
					TileIndex shore = TileAddByDiagDir(tile, ReverseDiagDir(front));
					if (shore == INVALID_TILE || !IsDockTile(shore) || IsDockWaterPart(shore)) front = ReverseDiagDir(front);
					entry = pack.Find(ModelKind::Dock, 0, 1);
				} else {
					front = GetDockDirection(tile);
					entry = pack.Find(ModelKind::Dock, 0, 0);
				}
			} else if (IsBuoy(tile)) {
				entry = pack.Find(ModelKind::Buoy, 0, 0);
				front = DiagDirection::SW;
			} else if (IsRailWaypoint(tile) && GetStationSpec(tile) == nullptr) {
				entry = pack.Find(ModelKind::Waypoint, 0, 0);
				front = GetRailStationAxis(tile) == Axis::X ? DiagDirection::SW : DiagDirection::NW;
			} else if (IsRoadWaypoint(tile) && GetRoadStopSpec(tile) == nullptr) {
				entry = pack.Find(ModelKind::Waypoint, 0, 1);
				front = GetDriveThroughStopAxis(tile) == Axis::X ? DiagDirection::SW : DiagDirection::NW;
			}
			if (entry == nullptr) return false;
			PlaceModel(*entry, tile, 1, 1, front, GetOwnerTint(IsBuoy(tile) ? OWNER_NONE : GetTileOwner(tile)), models);
			/* The track through stations and waypoints of the base game is the same as on plain rail; GetGroundPattern() gives them its ballast. */
			if (HasStationRail(tile) && GetStationSpec(tile) == nullptr) AddStationTrack(tile, pack, models);
			return true;
		}

		case TileType::TunnelBridge:
			if (IsTunnel(tile)) return AddTunnelModel(tile, pack, models);
			return AddBridgeRampModel(tile, pack, models);

		case TileType::Railway: {
			if (!IsRailDepot(tile)) return false;
			const ModelPack::Entry *entry = pack.Find(ModelKind::RailDepot, 0, 0);
			if (entry == nullptr) return false;
			PlaceModel(*entry, tile, 1, 1, GetRailDepotDirection(tile), GetOwnerTint(GetTileOwner(tile)), models);
			return true;
		}

		case TileType::Road: {
			if (IsLevelCrossing(tile)) return AddCrossingModel(tile, pack, models);
			if (!IsRoadDepot(tile)) return false;
			const ModelPack::Entry *entry = pack.Find(ModelKind::RoadDepot, 0, 0);
			if (entry == nullptr) return false;
			PlaceModel(*entry, tile, 1, 1, GetRoadDepotDirection(tile), GetOwnerTint(GetTileOwner(tile)), models);
			return true;
		}

		case TileType::Water: {
			if (IsLock(tile)) {
				const ModelPack::Entry *entry = pack.Find(ModelKind::Lock, 0, to_underlying(GetLockPart(tile)));
				if (entry == nullptr) return false;
				PlaceModel(*entry, tile, 1, 1, GetLockDirection(tile), GetOwnerTint(GetTileOwner(tile)), models);
				return true;
			}
			if (!IsShipDepot(tile)) return false;
			const ModelPack::Entry *entry = pack.Find(ModelKind::ShipDepot, 0, 0);
			if (entry == nullptr) return false;
			/* One model for both tiles, on the northern one. */
			if (GetShipDepotPart(tile) == DepotPart::North) {
				bool along_x = GetShipDepotAxis(tile) == Axis::X;
				PlaceModel(*entry, tile, along_x ? 2 : 1, along_x ? 1 : 2, along_x ? DiagDirection::SW : DiagDirection::SE, GetOwnerTint(GetTileOwner(tile)), models);
			}
			return true;
		}

		case TileType::Object: {
			const Object *o = Object::GetByTile(tile);
			const ObjectSpec *spec = ObjectSpec::Get(o->type);
			const ModelPack::Entry *entry = pack.Find(ModelKind::Object, spec->grf_prop, o->type);
			if (entry == nullptr) return false;
			if (tile == o->location.tile) {
				static constexpr DiagDirection VIEWS[4] = {DiagDirection::SW, DiagDirection::NW, DiagDirection::NE, DiagDirection::SE};
				PlaceModel(*entry, tile, o->location.w, o->location.h, VIEWS[o->view & 3], GetOwnerTint(GetTileOwner(tile)), models);
			}
			return true;
		}

		default:
			return false;
	}
}

/**
 * Get the 3D models a tile is drawn with, to tell whether a change to the tile changes what is drawn.
 * @param tile The tile.
 * @param pack The package of 3D models.
 * @param[out] models The models; previous contents are discarded.
 * @param[out] parts Their moving parts; previous contents are discarded.
 * @return Whether the tile has a model in place of its sprites.
 */
bool GetTileModels(TileIndex tile, const ModelPack &pack, std::vector<ModelInstance> &models, std::vector<AnimatedPart> &parts)
{
	models.clear();
	parts.clear();
	if (pack.IsEmpty()) return false;
	_tile_parts = &parts;
	bool replaced = AddTileModel(tile, pack, models);
	_tile_parts = nullptr;
	return replaced;
}

/**
 * Add a see-through model of what a build tool is about to build on a tile.
 * @param preview What is about to be built.
 * @param tile The tile.
 * @param track For rail stations, which track of the station the tile is on, counted across the platforms.
 * @param pack The package of 3D models.
 * @param models The models to add to.
 */
void AddPreviewModel(const View3DPreview &preview, TileIndex tile, uint track, const ModelPack &pack, std::vector<ModelInstance> &models)
{
	const ModelPack::Entry *entry = nullptr;
	DiagDirection front = preview.direction;
	switch (preview.kind) {
		case View3DPreview::Kind::RailDepot:
			entry = pack.Find(ModelKind::RailDepot, 0, 0);
			break;
		case View3DPreview::Kind::RoadDepot:
			entry = pack.Find(ModelKind::RoadDepot, 0, 0);
			break;
		case View3DPreview::Kind::RailStation:
			/* Every other track gets the station building, the others a bare platform. */
			entry = pack.Find(ModelKind::RailStation, 0, track % 2 == 0 ? 1 : 0);
			front = preview.axis == Axis::X ? DiagDirection::SW : DiagDirection::NW;
			break;
		case View3DPreview::Kind::RoadStop:
			entry = pack.Find(ModelKind::RoadStop, 0, (preview.drive_through ? 2 : 0) + (preview.truck ? 1 : 0));
			if (preview.drive_through) front = preview.axis == Axis::X ? DiagDirection::SW : DiagDirection::NW;
			break;
		default:
			break;
	}
	if (entry == nullptr) return;
	std::array<float, 4> tint = GetOwnerTint(_local_company);
	tint[3] = 0.55f;
	PlaceModel(*entry, tile, 1, 1, front, tint, models);
}

/**
 * Add 3D signals on a rail tile, where the 2D game draws its signal sprites.
 * @param tile The tile, a plain rail tile with signals.
 * @param pack The package of 3D models.
 * @param models The models to add to.
 * @param[out] places Where the 2D game puts the bounding boxes of the signals that got a model, in world coordinates.
 */
static void AddSignalModels(Tile tile, const ModelPack &pack, std::vector<ModelInstance> &models, std::vector<std::pair<int, int>> &places)
{
	/* Where the 2D game puts signals, by position as in DrawSingleSignal(); the first half for the left side. */
	static constexpr uint8_t POSITIONS[2][12][2] = {
		{{8, 5}, {14, 1}, {1, 14}, {9, 11}, {1, 0}, {3, 10}, {11, 4}, {14, 14}, {11, 3}, {4, 13}, {3, 4}, {11, 13}},
		{{14, 1}, {12, 10}, {4, 6}, {1, 14}, {10, 4}, {0, 1}, {14, 14}, {5, 12}, {11, 13}, {4, 3}, {13, 4}, {3, 11}},
	};
	/* The direction a signal faces, towards the trains it is for, by the image offsets of DrawSignals(). */
	static constexpr float D = 0.70710678f;
	static constexpr float FACING[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {-D, D}, {D, -D}, {D, D}, {-D, -D}};
	enum Image : uint8_t { SW, NE, SE, NW, E, W, S, N };
	struct Signal { uint8_t bit; Image image; uint8_t pos; Track track; };
	static constexpr Signal LEFT[] = {{2, N, 0, Track::Left}, {3, S, 1, Track::Left}};
	static constexpr Signal RIGHT[] = {{0, N, 2, Track::Right}, {1, S, 3, Track::Right}};
	static constexpr Signal UPPER[] = {{3, W, 4, Track::Upper}, {2, E, 5, Track::Upper}};
	static constexpr Signal LOWER[] = {{1, W, 6, Track::Lower}, {0, E, 7, Track::Lower}};
	static constexpr Signal X[] = {{3, SW, 8, Track::X}, {2, NE, 9, Track::X}};
	static constexpr Signal Y[] = {{3, SE, 10, Track::Y}, {2, NW, 11, Track::Y}};

	TrackBits rails = GetTrackBits(tile);
	std::vector<std::span<const Signal>> groups;
	if (rails.Test(Track::Y)) {
		groups.emplace_back(Y);
	} else if (rails.Test(Track::X)) {
		groups.emplace_back(X);
	} else {
		if (rails.Test(Track::Left)) groups.emplace_back(LEFT);
		if (rails.Test(Track::Right)) groups.emplace_back(RIGHT);
		if (rails.Test(Track::Upper)) groups.emplace_back(UPPER);
		if (rails.Test(Track::Lower)) groups.emplace_back(LOWER);
	}

	bool right = _settings_game.construction.train_signal_side == TrainSignalSide::Right ||
			(_settings_game.construction.train_signal_side == TrainSignalSide::RoadVehicleDrivingSide && _settings_game.vehicle.road_side == RoadVehicleDrivingSide::Right);
	for (std::span<const Signal> group : groups) {
		for (const Signal &signal : group) {
			if (!IsSignalPresent(tile, signal.bit)) continue;
			uint type = to_underlying(GetSignalType(tile, signal.track));
			uint variant = to_underlying(GetSignalVariant(tile, signal.track));
			uint state = to_underlying(GetSingleSignalState(tile, signal.bit));
			const ModelPack::Entry *entry = pack.Find(ModelKind::Signal, 0, type * 4 + variant * 2 + state);
			if (entry == nullptr) continue;

			int x = TileX(tile) * TILE_SIZE + POSITIONS[right][signal.pos][0];
			int y = TileY(tile) * TILE_SIZE + POSITIONS[right][signal.pos][1];
			places.emplace_back(x, y);

			/* The height of the track at the signal, as in GetSafeSlopeZ(). */
			int zx = x, zy = y;
			switch (signal.track) {
				case Track::Upper: zx &= ~0xF; zy &= ~0xF; break;
				case Track::Lower: zx |= 0xF; zy |= 0xF; break;
				case Track::Left: zx |= 0xF; zy &= ~0xF; break;
				case Track::Right: zx &= ~0xF; zy |= 0xF; break;
				default: break;
			}
			float z = GetSlopePixelZ(zx, zy) * WORLD_Z_SCALE;

			/* The models have the track on their right; signals on the left side of the track are mirrored. */
			float s = entry->scale;
			float m = right ? s : -s;
			auto [dx, dy] = FACING[signal.image];
			ModelInstance &instance = models.emplace_back();
			instance.model = entry->model;
			instance.transform.m = {
				dx * s, dy * s, 0, 0,
				-dy * m, dx * m, 0, 0,
				0, 0, s, 0,
				x + 0.5f, y + 0.5f, z, 1,
			};
			instance.tint = {1, 1, 1, 1};
		}
	}
}

/**
 * Get a key over what of a rail tile or level crossing the 3D view draws in its terrain chunk. Trains change these
 * tiles all the time in ways that do not change the chunk: they reserve their path, which is not shown unless the
 * game is set to, and change signals and crossing barriers, which are live models (GetLiveTileModels()).
 * @param tile The tile.
 * @param[out] key The key; when it stays the same, the terrain chunk need not be rebuilt for the tile.
 * @return Whether the tile is of a kind that has such a key.
 */
bool GetTileChangeKey(TileIndex tile, uint64_t &key)
{
	Tile t(tile);
	bool rail = IsTileType(t, TileType::Railway);
	bool crossing = IsTileType(t, TileType::Road) && IsLevelCrossing(t);
	if (!rail && !crossing) return false;
	bool reservation = _settings_client.gui.show_track_reservation;
	uint16_t m2 = t.m2();
	uint8_t m4 = t.m4(), m5 = t.m5();
	if (rail && IsPlainRail(t)) {
		m4 &= 0x0F; // signal states
		if (!reservation) m2 &= ~0x0F00; // reserved tracks
	} else if (rail) {
		if (!reservation) m5 &= ~(1 << 4); // depot reservation
	} else {
		m5 &= ~(1 << 5); // barriers down
		if (!reservation) m5 &= ~(1 << 4); // crossing reservation
	}
	key = 0xcbf29ce484222325ULL;
	for (uint v : {uint{t.type()}, uint{t.height()}, uint{t.m1()}, uint{m2}, uint{t.m3()}, uint{m4}, uint{m5}, uint{t.m6()}, uint{t.m7()}, uint{t.m8()}}) {
		key = (key ^ v) * 0x100000001b3ULL;
	}
	return true;
}

/**
 * Whether a tile has models that change often, such as signals and the barriers of level crossings.
 * @param tile The tile.
 * @return True for rail tiles with signals and level crossings.
 */
bool HasLiveTileModels(TileIndex tile)
{
	Tile t(tile);
	if (IsTileType(t, TileType::Railway)) return IsPlainRail(t) && HasSignals(t);
	return IsTileType(t, TileType::Road) && IsLevelCrossing(t);
}

/**
 * Get the models of a tile that change often, such as signals and the barriers of level crossings.
 * @param tile The tile.
 * @param pack The package of 3D models.
 * @param[out] models The models; previous contents are discarded.
 */
void GetLiveTileModels(TileIndex tile, const ModelPack &pack, std::vector<ModelInstance> &models)
{
	models.clear();
	if (pack.IsEmpty() || !HasLiveTileModels(tile)) return;
	if (IsTileType(tile, TileType::Railway)) {
		static std::vector<std::pair<int, int>> places;
		places.clear();
		AddSignalModels(Tile(tile), pack, models, places);
	} else {
		AddCrossingBarriers(tile, pack, models);
	}
}

/**
 * Draw the ground of plain track and road with the textures of the model package alone, rather than from the sprites of the 2D game.
 * Only where the 3D models stand in for what the sprites show, and nothing else is drawn on the ground.
 * @param tile The tile.
 * @param slope Its slope.
 * @param pack The model package.
 * @param textures The ground textures.
 * @param[out] result The ground, when there is a pattern for the tile.
 * @return Whether there is.
 */
static bool GetGroundPattern(TileIndex tile, Slope slope, const ModelPack *pack, GroundTextures &textures, TileGround &result)
{
	if (pack == nullptr || pack->IsEmpty() || IsHalftileSlope(slope) || IsBridgeAbove(tile)) return false;

	GroundTextures::PatternKind kind;
	uint8_t bits;
	uint8_t under = GROUND_TEXTURE_GRASS + 3;
	uint8_t reserved = 0;
	if (IsTileType(tile, TileType::Railway) && IsPlainRail(tile)) {
		RailType rt = GetRailType(tile);
		if (GetTrackPiece(rt, false, *pack) == nullptr || GetTrackPiece(rt, true, *pack) == nullptr) return false;
		/* Railway types of NewGRFs can draw their own ground. */
		if (GetRailTypeInfo(rt)->UsesOverlay()) return false;
		switch (GetRailGroundType(tile)) {
			case RailGroundType::Barren: under = GROUND_TEXTURE_GRASS; break;
			case RailGroundType::SnowOrDesert: under = GROUND_TEXTURE_SNOW_DESERT + 3; break;
			case RailGroundType::HalfTileWater: return false;
			default: break;
		}
		kind = GroundTextures::PatternKind::Rail;
		bits = static_cast<uint8_t>(GetTrackBits(tile).base());
		/* The reservation of a path shows along the track. */
		if (_settings_client.gui.show_track_reservation) reserved = static_cast<uint8_t>(GetRailReservationTrackBits(tile).base());
	} else if (IsTileType(tile, TileType::Road) && IsNormalRoad(tile)) {
		/* Only the road of the base game; others have their own look. Road works and one-way arrows are drawn on the road. */
		if (HasRoadWorks(tile) || GetDisallowedRoadDirections(tile).Any()) return false;
		if (HasRoadTypeRoad(tile) && GetRoadTypeRoad(tile) != ROADTYPE_ROAD) return false;
		RoadBits tram = HasRoadTypeTram(tile) ? GetRoadBits(tile, RoadTramType::Tram) : RoadBits{};
		if (tram.Any() && (pack->Find(ModelKind::Roadside, 0, 4) == nullptr || GetRoadTypeTram(tile) != ROADTYPE_TRAM)) return false;
		Roadside roadside = GetRoadside(tile);
		bool paved = roadside == Roadside::Paved || roadside == Roadside::StreetLights || roadside == Roadside::Trees;
		RoadBits road = HasRoadTypeRoad(tile) ? GetRoadBits(tile, RoadTramType::Road) : RoadBits{};
		kind = paved && road.Any() ? GroundTextures::PatternKind::PavedRoad : GroundTextures::PatternKind::Road;
		if (IsOnSnowOrDesert(tile)) {
			under = GROUND_TEXTURE_SNOW_DESERT + 3;
		} else if (roadside == Roadside::Barren) {
			under = GROUND_TEXTURE_GRASS;
		}
		bits = static_cast<uint8_t>(road.base());
	} else if (IsTileType(tile, TileType::Station) && HasStationRail(tile) && GetStationSpec(tile) == nullptr) {
		/* Stations and waypoints of the base game: ballast along the track, the platforms are models. */
		RailType rt = GetRailType(tile);
		if (GetTrackPiece(rt, false, *pack) == nullptr || GetRailTypeInfo(rt)->UsesOverlay()) return false;
		kind = GroundTextures::PatternKind::Rail;
		bits = static_cast<uint8_t>(TrackBits{GetRailStationTrack(tile)}.base());
		if (_settings_client.gui.show_track_reservation && HasStationReservation(tile)) reserved = bits;
	} else if (IsTileType(tile, TileType::Station) && IsAnyRoadStop(tile) && GetRoadStopSpec(tile) == nullptr) {
		/* Road stops of the base game: road along the stop, the shelters and canopies are models. */
		if (HasRoadTypeTram(tile) || !HasRoadTypeRoad(tile) || GetRoadTypeRoad(tile) != ROADTYPE_ROAD) return false;
		kind = GroundTextures::PatternKind::Road;
		Axis axis = IsDriveThroughStopTile(tile) ? GetDriveThroughStopAxis(tile) : DiagDirToAxis(GetBayRoadStopDir(tile));
		bits = static_cast<uint8_t>((axis == Axis::X ? ROAD_X : ROAD_Y).base());
	} else {
		return false;
	}

	uint texture = textures.GetPattern(kind, bits, reserved);
	if (texture == GROUND_TEXTURE_NONE) return false;
	result = TileGround{texture, COLOUR_WHITE};
	result.under = under;
	return true;
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
 * @param pack The package of 3D models, or \c nullptr.
 * @param mesh The mesh to add to.
 * @param ground_only Only look at the ground; used for landscape tiles with a selection on them, which add their trees and fences themselves.
 * @return How to draw the ground of the tile.
 */
static TileGround AddTileSprites(uint x, uint y, const std::array<uint8_t, 4> &heights, GroundTextures &textures, SpriteAtlas &atlas, const ModelPack *pack, TerrainChunkMesh &mesh, bool ground_only = false)
{
	static TileSprites sprites;
	static std::vector<SpritePart> group_parts;

	TileIndex tile = TileXY(x, y);
	CollectTileSprites(tile, sprites);

	/* A building with a 3D model keeps its ground and foundation, but not its other sprites. */
	_tile_parts = &mesh.parts;
	bool replaced = !ground_only && pack != nullptr && !pack->IsEmpty() && AddTileModel(tile, *pack, mesh.models);
	_tile_parts = nullptr;

	/* Signals with a 3D model replace the sprites the 2D game draws for them. */
	static std::vector<std::pair<int, int>> signal_places;
	signal_places.clear();
	if (!ground_only && !replaced && pack != nullptr && !pack->IsEmpty() && IsTileType(tile, TileType::Railway) && IsPlainRail(tile) && HasSignals(tile)) {
		AddSignalModels(Tile(tile), *pack, mesh.live[tile], signal_places);
	}
	if (replaced && IsTileType(tile, TileType::Road) && IsLevelCrossing(tile)) AddCrossingBarriers(tile, *pack, mesh.live[tile]);

	/* A bridge above the tile with a 3D model replaces what the 2D game draws for the bridge. */
	uint bridge_first = sprites.bridge_first;
	if (ground_only || bridge_first == UINT_MAX || pack == nullptr || pack->IsEmpty() || !AddBridgeMiddleModel(tile, *pack, mesh)) bridge_first = UINT_MAX;

	bool catenary = !ground_only && pack != nullptr && !pack->IsEmpty() && IsTileType(tile, TileType::Railway) && IsPlainRail(tile) && HasCatenaryModels(Tile(tile), *pack);
	bool road_details = !ground_only && !replaced && pack != nullptr && !pack->IsEmpty() && IsTileType(tile, TileType::Road) && IsNormalRoad(tile);
	bool road_wires = road_details && GetRoadWireBits(Tile(tile), *pack).Any();
	bool rail_fences = !ground_only && pack != nullptr && !pack->IsEmpty() && pack->Find(ModelKind::Fence, 0, 0) != nullptr && IsTileType(tile, TileType::Railway) && IsPlainRail(tile);

	int tile_x = x * TILE_SIZE;
	int tile_y = y * TILE_SIZE;
	auto [slope, tile_z] = GetTilePixelSlope(tile);

	/* Foundations under half a tile leave the other half sloped; those keep the foundation sprite. */
	bool flat_foundation = !IsHalftileSlope(slope) && std::ranges::any_of(sprites.groups, &TileSpriteGroup::foundation);
	/* Most foundations are flat on top; inclined ones, as under ramps and tracks going up a hill, slope along the way up. */
	auto [foundation_slope, foundation_z] = GetFoundationPixelSlope(tile);
	if (IsSteepSlope(foundation_slope) || IsHalftileSlope(foundation_slope)) foundation_slope = SLOPE_FLAT;
	std::array<uint8_t, 4> foundation_heights{};
	if (foundation_slope != SLOPE_FLAT) {
		auto raised = [&](Slope corner) -> uint8_t { return (foundation_slope & corner) != 0 ? 1 : 0; };
		foundation_heights = {raised(SLOPE_N), raised(SLOPE_W), raised(SLOPE_E), raised(SLOPE_S)};
	}
	static std::vector<SpritePart> ground;
	ground = sprites.ground;
	/* A tunnel portal has its own track or road; the ground in front of it is plain. */
	if (replaced && IsTileType(tile, TileType::TunnelBridge) && IsTunnel(tile)) ground.clear();
	/* A level crossing with 3D barriers and lights gets plain road as its ground: the ground sprite of the 2D game has the
	 * lights and posts painted in, and would only show them as they were when the chunk was built. */
	if (replaced && IsTileType(tile, TileType::Road) && IsLevelCrossing(tile)) {
		SpriteID base = GetRailTypeInfo(GetRailType(tile))->base_sprites.crossing;
		for (SpritePart &part : ground) {
			SpriteID sprite = part.sprite & SPRITE_MASK;
			if (sprite < base || sprite >= base + 12) continue;
			uint offset = sprite - base;
			uint variant = offset / 4; // grass, paved, snow or desert
			SpriteID road = SPR_ROAD_Y + (offset & 1);
			if (variant == 1) road -= NUM_SLOPES;
			if (variant == 2) road += NUM_SLOPES;
			part.sprite = (part.sprite & ~SPRITE_MASK) | road;
		}
	}

	uint index = 0;
	for (uint g = 0; g < sprites.groups.size(); g++) {
		const TileSpriteGroup &group = sprites.groups[g];
		if (group.count == 0) continue;
		if (g >= bridge_first) continue;
		if (group.foundation && flat_foundation) {
			/* The foundation becomes walls of the terrain. What lies on top of it, like tracks, becomes the ground of the tile;
			 * the 2D game draws that higher up, by the height of the foundation. */
			for (uint i = group.first + 1; i < group.first + group.count; i++) {
				SpritePart part = sprites.parts[i];
				part.y += (foundation_z - tile_z) * ZOOM_BASE;
				ground.push_back(part);
			}
			continue;
		}
		if (ground_only || (replaced && !group.foundation)) continue;
		if (rail_fences && IsRailFenceSprite(sprites.parts[group.first].sprite)) continue;
		if (road_details) {
			SpriteID sprite = sprites.parts[group.first].sprite & SPRITE_MASK;
			if ((sprite == 0x57E || sprite == 0x57F || sprite == 0x1212) && AddRoadDetailModel(group, sprite == 0x1212, *pack, mesh)) continue;
			if (road_wires && IsRoadWireSprite(sprite)) continue;
		}
		if (catenary) {
			SpriteID sprite = sprites.parts[group.first].sprite & SPRITE_MASK;
			if (sprite >= SPR_WIRE_BASE && sprite < SPR_PYLON_BASE) continue;
			if (sprite >= SPR_PYLON_BASE && sprite < SPR_PYLON_BASE + 8) {
				AddPylonModel(group, sprite - SPR_PYLON_BASE, *pack, mesh);
				continue;
			}
		}
		if (group.xmin == group.xmax && group.ymin == group.ymax &&
				std::ranges::find(signal_places, std::pair<int, int>{group.xmin, group.ymin}) != signal_places.end()) continue;

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
		AddUprightSprite(atlas.Get(group_parts), centre_x, centre_y, static_cast<float>(group.zmin), nearer, mesh.sprites);
	}

	TileGround result;
	if (!ground_only && textures.IsMixing() && GetGroundPattern(tile, slope, pack, textures, result)) {
		/* Drawn with the textures of the model package alone. */
	} else if (ground.empty()) {
		result = ground_only ? GetTileGround(Tile(tile), x, y, false) : TileGround{GROUND_TEXTURE_GRASS, COLOUR_WHITE};
	} else {
		uint texture = textures.Get(ground, flat_foundation ? foundation_heights : heights);
		if (texture == GROUND_TEXTURE_NONE) {
			result = TileGround{GROUND_TEXTURE_NONE, TerrainColour{0x80, 0x80, 0x80}};
		} else if (textures.IsMixing()) {
			/* The sprites have the shading of the slope in them, but the textures mixed in do not; the shader evens that out. */
			result = TileGround{texture, COLOUR_WHITE};
			result.under = textures.GetUnder(texture);
		} else {
			result = TileGround{texture, COLOUR_WHITE, true};
		}
	}
	result.foundation = flat_foundation;
	result.foundation_slope = flat_foundation ? foundation_slope : SLOPE_FLAT;
	return result;
}

/**
 * Mark a tile with a see-through plate that follows the ground, like the 2D game draws its selections on the ground.
 * @param mark How the tile is marked.
 * @param x X coordinate of the tile.
 * @param y Y coordinate of the tile.
 * @param heights Heights of the northern, western, eastern and southern corner of the top of the tile, in height levels.
 * @param split_ns Whether the ground of the tile is split along the line from north to south, rather than from west to east.
 * @param pack The package of 3D models, or \c nullptr.
 * @param mesh The mesh to add to.
 */
static void AddTileMark(TileMark mark, uint x, uint y, const std::array<float, 4> &heights, bool split_ns, const ModelPack *pack, TerrainChunkMesh &mesh)
{
	const ModelPack::Entry *half = pack != nullptr ? pack->Find(ModelKind::Interface, 0, 5) : nullptr;
	if (half == nullptr) return;
	std::array<float, 4> tint;
	switch (mark) {
		case TileMark::Blue: tint = {0.35f, 0.6f, 1.0f, 0.45f}; break;
		case TileMark::Red: tint = {1.0f, 0.25f, 0.2f, 0.5f}; break;
		case TileMark::Authority: tint = {1.0f, 0.55f, 0.35f, 0.4f}; break;
		default: tint = {1.0f, 1.0f, 1.0f, 0.4f}; break;
	}
	/* A little above the ground, so the plate is not lost in it. */
	static constexpr float LIFT = 0.3f;
	enum : uint { N, W, E, S };
	static constexpr float CORNERS[4][2] = {{0, 0}, {TILE_SIZE, 0}, {0, TILE_SIZE}, {TILE_SIZE, TILE_SIZE}};
	auto corner = [&](uint c) {
		return std::array<float, 3>{x * TILE_SIZE + CORNERS[c][0], y * TILE_SIZE + CORNERS[c][1], heights[c] * HEIGHT_LEVEL_SIZE + LIFT};
	};
	/* Each half from a corner along two sides, turning the same way as the model, so its top stays up. */
	auto add = [&](uint origin, uint a, uint b) {
		std::array<float, 3> o = corner(origin), pa = corner(a), pb = corner(b);
		ModelInstance &instance = mesh.models.emplace_back();
		instance.model = half->model;
		instance.transform.m = {
			pa[0] - o[0], pa[1] - o[1], pa[2] - o[2], 0,
			pb[0] - o[0], pb[1] - o[1], pb[2] - o[2], 0,
			0, 0, 1, 0,
			o[0], o[1], o[2], 1,
		};
		instance.tint = tint;
	};
	if (split_ns) {
		add(W, S, N);
		add(E, N, S);
	} else {
		add(N, W, E);
		add(S, E, W);
	}
}

/**
 * Build the geometry of one chunk of terrain from the current state of the map.
 * @param chunk_x X coordinate of the chunk, in chunks.
 * @param chunk_y Y coordinate of the chunk, in chunks.
 * @param textures The ground textures.
 * @param atlas The atlas that provides the sprites.
 * @param pack The package of 3D models, or \c nullptr.
 * @param[out] mesh The mesh to fill; previous contents are discarded.
 */
void BuildTerrainChunkMesh(uint chunk_x, uint chunk_y, GroundTextures &textures, SpriteAtlas &atlas, const ModelPack *pack, TerrainChunkMesh &mesh)
{
	/* Direction towards the light. It comes from the east, like in the original graphics. */
	static constexpr float SUN_X = -0.4683f, SUN_Y = 0.5620f, SUN_Z = 0.6817f;
	static constexpr float AMBIENT = 0.45f;
	static constexpr float DIFFUSE = 0.80f;
	/* Change in world height per world unit of distance, for one height level difference across a tile. */
	static constexpr float SLOPE_SCALE = 1.5f * TILE_HEIGHT / TILE_SIZE; // a little more than the true slope, as the 2D ground sprites shade slopes strongly

	mesh.vertices.clear();
	mesh.water.clear();
	mesh.sprites.clear();

	/* With ground textures in the model package, coasts are land that goes down into the water. */
	const bool pack_ground = textures.IsMixing();
	mesh.models.clear();
	mesh.parts.clear();
	mesh.live.clear();
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

			/* Where a coast is drawn as land that goes down into the water, its corners in the sea lie below the water. */
			float fn = hn, fw = hw, fe = he, fs = hs;
			bool shore = pack_ground && IsShoreSlope(tile, x, y);
			if (shore) {
				if (IsSeaCorner(x, y)) fn = -SEABED_DEPTH;
				if (IsSeaCorner(x + 1, y)) fw = -SEABED_DEPTH;
				if (IsSeaCorner(x, y + 1)) fe = -SEABED_DEPTH;
				if (IsSeaCorner(x + 1, y + 1)) fs = -SEABED_DEPTH;
			}

			/* Light the tile as one flat surface, using the average slope in both directions.
			 * A coast is lit as the slope it is on the map: the steeper drop below the water would make it too dark or too bright. */
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

			/* The selection of a build tool, the catchment area of a station and such are marked on top of the ground. */
			TileMark mark = GetTileMark(tile);

			TileGround ground;
			if (collect) {
				int low = std::min({hn, hw, he, hs});
				ground = AddTileSprites(x, y, {static_cast<uint8_t>(hn - low), static_cast<uint8_t>(hw - low), static_cast<uint8_t>(he - low), static_cast<uint8_t>(hs - low)}, textures, atlas, pack, mesh);
			} else {
				ground = GetTileGround(tile, x, y, pack_ground);
			}
			if (ground.shaded) light = AMBIENT + DIFFUSE * SUN_Z;
			/* A slight checkerboard keeps individual tiles recognisable where there is no texture. */
			if (ground.texture == GROUND_TEXTURE_NONE && ((x ^ y) & 1) != 0) light *= 0.97f;

			/* The light can be brighter than 1; the shader doubles the colour to make up for the halving here. */
			light *= 0.5f;
			uint8_t r = static_cast<uint8_t>(std::clamp(ground.colour.r * light, 0.0f, 255.0f));
			uint8_t g = static_cast<uint8_t>(std::clamp(ground.colour.g * light, 0.0f, 255.0f));
			uint8_t b = static_cast<uint8_t>(std::clamp(ground.colour.b * light, 0.0f, 255.0f));

			auto vertex = [&](uint vx, uint vy, float h) {
				return TerrainVertex{static_cast<float>(vx), static_cast<float>(vy), h, r, g, b, ground.under, static_cast<float>(ground.texture), static_cast<float>(vx - x), static_cast<float>(vy - y)};
			};
			if (ground.foundation) {
				/* Walls from the flat top of the foundation down to the ground around it. */
				static constexpr TerrainColour STONE{0xa8, 0xa0, 0x90};
				int top = std::max({hn, hw, he, hs});
				/* An inclined foundation has its raised corners at the top, the others one level lower. */
				Slope fs_slope = ground.foundation_slope;
				auto top_of = [&](Slope corner) { return fs_slope == SLOPE_FLAT || (fs_slope & corner) != 0 ? top : top - 1; };
				int tn = top_of(SLOPE_N), tw = top_of(SLOPE_W), te = top_of(SLOPE_E), ts = top_of(SLOPE_S);
				auto wall = [&](uint ax, uint ay, int ha, int ta, uint bx, uint by, int hb, int tb, float nx, float ny) {
					if ((ha >= ta && hb >= tb) || mesh.vertices.size() / 4 >= MAX_CHUNK_QUADS - 1) return;
					float wall_light = (AMBIENT + DIFFUSE * std::max(0.0f, nx * SUN_X + ny * SUN_Y)) * 0.5f;
					uint8_t wr = static_cast<uint8_t>(std::clamp(STONE.r * wall_light, 0.0f, 255.0f));
					uint8_t wg = static_cast<uint8_t>(std::clamp(STONE.g * wall_light, 0.0f, 255.0f));
					uint8_t wb = static_cast<uint8_t>(std::clamp(STONE.b * wall_light, 0.0f, 255.0f));
					auto corner = [&](uint vx, uint vy, int h) {
						return TerrainVertex{static_cast<float>(vx), static_cast<float>(vy), static_cast<float>(h), wr, wg, wb, 0, static_cast<float>(GROUND_TEXTURE_NONE), 0.0f, 0.0f};
					};
					mesh.vertices.insert(mesh.vertices.end(), {corner(ax, ay, ta), corner(bx, by, tb), corner(ax, ay, std::min(ha, ta)), corner(bx, by, std::min(hb, tb))});
				};
				wall(x, y, hn, tn, x + 1, y, hw, tw, 0, -1);
				wall(x + 1, y, hw, tw, x + 1, y + 1, hs, ts, 1, 0);
				wall(x + 1, y + 1, hs, ts, x, y + 1, he, te, 0, 1);
				wall(x, y + 1, he, te, x, y, hn, tn, -1, 0);
				hn = tn;
				hw = tw;
				he = te;
				hs = ts;
			}
			/* A tunnel portal stands on flat ground at the foot of the slope; its wing walls close the sides. */
			if (pack != nullptr && !pack->IsEmpty() && HasTunnelModel(tile, *pack)) {
				hn = hw = he = hs = std::min({hn, hw, he, hs});
			}

			if (!shore) {
				/* Foundations and tunnels may have changed the heights. */
				fn = hn; fw = hw; fe = he; fs = hs;
			}
			TerrainVertex vn = vertex(x, y, fn);
			TerrainVertex vw = vertex(x + 1, y, fw);
			TerrainVertex ve = vertex(x, y + 1, fe);
			TerrainVertex vs = vertex(x + 1, y + 1, fs);

			if (pack_ground && ground.texture == GROUND_TEXTURE_WATER && IsTileType(tile, TileType::Water) && GetWaterClass(tile) == WaterClass::Sea && std::max({hn, hw, he, hs}) == 0) {
				/* The sea is shallow in corners where it meets something other than sea or coast, to go with the water over the coasts next to it. */
				static constexpr uint8_t SHALLOW = static_cast<uint8_t>((1.0f - 0.1f) * 64.0f);
				if (!IsSeaCorner(x, y)) vn.depth = SHALLOW;
				if (!IsSeaCorner(x + 1, y)) vw.depth = SHALLOW;
				if (!IsSeaCorner(x, y + 1)) ve.depth = SHALLOW;
				if (!IsSeaCorner(x + 1, y + 1)) vs.depth = SHALLOW;
			}

			/* Split the tile along the diagonal with the smallest height difference,
			 * so a single raised or lowered corner gives one flat and one sloped half. */
			bool split_ns = std::abs(fn - fs) <= std::abs(fw - fe);
			if (split_ns) {
				mesh.vertices.insert(mesh.vertices.end(), {vw, vn, vs, ve});
			} else {
				mesh.vertices.insert(mesh.vertices.end(), {vn, ve, vw, vs});
			}

			if (mark != TileMark::None) AddTileMark(mark, x, y, {static_cast<float>(fn), static_cast<float>(fw), static_cast<float>(fe), static_cast<float>(fs)}, split_ns, pack, mesh);

			if (shore && std::min({fn, fw, fe, fs}) < 0 && (mesh.vertices.size() + mesh.water.size()) / 4 < MAX_CHUNK_QUADS) {
				/* The water over the land, at sea level. Each corner knows how deep the land is below it;
				 * the quad is split like the land, so that the depth in between is right as well. */
				uint8_t lit = static_cast<uint8_t>(std::clamp(255.0f * (AMBIENT + DIFFUSE * SUN_Z) * 0.5f, 0.0f, 255.0f));
				auto surface = [&](uint vx, uint vy, float h) {
					uint8_t depth = static_cast<uint8_t>(std::clamp((h + 1.0f) * 64.0f, 0.0f, 255.0f));
					return TerrainVertex{static_cast<float>(vx), static_cast<float>(vy), 0.0f, lit, lit, lit, depth, static_cast<float>(GROUND_TEXTURE_SHALLOWS), static_cast<float>(vx - x), static_cast<float>(vy - y)};
				};
				TerrainVertex wn = surface(x, y, fn), ww = surface(x + 1, y, fw), we = surface(x, y + 1, fe), ws = surface(x + 1, y + 1, fs);
				if (split_ns) {
					mesh.water.insert(mesh.water.end(), {ww, wn, ws, we});
				} else {
					mesh.water.insert(mesh.water.end(), {wn, we, ww, ws});
				}
			}

			bool models = pack != nullptr && !pack->IsEmpty();
			if (collect && models && IsTileType(tile, TileType::Railway) && IsPlainRail(tile)) {
				AddTrackModels(tile, x, y, {hn, hw, he, hs}, *pack, mesh);
				AddRailFenceModels(tile, x, y, {hn, hw, he, hs}, *pack, mesh);
				AddWireModels(tile, x, y, {hn, hw, he, hs}, *pack, mesh);
			}
			if (collect && models && IsTileType(tile, TileType::Road) && IsNormalRoad(tile)) {
				AddRoadModels(tile, x, y, {hn, hw, he, hs}, *pack, mesh);
				RoadBits wires = GetRoadWireBits(Tile(tile), *pack);
				if (wires.Any()) AddRoadWireModels(wires, x, y, {hn, hw, he, hs}, *pack, mesh);
			}
			if (!collect) {
				if (IsTileType(tile, TileType::Trees) && !(models && AddTreeModels(tile, x, y, *pack, mesh))) AddTrees(tile, x, y, atlas, mesh);
				if (IsTileType(tile, TileType::Clear)) {
					if (IsClearGround(tile, ClearGround::Fields)) AddFences(tile, x, y, atlas, pack, mesh);
					if (models && !ground.foundation) AddClearModel(tile, x, y, {hn, hw, he, hs}, *pack, mesh);
				}
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
	data = reinterpret_cast<const uint8_t *>(mesh.water.data());
	for (size_t i = 0; i < mesh.water.size() * sizeof(TerrainVertex); i++) {
		hash = (hash ^ data[i]) * 0x100000001b3ULL;
	}
	data = reinterpret_cast<const uint8_t *>(mesh.sprites.data());
	for (size_t i = 0; i < mesh.sprites.size() * sizeof(SpriteVertex); i++) {
		hash = (hash ^ data[i]) * 0x100000001b3ULL;
	}
	for (const ModelInstance &instance : mesh.models) {
		hash = (hash ^ instance.model) * 0x100000001b3ULL;
		for (float f : instance.transform.m) hash = (hash ^ std::bit_cast<uint32_t>(f)) * 0x100000001b3ULL;
		for (float f : instance.tint) hash = (hash ^ std::bit_cast<uint32_t>(f)) * 0x100000001b3ULL;
	}
	mesh.hash = hash;
}
