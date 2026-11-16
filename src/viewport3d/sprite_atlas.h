/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file sprite_atlas.h Sprites of the 2D game collected in one texture, for the 3D viewport. */

#ifndef VIEWPORT3D_SPRITE_ATLAS_H
#define VIEWPORT3D_SPRITE_ATLAS_H

#include "tile_sprites.h"
#include "../gfx_type.h"
#include "../zoom_type.h"

/** Where a sprite is in the atlas, and how big it is in the 2D game. */
struct AtlasSprite {
	bool valid = false; ///< Whether the sprite is in the atlas.
	float u0 = 0, v0 = 0; ///< Texture coordinate of the top left corner.
	float u1 = 0, v1 = 0; ///< Texture coordinate of the bottom right corner.
	float left = 0; ///< Position of the left edge relative to the reference point, in pixels at normal zoom.
	float top = 0; ///< Position of the top edge relative to the reference point, in pixels at normal zoom; down is positive.
	float width = 0; ///< Width in pixels at normal zoom.
	float height = 0; ///< Height in pixels at normal zoom.
};

/**
 * One big image that sprites are added to the first time they are needed.
 * Must be used from the thread that draws the windows.
 */
class SpriteAtlas {
public:
	static constexpr uint WIDTH = 4096; ///< Width of the atlas in pixels.
	static constexpr uint HEIGHT = 4096; ///< Height of the atlas in pixels.

	SpriteAtlas();

	const AtlasSprite &Get(SpriteID sprite, PaletteID pal);
	const AtlasSprite &Get(std::span<const SpritePart> sprites);

	/** Pixels of the atlas, top row first, in the format of 32bpp blitters. */
	std::span<const uint32_t> GetPixels() const { return this->pixels; }

	/**
	 * Get the rows that changed since the last call, and consider them seen.
	 * @param[out] top First changed row.
	 * @param[out] bottom One past the last changed row.
	 * @return Whether any row changed.
	 */
	bool TakeDirtyRows(uint &top, uint &bottom)
	{
		top = this->dirty_top;
		bottom = this->dirty_bottom;
		this->dirty_top = HEIGHT;
		this->dirty_bottom = 0;
		return top < bottom;
	}

private:
	ZoomLevel zoom; ///< Zoom level of the sprites in the atlas.
	std::vector<uint32_t> pixels; ///< The image.
	std::map<std::vector<SpritePart>, AtlasSprite> sprites; ///< The sprites in the atlas, and the ones that did not fit.
	uint shelf_x = 0; ///< Where the next sprite goes in the current row of sprites.
	uint shelf_y = 0; ///< Top of the current row of sprites.
	uint shelf_height = 0; ///< Height of the tallest sprite in the current row.
	uint dirty_top = HEIGHT; ///< First row that changed.
	uint dirty_bottom = 0; ///< One past the last row that changed.
};

#endif /* VIEWPORT3D_SPRITE_ATLAS_H */
