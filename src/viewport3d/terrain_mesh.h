/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file terrain_mesh.h Building terrain geometry for the 3D viewport from the map array. */

#ifndef VIEWPORT3D_TERRAIN_MESH_H
#define VIEWPORT3D_TERRAIN_MESH_H

#include <map>

#include "model3d.h"
#include "../tile_type.h"

/** Number of tiles along one side of a terrain chunk. */
static constexpr uint TERRAIN_CHUNK_SIZE = 64;

/** One corner of a tile, as sent to the GPU. */
struct TerrainVertex {
	float x; ///< Position along the x axis in tiles.
	float y; ///< Position along the y axis in tiles.
	float z; ///< Height in height levels.
	uint8_t r, g, b; ///< Colour to multiply the texture with, including the lighting, at half brightness.
	uint8_t depth; ///< For water: the height of the ground below it, as (height levels + 1) * 64; 0 is deep water. For land with a mixed texture (see #GroundMix): the layer of the texture of the model package under it.
	float texture; ///< The layer of the tile's ground texture.
	float u; ///< Position within the tile along the x axis, 0 to 1.
	float v; ///< Position within the tile along the y axis, 0 to 1.
};
static_assert(sizeof(TerrainVertex) == 28);

/**
 * One corner of a sprite that stands on the map.
 * A sprite is a quad; the offset moves each corner away from the position,
 * sideways as seen from the camera and straight up, so the quad always faces the camera.
 */
struct SpriteVertex {
	float x; ///< Position along the x axis in tiles.
	float y; ///< Position along the y axis in tiles.
	float z; ///< Height in height levels.
	float right; ///< Offset to the right of the camera, in pixels of the 2D game at normal zoom.
	float up; ///< Offset upwards, in pixels of the 2D game at normal zoom.
	float u; ///< Horizontal texture coordinate in the sprite atlas.
	float v; ///< Vertical texture coordinate in the sprite atlas.
	float nearer; ///< How much nearer to the camera the sprite counts as being when deciding what is in front, in pixels of height of the 2D game.
};
static_assert(sizeof(SpriteVertex) == 32);

/** Most quads, of terrain or of sprites, that a chunk can have; limited by the 16 bit indices. */
static constexpr uint MAX_CHUNK_QUADS = 16384;

/** A part of a building model that moves, placed on the map. */
struct AnimatedPart {
	Matrix4 transform; ///< From the frame of the building's model to world units.
	std::array<float, 4> tint; ///< Colour of the building.
	uint model; ///< Model of the part.
	std::array<float, 3> at; ///< Where the part turns around, in the frame of the building's model.
	std::array<float, 3> axis; ///< Around which axis it turns, in that frame.
	float spin; ///< Radians per second it keeps turning by.
	float swing; ///< Radians it swings to either side.
	float period; ///< Seconds for one swing back and forth.
	float lift; ///< How far it moves along its axis and back, once per period.
	float phase; ///< Seconds the part is ahead of others, so that neighbours do not all move together.

	bool operator==(const AnimatedPart &other) const = default;
	ModelInstance Place(float seconds) const;
};

/**
 * Geometry of one chunk of terrain.
 * Every tile is a quad of four vertices, of which vertex 1 and 2 are on the diagonal
 * along which the tile is split in two triangles. Index buffers are therefore the
 * same for every chunk: (0, 1, 2), (2, 1, 3) for each quad.
 */
struct TerrainChunkMesh {
	std::vector<TerrainVertex> vertices; ///< Four vertices per tile.
	std::vector<TerrainVertex> water; ///< Four vertices per tile of a coast, for the water over its land; see-through, so drawn after everything else.
	std::vector<SpriteVertex> sprites; ///< Four vertices per sprite standing on the terrain, in the same order as for tiles.
	uint min_height = 0; ///< Lowest height level in the chunk.
	uint max_height = 0; ///< Highest height level in the chunk.
	std::vector<ModelInstance> models; ///< 3D models standing on the terrain, in place of sprites.
	std::vector<AnimatedPart> parts; ///< Parts of those models that move.
	std::vector<TileIndex> locks; ///< Middle parts of locks whose gates and water move, see AddLockMovingParts().
	std::map<TileIndex, std::vector<ModelInstance>> live; ///< Models that change often, like signals, by tile; drawn anew for every image, so a change does not rebuild the chunk.
	uint64_t hash = 0; ///< Hash over the vertices, to detect whether a rebuild changed anything.
};

class GroundTextures;
class ModelPack;
class SpriteAtlas;
struct AtlasSprite;

bool GetTileModels(TileIndex tile, const ModelPack &pack, std::vector<ModelInstance> &models, std::vector<AnimatedPart> &parts);
struct View3DPreview;
void AddPreviewModel(const View3DPreview &preview, TileIndex tile, uint track, const ModelPack &pack, std::vector<ModelInstance> &models);
struct View3DPicture;
bool AddPictureModels(const View3DPicture &picture, const ModelPack &pack, std::vector<ModelInstance> &models);
bool HasLiveTileModels(TileIndex tile);
bool GetTileChangeKey(TileIndex tile, uint64_t &key);
void GetLiveTileModels(TileIndex tile, const ModelPack &pack, std::vector<ModelInstance> &models);
void AddUprightSprite(const AtlasSprite &sprite, float x, float y, float z, float nearer, std::vector<SpriteVertex> &vertices);

void BuildTerrainChunkMesh(uint chunk_x, uint chunk_y, GroundTextures &textures, SpriteAtlas &atlas, const ModelPack *pack, TerrainChunkMesh &mesh);

#endif /* VIEWPORT3D_TERRAIN_MESH_H */
