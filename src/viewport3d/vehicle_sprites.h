/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file vehicle_sprites.h Vehicles in the 3D viewport. */

#ifndef VIEWPORT3D_VEHICLE_SPRITES_H
#define VIEWPORT3D_VEHICLE_SPRITES_H

#include "camera3d.h"
#include "model_pack.h"
#include "sprite_atlas.h"
#include "terrain_mesh.h"
#include "../vehicle_type.h"

/** Where a vehicle is in the 3D view, to find the vehicle below the cursor. */
struct VehicleSpritePlace {
	VehicleID vehicle; ///< The vehicle.
	float x, y, z; ///< The place the sprite stands at, in world units with the height in the z units of the game.
	float left, top, width, height; ///< The sprite relative to that place, in pixels of the 2D game.
};

void BuildVehicleSprites(const Camera3D &camera, const Frustum3D &frustum, SpriteAtlas &atlas, const ModelPack *pack, std::vector<SpriteVertex> &vertices, std::vector<VehicleSpritePlace> &places, std::vector<ModelInstance> &models);

#endif /* VIEWPORT3D_VEHICLE_SPRITES_H */
