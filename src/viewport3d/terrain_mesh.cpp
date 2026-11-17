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
#include "../road_map.h"
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
	bool foundation = false; ///< Whether the tile stands on a foundation, so its top is flat at the height of its highest corner.
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
			if (!IsHouseCompleted(tile)) return false;
			/* Buildings of several tiles have a model for their northern tile. */
			HouseID house = GetHouseType(tile);
			TileIndex north = tile + GetHouseNorthPart(house);
			const HouseSpec *hs = HouseSpec::Get(house);
			const ModelPack::Entry *entry = pack.Find(ModelKind::House, hs->grf_prop, house);
			if (entry == nullptr) return false;
			if (north == tile) {
				uint size_x = hs->building_flags.Any(BUILDING_2_TILES_X) ? 2 : 1;
				uint size_y = hs->building_flags.Any(BUILDING_2_TILES_Y) ? 2 : 1;
				uint hash = TileHash2Bit(TileX(tile), TileY(tile));
				Colours colour = hs->grf_prop.HasGrfFile() ? hs->random_colour[hash] : HOUSE_COLOURS[hash];
				PlaceModel(*entry, tile, size_x, size_y, DiagDirection::SW, ModelPack::GetTint(colour), models);
			}
			return true;
		}

		case TileType::Industry: {
			if (!IsIndustryCompleted(tile)) return false;
			IndustryGfx gfx = GetIndustryGfx(tile);
			const ModelPack::Entry *entry = pack.Find(ModelKind::IndustryTile, GetIndustryTileSpec(gfx)->grf_prop, gfx);
			if (entry == nullptr) return false;
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
			}
			if (entry == nullptr) return false;
			PlaceModel(*entry, tile, 1, 1, front, GetOwnerTint(IsBuoy(tile) ? OWNER_NONE : GetTileOwner(tile)), models);
			return true;
		}

		case TileType::Railway: {
			if (!IsRailDepot(tile)) return false;
			const ModelPack::Entry *entry = pack.Find(ModelKind::RailDepot, 0, 0);
			if (entry == nullptr) return false;
			PlaceModel(*entry, tile, 1, 1, GetRailDepotDirection(tile), GetOwnerTint(GetTileOwner(tile)), models);
			return true;
		}

		case TileType::Road: {
			if (!IsRoadDepot(tile)) return false;
			const ModelPack::Entry *entry = pack.Find(ModelKind::RoadDepot, 0, 0);
			if (entry == nullptr) return false;
			PlaceModel(*entry, tile, 1, 1, GetRoadDepotDirection(tile), GetOwnerTint(GetTileOwner(tile)), models);
			return true;
		}

		case TileType::Water: {
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

	int tile_x = x * TILE_SIZE;
	int tile_y = y * TILE_SIZE;
	auto [slope, tile_z] = GetTilePixelSlope(tile);

	/* Foundations under half a tile leave the other half sloped; those keep the foundation sprite. */
	bool flat_foundation = !IsHalftileSlope(slope) && std::ranges::any_of(sprites.groups, &TileSpriteGroup::foundation);
	int foundation_z = GetTileMaxPixelZ(tile);
	static std::vector<SpritePart> ground;
	ground = sprites.ground;

	uint index = 0;
	for (const TileSpriteGroup &group : sprites.groups) {
		if (group.count == 0) continue;
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
	if (ground.empty()) {
		result = ground_only ? GetTileGround(Tile(tile), x, y) : TileGround{GROUND_TEXTURE_GRASS, COLOUR_WHITE};
	} else {
		uint texture = textures.Get(ground, flat_foundation ? std::array<uint8_t, 4>{} : heights);
		result = texture == GROUND_TEXTURE_NONE ? TileGround{GROUND_TEXTURE_NONE, TerrainColour{0x80, 0x80, 0x80}} : TileGround{texture, COLOUR_WHITE, true};
	}
	result.foundation = flat_foundation;
	return result;
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
	static constexpr float SLOPE_SCALE = static_cast<float>(TILE_HEIGHT) / TILE_SIZE;

	mesh.vertices.clear();
	mesh.sprites.clear();
	mesh.models.clear();
	mesh.parts.clear();
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

			/* The selection of a build tool is drawn on the ground, so the ground of selected tiles comes from the 2D game too. */
			bool selected = !collect && HasTileSelection(TileXY(x, y));

			TileGround ground;
			if (collect || selected) {
				int low = std::min({hn, hw, he, hs});
				ground = AddTileSprites(x, y, {static_cast<uint8_t>(hn - low), static_cast<uint8_t>(hw - low), static_cast<uint8_t>(he - low), static_cast<uint8_t>(hs - low)}, textures, atlas, pack, mesh, selected);
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
			if (ground.foundation) {
				/* Walls from the flat top of the foundation down to the ground around it. */
				static constexpr TerrainColour STONE{0xa8, 0xa0, 0x90};
				int top = std::max({hn, hw, he, hs});
				auto wall = [&](uint ax, uint ay, int ha, uint bx, uint by, int hb, float nx, float ny) {
					if ((ha == top && hb == top) || mesh.vertices.size() / 4 >= MAX_CHUNK_QUADS - 1) return;
					float wall_light = (AMBIENT + DIFFUSE * std::max(0.0f, nx * SUN_X + ny * SUN_Y)) * 0.5f;
					uint8_t wr = static_cast<uint8_t>(std::clamp(STONE.r * wall_light, 0.0f, 255.0f));
					uint8_t wg = static_cast<uint8_t>(std::clamp(STONE.g * wall_light, 0.0f, 255.0f));
					uint8_t wb = static_cast<uint8_t>(std::clamp(STONE.b * wall_light, 0.0f, 255.0f));
					auto corner = [&](uint vx, uint vy, int h) {
						return TerrainVertex{static_cast<float>(vx), static_cast<float>(vy), static_cast<float>(h), wr, wg, wb, 0, static_cast<float>(GROUND_TEXTURE_NONE)};
					};
					mesh.vertices.insert(mesh.vertices.end(), {corner(ax, ay, top), corner(bx, by, top), corner(ax, ay, ha), corner(bx, by, hb)});
				};
				wall(x, y, hn, x + 1, y, hw, 0, -1);
				wall(x + 1, y, hw, x + 1, y + 1, hs, 1, 0);
				wall(x + 1, y + 1, hs, x, y + 1, he, 0, 1);
				wall(x, y + 1, he, x, y, hn, -1, 0);
				hn = hw = he = hs = top;
			}

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
				bool models = pack != nullptr && !pack->IsEmpty();
				if (IsTileType(tile, TileType::Trees) && !(models && AddTreeModels(tile, x, y, *pack, mesh))) AddTrees(tile, x, y, atlas, mesh);
				if (IsTileType(tile, TileType::Clear)) {
					if (IsClearGround(tile, ClearGround::Fields)) AddFences(tile, x, y, atlas, mesh);
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
