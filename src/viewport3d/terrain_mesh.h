/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file terrain_mesh.h Building terrain geometry for the 3D viewport from the map array. */

#ifndef VIEWPORT3D_TERRAIN_MESH_H
#define VIEWPORT3D_TERRAIN_MESH_H

/** Number of tiles along one side of a terrain chunk. */
static constexpr uint TERRAIN_CHUNK_SIZE = 64;

/** One corner of a tile, as sent to the GPU. */
struct TerrainVertex {
	float x; ///< Position along the x axis in tiles.
	float y; ///< Position along the y axis in tiles.
	float z; ///< Height in height levels.
	uint8_t r, g, b; ///< Colour to multiply the texture with, including the lighting, at half brightness.
	uint8_t unused; ///< Not used.
	float texture; ///< The layer of the tile's ground texture.
};
static_assert(sizeof(TerrainVertex) == 20);

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

/**
 * Geometry of one chunk of terrain.
 * Every tile is a quad of four vertices, of which vertex 1 and 2 are on the diagonal
 * along which the tile is split in two triangles. Index buffers are therefore the
 * same for every chunk: (0, 1, 2), (2, 1, 3) for each quad.
 */
struct TerrainChunkMesh {
	std::vector<TerrainVertex> vertices; ///< Four vertices per tile.
	std::vector<SpriteVertex> sprites; ///< Four vertices per sprite standing on the terrain, in the same order as for tiles.
	uint min_height = 0; ///< Lowest height level in the chunk.
	uint max_height = 0; ///< Highest height level in the chunk.
	uint64_t hash = 0; ///< Hash over the vertices, to detect whether a rebuild changed anything.
};

class GroundTextures;
class SpriteAtlas;
struct AtlasSprite;

void AddUprightSprite(const AtlasSprite &sprite, float x, float y, float z, float nearer, std::vector<SpriteVertex> &vertices);

void BuildTerrainChunkMesh(uint chunk_x, uint chunk_y, GroundTextures &textures, SpriteAtlas &atlas, TerrainChunkMesh &mesh);

#endif /* VIEWPORT3D_TERRAIN_MESH_H */
