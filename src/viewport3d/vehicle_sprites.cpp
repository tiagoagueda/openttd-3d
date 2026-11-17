/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file vehicle_sprites.cpp Vehicles in the 3D viewport. */

#include "../stdafx.h"
#include "vehicle_sprites.h"
#include "../effectvehicle_base.h"
#include "../transparency.h"
#include "../vehicle_base.h"
#include "../vehicle_func.h"
#include "../table/sprites.h"

#include "../safeguards.h"

/**
 * Make the sprites of all vehicles that are in view.
 * Vehicles are sprites that stand upright and face the camera, like buildings. The 2D game has
 * sprites of a vehicle for eight directions; the one that is used is the one that fits how the
 * vehicle is seen from where the camera is, so vehicles keep pointing the right way when the
 * camera goes around them.
 * Must be called from the thread that draws the windows.
 * @param camera Where we look from.
 * @param frustum What is in view.
 * @param atlas The atlas that provides the sprites.
 * @param[out] vertices Four vertices per sprite; previous contents are discarded.
 * @param[out] places Where the vehicles that can be clicked are; previous contents are discarded.
 */
void BuildVehicleSprites(const Camera3D &camera, const Frustum3D &frustum, SpriteAtlas &atlas, std::vector<SpriteVertex> &vertices, std::vector<VehicleSpritePlace> &places)
{
	static std::vector<SpritePart> parts;
	vertices.clear();
	places.clear();

	/* The 2D game looks from the south, a yaw of 45 degrees. Each further 45 degrees shows the next side of a vehicle. */
	static constexpr float EIGHTH_TURN = 0.78539816f;
	int turn = static_cast<int>(std::lround((camera.yaw - EIGHTH_TURN) / EIGHTH_TURN));

	for (const Vehicle *v : Vehicle::Iterate()) {
		if (v->vehstatus.Test(VehState::Hidden)) continue;
		/* Shadows of aircraft darken what is below them, which a sprite on its own cannot do. */
		if (v->vehstatus.Test(VehState::Shadow)) continue;
		if (v->type == VehicleType::Effect) {
			TransparencyOption to = EffectVehicle::From(v)->GetTransparencyOption();
			if (to != TransparencyOption::Invalid && (IsTransparencySet(to) || IsInvisibilitySet(to))) continue;
		}

		/* The place the 2D game draws the sprites at. */
		float x = static_cast<float>(v->x_pos + v->bounds.origin.x + v->bounds.offset.x);
		float y = static_cast<float>(v->y_pos + v->bounds.origin.y + v->bounds.offset.y);
		float z = static_cast<float>(v->z_pos + v->bounds.origin.z + v->bounds.offset.z);

		static constexpr float MARGIN = 32.0f;
		Vec3 min{x - MARGIN, y - MARGIN, z * WORLD_Z_SCALE - MARGIN};
		Vec3 max{x + MARGIN, y + MARGIN, z * WORLD_Z_SCALE + MARGIN};
		if (!frustum.IntersectsBox(min, max)) continue;

		VehicleSpriteSeq seq;
		if (v->type == VehicleType::Effect || v->direction >= Direction::End) {
			seq = v->sprite_cache.sprite_seq;
		} else {
			Direction direction = static_cast<Direction>((to_underlying(v->direction) + turn) & 7);
			v->GetImage(direction, EngineImageType::OnMap, &seq);
			if (!seq.IsValid()) seq = v->sprite_cache.sprite_seq;
		}
		if (!seq.IsValid()) continue;

		PaletteID pal = PAL_NONE;
		if (v->vehstatus.Test(VehState::DefaultPalette)) pal = v->vehstatus.Test(VehState::Crashed) ? PALETTE_CRASH : GetVehiclePalette(v);

		parts.clear();
		for (uint i = 0; i < seq.count; i++) {
			PaletteID sprite_pal = seq.seq[i].pal;
			if (sprite_pal == 0 || v->vehstatus.Test(VehState::Crashed)) sprite_pal = pal;
			parts.push_back({.sprite = seq.seq[i].sprite, .pal = sprite_pal});
		}

		/* Vehicles are drawn over the tracks and roads they are on, and stand in the middle of their bounding box like buildings. */
		float nearer = (v->bounds.extent.x + v->bounds.extent.y) * 0.5f + 2.0f;
		const AtlasSprite &sprite = atlas.Get(parts);
		AddUprightSprite(sprite, x, y, z, nearer, vertices);
		if (sprite.valid && !v->vehstatus.Test(VehState::Unclickable)) {
			places.push_back({v->index, x, y, z, static_cast<float>(sprite.left), static_cast<float>(sprite.top), static_cast<float>(sprite.width), static_cast<float>(sprite.height)});
		}
	}
}
