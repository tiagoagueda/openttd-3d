/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file ground_textures.h Ground textures for the 3D viewport, made from the ground sprites of the 2D game. */

#ifndef VIEWPORT3D_GROUND_TEXTURES_H
#define VIEWPORT3D_GROUND_TEXTURES_H

/**
 * The ground textures of the 3D viewport.
 * Each is one layer of a texture array; a tile names its layer in the alpha channel of its vertices.
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
	GROUND_TEXTURE_END, ///< Number of ground textures.
};

/** The pixels of all ground textures. */
struct GroundTextureSet {
	uint size = 0; ///< Width and height of each texture in pixels; a power of two.
	std::vector<std::vector<uint32_t>> layers; ///< Pixels per GroundTexture, in the format of 32bpp blitters.
};

GroundTextureSet BuildGroundTextures();

#endif /* VIEWPORT3D_GROUND_TEXTURES_H */
