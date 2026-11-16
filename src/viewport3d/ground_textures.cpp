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
#include "../slope_func.h"
#include "../zoom_func.h"
#include "../table/sprites.h"

#include "../safeguards.h"

/**
 * Turn the sprites of a ground tile into a square texture.
 *
 * In the 2D game a flat tile is a diamond of 64 by 31 pixels (at normal zoom). The texture is that
 * diamond pulled straight: the texture's u axis runs along the tile's x axis and v along its y axis.
 * On a slope the diamond is bent; the heights of the corners tell how.
 *
 * @param layer The layer of the texture array to make.
 * @param sprites The ground sprites, in the order the 2D game draws them, relative to where it draws the northern corner of the tile.
 * @param heights Heights of the northern, western, eastern and southern corner, in height levels above the lowest.
 * @return Whether the sprites could be used.
 */
bool GroundTextures::Build(uint layer, std::span<const SpritePart> sprites, const std::array<uint8_t, 4> &heights)
{
	const ZoomLevel zoom = this->zoom;
	const uint size = this->size;

	/* Size of the sprites compared to normal zoom, at which the diamond is 64 pixels wide and 31 high. */
	const int scale = UnScaleByZoom(4, zoom);
	const int diamond_width = 64 * scale;
	/* Raised corners are drawn up to two height levels of 8 pixels above the tile's origin. */
	const int margin = 16 * scale;
	const int diamond_height = 31 * scale + margin + scale;

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
		for (const SpritePart &part : sprites) {
			/* Sprites that make what is below them see-through cannot be drawn onto nothing. */
			if (HasBit(part.sprite, PALETTE_MODIFIER_TRANSPARENT)) continue;
			/* Clipping is relative to the position of the sprite, here in pixels at the most detailed zoom level. */
			SubSprite sub{part.clip_left * static_cast<int>(ZOOM_BASE), part.clip_top * static_cast<int>(ZOOM_BASE), (part.clip_right + 1) * static_cast<int>(ZOOM_BASE) - 1, (part.clip_bottom + 1) * static_cast<int>(ZOOM_BASE) - 1};
			DrawSprite(part.sprite, part.pal, diamond_width / 2 - scale + UnScaleByZoom(part.x, zoom), margin + UnScaleByZoom(part.y, zoom), part.clipped ? &sub : nullptr, zoom);
		}
	}

	const float hn = heights[0], hw = heights[1], he = heights[2], hs = heights[3];
	/* Height of the surface in height levels; the tile is split in two triangles the same way as in the terrain mesh. */
	auto surface = [&](float u, float v) {
		if (std::abs(hn - hs) <= std::abs(hw - he)) {
			return u >= v ? hn + (hw - hn) * (u - v) + (hs - hn) * v : hn + (he - hn) * (v - u) + (hs - hn) * u;
		}
		return u + v <= 1.0f ? hn + (hw - hn) * u + (he - hn) * v : hs + (hw - hs) * (1.0f - v) + (he - hs) * (1.0f - u);
	};
	/* Where a point of the tile is in the image. */
	auto project = [&](float u, float v, float &fx, float &fy) {
		fx = diamond_width * 0.5f + (v - u) * diamond_width * 0.5f;
		fy = margin + (u + v) * 15.5f * scale - surface(u, v) * 8 * scale;
	};
	float centre_x, centre_y;
	project(0.5f, 0.5f, centre_x, centre_y);

	auto sample = [&](float u, float v, uint &b, uint &g, uint &r) {
		float fx, fy;
		project(u, v, fx, fy);
		/* The very corners of the diamond are not in the sprite; move towards the centre until there is a pixel. */
		for (int step = 0; step <= 16; step++) {
			float t = step / 16.0f;
			int x = Clamp(static_cast<int>(fx + (centre_x - fx) * t), 0, diamond_width - 1);
			int y = Clamp(static_cast<int>(fy + (centre_y - fy) * t), 0, diamond_height - 1);
			uint32_t pixel = diamond[static_cast<size_t>(y) * diamond_width + x];
			if ((pixel >> 24) == 0) continue;
			b += pixel & 0xFF;
			g += (pixel >> 8) & 0xFF;
			r += (pixel >> 16) & 0xFF;
			return true;
		}
		return false;
	};

	std::vector<uint32_t> texture(static_cast<size_t>(size) * size);
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
	this->pending.push_back({layer, std::move(texture)});
	return true;
}

/**
 * Get the texture for a tile with certain sprites on its ground, making it when it does not exist yet.
 * @param sprites The ground sprites, in the order the 2D game draws them, relative to where it draws the northern corner of the tile.
 * @param heights Heights of the northern, western, eastern and southern corner, in height levels above the lowest.
 * @return The layer of the texture, or GROUND_TEXTURE_NONE when there is no room or nothing to see.
 */
uint GroundTextures::Get(std::span<const SpritePart> sprites, const std::array<uint8_t, 4> &heights)
{
	auto [it, inserted] = this->layers.try_emplace(Key{heights, {sprites.begin(), sprites.end()}}, GROUND_TEXTURE_NONE);
	if (inserted && this->next_layer < this->max_layers && this->Build(this->next_layer, sprites, heights)) {
		it->second = this->next_layer++;
	}
	return it->second;
}

/**
 * Make the fixed ground textures from the sprites that are loaded right now.
 * @param max_layers Number of layers the texture array has room for.
 */
GroundTextures::GroundTextures(uint max_layers) : max_layers(max_layers)
{
	/* Use the most detailed sprites the game is allowed to use; base sets without them get scaled up by the sprite loader. */
	this->zoom = std::min(_settings_client.gui.zoom_min, ZoomLevel::Normal);
	/* At the most detailed zoom level a tile is 256 pixels wide, but textures of that size for every kind of tile take too much memory. */
	this->size = std::min(UnScaleByZoom(256, this->zoom), 128);

	this->pending.push_back({GROUND_TEXTURE_NONE, std::vector<uint32_t>(static_cast<size_t>(this->size) * this->size, 0xFFFFFFFF)});
	auto build = [&](uint layer, std::initializer_list<SpriteID> sprites, const std::array<uint8_t, 4> &heights = {}) {
		std::vector<SpritePart> parts;
		for (SpriteID sprite : sprites) parts.push_back({.sprite = sprite, .pal = PAL_NONE});
		/* Textures whose sprites cannot be used are plain white. */
		if (!this->Build(layer, parts, heights)) this->pending.push_back({layer, this->pending.front().pixels});
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

	/* The slope that each shore sprite is drawn for; the reverse of the table in DrawShoreTile(). */
	static constexpr Slope SHORE_SLOPES[] = {
		SLOPE_STEEP_S, SLOPE_W, SLOPE_S, SLOPE_SW, SLOPE_E, SLOPE_STEEP_W, SLOPE_SE, SLOPE_WSE, SLOPE_N,
		SLOPE_NW, SLOPE_STEEP_N, SLOPE_NWS, SLOPE_NE, SLOPE_ENW, SLOPE_SEN, SLOPE_STEEP_E, SLOPE_EW, SLOPE_NS,
	};
	for (uint i = 0; i < std::size(SHORE_SLOPES); i++) {
		Slope slope = SHORE_SLOPES[i];
		auto height = [&](Slope bit, Corner corner) -> uint8_t {
			if (IsSteepSlope(slope) && GetHighestSlopeCorner(slope) == corner) return 2;
			return (slope & bit) != 0 ? 1 : 0;
		};
		build(GROUND_TEXTURE_SHORE + i, {SPR_SHORE_BASE + i}, {height(SLOPE_N, Corner::N), height(SLOPE_W, Corner::W), height(SLOPE_E, Corner::E), height(SLOPE_S, Corner::S)});
	}

}
