/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file tile_sprites.h Finding out which sprites the 2D game draws for a tile, for the 3D viewport. */

#ifndef VIEWPORT3D_TILE_SPRITES_H
#define VIEWPORT3D_TILE_SPRITES_H

#include "../core/geometry_type.hpp"
#include "../gfx_type.h"
#include "../tile_type.h"

struct Vehicle;

/**
 * Tells where the 3D view shows a point of the map.
 * @param x Position along the x axis in world units.
 * @param y Position along the y axis in world units.
 * @param z Height in the z units of the game.
 * @param[out] pt Where the point is in the 3D image, in pixels.
 * @return Whether the point is in front of the camera.
 */
using View3DProjectProc = std::function<bool(int x, int y, int z, Point &pt)>;

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
	bool foundation = false; ///< Whether this is the foundation of the tile.
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
	uint bridge_first = UINT_MAX; ///< Index of the first group that belongs to a bridge above the tile; UINT_MAX if there is none.
};

void CollectTileSprites(TileIndex tile, TileSprites &sprites);
bool HasTileSelection(TileIndex tile);
/** A town name, station name or sign, which the 3D view shows on a board that stands on the map. */
struct View3DLabel {
	/** What the label names. */
	enum class Kind : uint8_t {
		Town, ///< A town.
		Sign, ///< A sign placed by a player or script.
		Station, ///< A station or waypoint.
	};
	int x = 0; ///< Where it stands along the x axis, in world units.
	int y = 0; ///< Where it stands along the y axis, in world units.
	int z = 0; ///< Height of its bottom edge in the 2D views, in the z units of the game.
	uint width = 0; ///< Width of its image in pixels.
	uint height = 0; ///< Height of its image in pixels.
	std::string key; ///< Tells labels that look different apart: the text, colour and flags.
	Kind kind = Kind::Town; ///< What it names.
	uint32_t id = 0; ///< Index of the town, sign or station.
};

void GetView3DLabels(std::vector<View3DLabel> &labels);
void DrawView3DLabel(size_t index);
bool HandleView3DClicked(const Vehicle *v, const View3DLabel *label);

#endif /* VIEWPORT3D_TILE_SPRITES_H */
