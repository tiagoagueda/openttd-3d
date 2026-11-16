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
	uint8_t r, g, b, a; ///< Colour with the lighting already applied.
};
static_assert(sizeof(TerrainVertex) == 16);

/**
 * Geometry of one chunk of terrain.
 * Every tile is a quad of four vertices, of which vertex 1 and 2 are on the diagonal
 * along which the tile is split in two triangles. Index buffers are therefore the
 * same for every chunk: (0, 1, 2), (2, 1, 3) for each quad.
 */
struct TerrainChunkMesh {
	std::vector<TerrainVertex> vertices; ///< Four vertices per tile.
	uint min_height = 0; ///< Lowest height level in the chunk.
	uint max_height = 0; ///< Highest height level in the chunk.
	uint64_t hash = 0; ///< Hash over the vertices, to detect whether a rebuild changed anything.
};

void BuildTerrainChunkMesh(uint chunk_x, uint chunk_y, TerrainChunkMesh &mesh);

#endif /* VIEWPORT3D_TERRAIN_MESH_H */
