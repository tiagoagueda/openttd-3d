/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file tile_sprites.h Finding out which sprites the 2D game draws for a tile, for the 3D viewport. */

#ifndef VIEWPORT3D_TILE_SPRITES_H
#define VIEWPORT3D_TILE_SPRITES_H

#include "../gfx_type.h"
#include "../tile_type.h"

/** One sprite as the 2D game draws it, at a position relative to some reference point. */
struct SpritePart {
	SpriteID sprite = 0; ///< The sprite, including its modifier bits.
	PaletteID pal = 0; ///< The palette.
	int x = 0; ///< Horizontal position on screen relative to the reference point, in pixels at the most detailed zoom level.
	int y = 0; ///< Vertical position on screen relative to the reference point, in pixels at the most detailed zoom level.
	bool clipped = false; ///< Whether only the part of the sprite within \c clip is drawn.
	int clip_left = 0, clip_top = 0, clip_right = 0, clip_bottom = 0; ///< The part to draw, as in SubSprite.

	auto operator<=>(const SpritePart &) const = default;
};

/** A sprite with a bounding box in the world, and the sprites that are drawn along with it. */
struct TileSpriteGroup {
	int xmin, ymin, zmin; ///< Lowest corner of the bounding box, in world coordinates.
	int xmax, ymax, zmax; ///< Highest corner of the bounding box, inclusive.
	uint first; ///< Index of the first sprite of the group in TileSprites::parts.
	uint count; ///< Number of sprites in the group.
};

/** Everything the 2D game draws for one tile, apart from vehicles and signs. */
struct TileSprites {
	std::vector<SpritePart> ground; ///< Sprites drawn flat on the ground, relative to the screen position of the tile's northern corner at the tile's base height.
	std::vector<TileSpriteGroup> groups; ///< Things standing on the tile.
	std::vector<SpritePart> parts; ///< The sprites of the groups, relative to the same reference point as the ground sprites.
};

void CollectTileSprites(TileIndex tile, TileSprites &sprites);

#endif /* VIEWPORT3D_TILE_SPRITES_H */
