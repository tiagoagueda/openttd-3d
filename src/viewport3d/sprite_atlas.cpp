/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file sprite_atlas.cpp Sprites of the 2D game collected in one texture, for the 3D viewport. */

#include "../stdafx.h"
#include "sprite_atlas.h"
#include "../core/backup_type.hpp"
#include "../gfx_func.h"
#include "../settings_type.h"
#include "../spritecache.h"
#include "../zoom_func.h"
#include "../table/sprites.h"

#include "../safeguards.h"

SpriteAtlas::SpriteAtlas() : pixels(static_cast<size_t>(WIDTH) * HEIGHT, 0)
{
	/* Twice the normal size is a fair balance between sharpness and the room the sprites take. */
	this->zoom = std::clamp(_settings_client.gui.zoom_min, ZoomLevel::In2x, ZoomLevel::Normal);
}

/**
 * Get a sprite from the atlas, adding it when it is not there yet.
 * @param sprite The sprite.
 * @param pal The palette to recolour the sprite with, or PAL_NONE.
 * @return Where the sprite is, relative to the point the 2D game draws it at; not valid when the sprite could not be added.
 */
const AtlasSprite &SpriteAtlas::Get(SpriteID sprite, PaletteID pal)
{
	SpritePart part{.sprite = sprite, .pal = pal};
	return this->Get({&part, 1});
}

/**
 * Get a few sprites drawn over each other as one sprite from the atlas, adding it when it is not there yet.
 * @param parts The sprites, in the order the 2D game draws them, relative to a reference point.
 * @return Where the result is, relative to the reference point; not valid when it could not be added.
 */
const AtlasSprite &SpriteAtlas::Get(std::span<const SpritePart> parts)
{
	auto [it, inserted] = this->sprites.try_emplace(std::vector<SpritePart>(parts.begin(), parts.end()));
	AtlasSprite &entry = it->second;
	if (!inserted) return entry;

	/* Find the area that the sprites cover together. */
	int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
	for (const SpritePart &part : parts) {
		/* Sprites that make what is behind them see-through are left out. */
		if (HasBit(part.sprite, PALETTE_MODIFIER_TRANSPARENT)) continue;
		const Sprite *data = GetSprite(GB(part.sprite, 0, SPRITE_WIDTH), SpriteType::Normal);
		int x = UnScaleByZoom(part.x + data->x_offs, this->zoom);
		int y = UnScaleByZoom(part.y + data->y_offs, this->zoom);
		left = std::min(left, x);
		top = std::min(top, y);
		right = std::max(right, x + UnScaleByZoom(data->width, this->zoom));
		bottom = std::max(bottom, y + UnScaleByZoom(data->height, this->zoom));
	}
	if (left >= right || top >= bottom) return entry;
	int x_offs = left;
	int y_offs = top;
	uint width = right - left;
	uint height = bottom - top;

	/* Sprites go next to each other in rows, each with a border of one pixel around it. */
	uint cell_width = width + 2;
	uint cell_height = height + 2;
	if (cell_width > WIDTH) return entry;
	if (this->shelf_x + cell_width > WIDTH) {
		this->shelf_x = 0;
		this->shelf_y += this->shelf_height;
		this->shelf_height = 0;
	}
	if (this->shelf_y + cell_height > HEIGHT) return entry;

	/* Let the blitter draw the sprites into an empty image, so what stays transparent can be told from what is drawn. */
	std::vector<uint32_t> image(static_cast<size_t>(cell_width) * cell_height, 0);
	{
		DrawPixelInfo dpi;
		dpi.dst_ptr = image.data();
		dpi.pitch = cell_width;
		dpi.left = 0;
		dpi.top = 0;
		dpi.width = cell_width;
		dpi.height = cell_height;
		dpi.zoom = this->zoom;

		AutoRestoreBackup dpi_backup(_cur_dpi, &dpi);
		/* Keep the blitters with palette animation from writing to the animation buffer of the screen. */
		AutoRestoreBackup anim_backup(_screen_disable_anim, true);
		for (const SpritePart &part : parts) {
			if (HasBit(part.sprite, PALETTE_MODIFIER_TRANSPARENT)) continue;
			/* Clipping is relative to the position of the sprite, here in pixels at the most detailed zoom level. */
			SubSprite sub{part.clip_left * static_cast<int>(ZOOM_BASE), part.clip_top * static_cast<int>(ZOOM_BASE), (part.clip_right + 1) * static_cast<int>(ZOOM_BASE) - 1, (part.clip_bottom + 1) * static_cast<int>(ZOOM_BASE) - 1};
			DrawSprite(part.sprite, part.pal, 1 - x_offs + UnScaleByZoom(part.x, this->zoom), 1 - y_offs + UnScaleByZoom(part.y, this->zoom), part.clipped ? &sub : nullptr, this->zoom);
		}
	}

	/* Give transparent pixels the colour of a neighbour, so filtering the texture does not darken the outline of the sprite. */
	for (uint y = 0; y < cell_height; y++) {
		for (uint x = 0; x < cell_width; x++) {
			uint32_t pixel = image[static_cast<size_t>(y) * cell_width + x];
			if ((pixel >> 24) != 0) {
				pixel |= 0xFF000000;
			} else {
				pixel = 0;
				static constexpr int NEIGHBOURS[][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
				for (const auto &offset : NEIGHBOURS) {
					uint nx = x + offset[0];
					uint ny = y + offset[1];
					if (nx >= cell_width || ny >= cell_height) continue;
					uint32_t neighbour = image[static_cast<size_t>(ny) * cell_width + nx];
					if ((neighbour >> 24) == 0) continue;
					pixel = neighbour & 0x00FFFFFF;
					break;
				}
			}
			this->pixels[static_cast<size_t>(this->shelf_y + y) * WIDTH + this->shelf_x + x] = pixel;
		}
	}

	float scale = static_cast<float>(UnScaleByZoom(4, this->zoom));
	entry.valid = true;
	entry.u0 = static_cast<float>(this->shelf_x + 1) / WIDTH;
	entry.v0 = static_cast<float>(this->shelf_y + 1) / HEIGHT;
	entry.u1 = static_cast<float>(this->shelf_x + 1 + width) / WIDTH;
	entry.v1 = static_cast<float>(this->shelf_y + 1 + height) / HEIGHT;
	entry.left = x_offs / scale;
	entry.top = y_offs / scale;
	entry.width = width / scale;
	entry.height = height / scale;

	this->dirty_top = std::min(this->dirty_top, this->shelf_y);
	this->dirty_bottom = std::max(this->dirty_bottom, this->shelf_y + cell_height);
	this->shelf_x += cell_width;
	this->shelf_height = std::max(this->shelf_height, cell_height);
	return entry;
}
