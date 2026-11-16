/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file ground_textures.cpp Ground textures for the 3D viewport, made from the ground sprites of the 2D game. */

#include "../stdafx.h"
#include "ground_textures.h"
#include "../core/backup_type.hpp"
#include "../core/math_func.hpp"
#include "../gfx_func.h"
#include "../settings_type.h"
#include "../zoom_func.h"
#include "../table/sprites.h"

#include "../safeguards.h"

/**
 * Turn the sprite of a flat ground tile into a square texture.
 *
 * In the 2D game a flat tile is a diamond of 64 by 31 pixels (at normal zoom). The texture is that
 * diamond pulled straight: the texture's u axis runs along the tile's x axis and v along its y axis.
 *
 * @param sprites The ground sprite, and optionally sprites that the 2D game draws on top of it.
 * @param zoom Zoom level of the sprites to use.
 * @param size Width and height of the texture.
 * @param[out] texture The pixels of the texture.
 * @return Whether the sprites could be used.
 */
static bool BuildGroundTexture(std::initializer_list<SpriteID> sprites, ZoomLevel zoom, uint size, std::vector<uint32_t> &texture)
{
	/* Size of the sprites compared to normal zoom, at which the diamond is 64 pixels wide and 31 high. */
	const int scale = UnScaleByZoom(4, zoom);
	const int diamond_width = 64 * scale;
	const int diamond_height = 31 * scale;

	/* Let the blitter draw the sprites over each other into an image that covers exactly the diamond. */
	std::vector<uint32_t> diamond(static_cast<size_t>(diamond_width) * diamond_height, 0);
	{
		DrawPixelInfo dpi;
		dpi.dst_ptr = diamond.data();
		dpi.pitch = diamond_width;
		dpi.left = 0;
		dpi.top = 0;
		dpi.width = diamond_width;
		dpi.height = diamond_height;
		dpi.zoom = zoom;

		AutoRestoreBackup dpi_backup(_cur_dpi, &dpi);
		/* Keep the blitters with palette animation from writing to the animation buffer of the screen. */
		AutoRestoreBackup anim_backup(_screen_disable_anim, true);
		/* The tile's origin, its northern corner, is one (unzoomed) pixel left of the diamond's centre line. */
		for (SpriteID sprite : sprites) DrawSprite(sprite, PAL_NONE, diamond_width / 2 - scale, 0, nullptr, zoom);
	}

	auto sample = [&](float u, float v, uint &b, uint &g, uint &r) {
		float fx = diamond_width * 0.5f + (v - u) * diamond_width * 0.5f;
		float fy = (u + v) * diamond_height * 0.5f;
		/* The corners of the diamond are not in the sprite; move towards the centre until there is a pixel. */
		for (int step = 0; step <= 16; step++) {
			float t = step / 16.0f;
			int x = Clamp(static_cast<int>(fx + (diamond_width * 0.5f - fx) * t), 0, diamond_width - 1);
			int y = Clamp(static_cast<int>(fy + (diamond_height * 0.5f - fy) * t), 0, diamond_height - 1);
			uint32_t pixel = diamond[static_cast<size_t>(y) * diamond_width + x];
			if ((pixel >> 24) == 0) continue;
			b += pixel & 0xFF;
			g += (pixel >> 8) & 0xFF;
			r += (pixel >> 16) & 0xFF;
			return true;
		}
		return false;
	};

	texture.resize(static_cast<size_t>(size) * size);
	for (uint ty = 0; ty < size; ty++) {
		for (uint tx = 0; tx < size; tx++) {
			/* Average four samples per pixel; the diamond has fewer pixels than the texture along one diagonal and more along the other. */
			uint b = 0, g = 0, r = 0, count = 0;
			for (float dy : {0.25f, 0.75f}) {
				for (float dx : {0.25f, 0.75f}) {
					if (sample((tx + dx) / size, (ty + dy) / size, b, g, r)) count++;
				}
			}
			if (count == 0) return false;
			texture[static_cast<size_t>(ty) * size + tx] = 0xFF000000 | ((r / count) << 16) | ((g / count) << 8) | (b / count);
		}
	}
	return true;
}

/**
 * Build all ground textures from the sprites that are loaded right now.
 * Must be called from the thread that draws the windows.
 * @return The textures. Textures whose sprites cannot be used are plain white.
 */
GroundTextureSet BuildGroundTextures()
{
	GroundTextureSet set;

	/* Use the most detailed sprites the game is allowed to use; base sets without them get scaled up by the sprite loader. */
	ZoomLevel zoom = std::min(_settings_client.gui.zoom_min, ZoomLevel::Normal);
	set.size = UnScaleByZoom(256, zoom);

	set.layers.resize(GROUND_TEXTURE_END);
	auto build = [&](uint layer, std::initializer_list<SpriteID> sprites) {
		if (!BuildGroundTexture(sprites, zoom, set.size, set.layers[layer])) set.layers[layer].clear();
	};

	static constexpr SpriteID GRASS[] = {SPR_FLAT_BARE_LAND, SPR_FLAT_1_THIRD_GRASS_TILE, SPR_FLAT_2_THIRD_GRASS_TILE, SPR_FLAT_GRASS_TILE};
	static constexpr SpriteID ROUGH[] = {SPR_FLAT_ROUGH_LAND, SPR_FLAT_ROUGH_LAND_1, SPR_FLAT_ROUGH_LAND_2, SPR_FLAT_ROUGH_LAND_3, SPR_FLAT_ROUGH_LAND_4};
	static constexpr SpriteID SNOW_DESERT[] = {SPR_FLAT_1_QUART_SNOW_DESERT_TILE, SPR_FLAT_2_QUART_SNOW_DESERT_TILE, SPR_FLAT_3_QUART_SNOW_DESERT_TILE, SPR_FLAT_SNOW_DESERT_TILE};
	static constexpr SpriteID SNOWY_ROCKS[] = {SPR_OVERLAY_ROCKS_1_QUART_SNOW, SPR_OVERLAY_ROCKS_2_QUART_SNOW, SPR_OVERLAY_ROCKS_3_QUART_SNOW, SPR_OVERLAY_ROCKS_FULL_SNOW};

	for (uint i = 0; i < std::size(GRASS); i++) build(GROUND_TEXTURE_GRASS + i, {GRASS[i]});
	for (uint i = 0; i < std::size(ROUGH); i++) build(GROUND_TEXTURE_ROUGH + i, {ROUGH[i]});
	build(GROUND_TEXTURE_ROCKS, {SPR_FLAT_ROCKY_LAND_1});
	/* The sprites of the farm field kinds follow each other, one per slope. */
	for (uint i = 0; i < 9; i++) build(GROUND_TEXTURE_FIELDS + i, {SPR_FARMLAND_BARE + i * 19});
	for (uint i = 0; i < std::size(SNOW_DESERT); i++) {
		build(GROUND_TEXTURE_SNOW_DESERT + i, {SNOW_DESERT[i]});
		build(GROUND_TEXTURE_SNOWY_ROCKS + i, {SNOW_DESERT[i], SNOWY_ROCKS[i]});
		build(GROUND_TEXTURE_DESERT_ROCKS + i, {SNOW_DESERT[i], SPR_OVERLAY_ROCKS_BASE});
	}
	build(GROUND_TEXTURE_WATER, {SPR_FLAT_WATER_TILE});

	for (auto &layer : set.layers) {
		if (layer.empty()) layer.assign(static_cast<size_t>(set.size) * set.size, 0xFFFFFFFF);
	}
	return set;
}
