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

/**
 * Copy an image into the atlas, giving transparent pixels the colour of a neighbour,
 * so filtering the texture does not darken the outline of what is drawn.
 * @param image The image, as drawn by the blitter.
 * @param width Its width.
 * @param height Its height.
 * @param dest Where its top left corner goes in the atlas.
 */
static void CopyCell(const std::vector<uint32_t> &image, uint width, uint height, uint32_t *dest)
{
	for (uint y = 0; y < height; y++) {
		for (uint x = 0; x < width; x++) {
			uint32_t pixel = image[static_cast<size_t>(y) * width + x];
			if ((pixel >> 24) != 0) {
				pixel |= 0xFF000000;
			} else {
				pixel = 0;
				static constexpr int NEIGHBOURS[][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
				for (const auto &offset : NEIGHBOURS) {
					uint nx = x + offset[0];
					uint ny = y + offset[1];
					if (nx >= width || ny >= height) continue;
					uint32_t neighbour = image[static_cast<size_t>(ny) * width + nx];
					if ((neighbour >> 24) == 0) continue;
					pixel = neighbour & 0x00FFFFFF;
					break;
				}
			}
			dest[static_cast<size_t>(y) * SpriteAtlas::WIDTH + x] = pixel;
		}
	}
}

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
	if (this->shelf_y + cell_height > HEIGHT - LABEL_ROWS) return entry;

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

	CopyCell(image, cell_width, cell_height, this->pixels.data() + static_cast<size_t>(this->shelf_y) * WIDTH + this->shelf_x);

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

/**
 * Get ready for the labels of a new image. When a label did not fit before, the labels start afresh,
 * so only those still in use take room.
 */
void SpriteAtlas::BeginLabels()
{
	if (!this->labels_full) return;
	this->labels_full = false;
	this->labels.clear();
	this->label_x = 0;
	this->label_y = HEIGHT - LABEL_ROWS;
	this->label_height = 0;
}

/**
 * Get a label from the atlas, drawing it when it is not there yet.
 * Its reference point is the middle of its bottom edge.
 * @param key Tells labels that look different apart.
 * @param width Width of the label in pixels.
 * @param height Height of the label in pixels.
 * @param draw Draws the label at the top left corner of the current drawing area.
 * @return Where the label is; not valid when it did not fit.
 */
const AtlasSprite &SpriteAtlas::GetLabel(const std::string &key, uint width, uint height, const std::function<void()> &draw)
{
	static const AtlasSprite missing{};
	auto found = this->labels.find(key);
	if (found != this->labels.end()) return found->second;
	if (width == 0 || height == 0) return missing;

	uint cell_width = width + 2;
	uint cell_height = height + 2;
	if (cell_width > WIDTH) return missing;
	if (this->label_x + cell_width > WIDTH) {
		this->label_x = 0;
		this->label_y += this->label_height;
		this->label_height = 0;
	}
	if (this->label_y + cell_height > HEIGHT) {
		this->labels_full = true;
		return missing;
	}

	std::vector<uint32_t> image(static_cast<size_t>(cell_width) * cell_height, 0);
	{
		DrawPixelInfo dpi;
		dpi.dst_ptr = image.data() + cell_width + 1;
		dpi.pitch = cell_width;
		dpi.left = 0;
		dpi.top = 0;
		dpi.width = width;
		dpi.height = height;
		dpi.zoom = ZoomLevel::Min;
		AutoRestoreBackup dpi_backup(_cur_dpi, &dpi);
		AutoRestoreBackup anim_backup(_screen_disable_anim, true);
		/* Blitters fill rectangles row by row with the pitch of the screen. */
		AutoRestoreBackup pitch_backup(_screen.pitch, static_cast<int>(cell_width));
		draw();
	}
	/* Labels are solid boards; colours from the palette can come without opacity. */
	for (uint y = 1; y <= height; y++) {
		for (uint x = 1; x <= width; x++) image[static_cast<size_t>(y) * cell_width + x] |= 0xFF000000;
	}
	CopyCell(image, cell_width, cell_height, this->pixels.data() + static_cast<size_t>(this->label_y) * WIDTH + this->label_x);

	AtlasSprite &entry = this->labels[key];
	entry.valid = true;
	entry.u0 = static_cast<float>(this->label_x + 1) / WIDTH;
	entry.v0 = static_cast<float>(this->label_y + 1) / HEIGHT;
	entry.u1 = static_cast<float>(this->label_x + 1 + width) / WIDTH;
	entry.v1 = static_cast<float>(this->label_y + 1 + height) / HEIGHT;
	entry.left = -(width / 2.0f);
	entry.top = -static_cast<float>(height);
	entry.width = static_cast<float>(width);
	entry.height = static_cast<float>(height);

	this->dirty_top = std::min(this->dirty_top, this->label_y);
	this->dirty_bottom = std::max(this->dirty_bottom, this->label_y + cell_height);
	this->label_x += cell_width;
	this->label_height = std::max(this->label_height, cell_height);
	return entry;
}
