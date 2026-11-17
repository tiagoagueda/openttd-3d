/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file ground_textures.h Ground textures for the 3D viewport, made from the ground sprites of the 2D game. */

#ifndef VIEWPORT3D_GROUND_TEXTURES_H
#define VIEWPORT3D_GROUND_TEXTURES_H

#include "tile_sprites.h"
#include "model_pack.h"
#include "../zoom_type.h"

/**
 * The ground textures of the 3D viewport that are always there.
 * Each ground texture is one layer of a texture array, and these are the first layers.
 */
enum GroundTexture : uint8_t {
	GROUND_TEXTURE_NONE = 0, ///< Plain white, for tiles that are drawn in a flat colour.
	GROUND_TEXTURE_GRASS = 1, ///< Bare land to full grass; 4 densities.
	GROUND_TEXTURE_ROUGH = GROUND_TEXTURE_GRASS + 4, ///< Rough land; 5 variants.
	GROUND_TEXTURE_ROCKS = GROUND_TEXTURE_ROUGH + 5, ///< Rocks.
	GROUND_TEXTURE_FIELDS = GROUND_TEXTURE_ROCKS + 1, ///< Farm fields; 9 kinds.
	GROUND_TEXTURE_SNOW_DESERT = GROUND_TEXTURE_FIELDS + 9, ///< Snow or desert; 4 densities.
	GROUND_TEXTURE_SNOWY_ROCKS = GROUND_TEXTURE_SNOW_DESERT + 4, ///< Rocks under snow; 4 densities.
	GROUND_TEXTURE_DESERT_ROCKS = GROUND_TEXTURE_SNOWY_ROCKS + 4, ///< Rocks in the desert; 4 densities.
	GROUND_TEXTURE_WATER = GROUND_TEXTURE_DESERT_ROCKS + 4, ///< Water.
	GROUND_TEXTURE_SHORE, ///< Coasts; 18 shapes, in the order of the shore sprites.
	GROUND_TEXTURE_END = GROUND_TEXTURE_SHORE + 18, ///< Number of ground textures that are always there.

	/* With ground textures from the model package the shape of a coast comes from the terrain, not from sprites. */
	GROUND_TEXTURE_BEACH = GROUND_TEXTURE_SHORE, ///< The land of a coast: sand near the water, grass higher up.
	GROUND_TEXTURE_SHALLOWS, ///< The water over the land of a coast; see-through where it is shallow.
	GROUND_TEXTURE_TERRAIN_END = GROUND_TEXTURE_BEACH + 1, ///< Number of ground textures that come from the model package.
};

/**
 * The ground textures of the 3D viewport.
 * Besides the fixed ones of #GroundTexture, textures are made on demand for whatever
 * sprites the 2D game draws on the ground of a tile, such as tracks and roads.
 * Must be used from the thread that draws the windows.
 */
class GroundTextures {
public:
	/** A texture that still has to be sent to the GPU. */
	struct Pending {
		uint layer; ///< The layer of the texture array.
		std::vector<uint32_t> pixels; ///< The pixels, in the format of 32bpp blitters.
	};

	GroundTextures(uint max_layers);

	/** Width and height of each texture in pixels; a power of two. */
	uint GetSize() const { return this->size; }

	uint Get(std::span<const SpritePart> sprites, const std::array<uint8_t, 4> &heights);

	std::vector<Pending> MakeTerrainTextures(const ModelPack &pack);

	/** Whether there is no room left for more textures. */
	bool IsFull() const { return this->next_layer >= this->max_layers; }

	/** The textures that were made since the last call. */
	std::vector<Pending> TakePending()
	{
		std::vector<Pending> result;
		result.swap(this->pending);
		return result;
	}

private:
	using Key = std::pair<std::array<uint8_t, 4>, std::vector<SpritePart>>;

	bool Render(std::span<const SpritePart> sprites, const std::array<uint8_t, 4> &heights, std::vector<uint32_t> &texture);
	bool Build(uint layer, std::span<const SpritePart> sprites, const std::array<uint8_t, 4> &heights);

	ZoomLevel zoom; ///< Zoom level of the sprites to make the textures from.
	uint size; ///< Width and height of each texture.
	uint max_layers; ///< Number of layers the texture array has room for.
	uint next_layer = GROUND_TEXTURE_END; ///< The next free layer.
	std::map<Key, uint> layers; ///< The textures made on demand.
	std::vector<Pending> pending; ///< Textures not sent to the GPU yet.
};

#endif /* VIEWPORT3D_GROUND_TEXTURES_H */
