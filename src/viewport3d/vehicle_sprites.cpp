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

#include "../aircraft.h"
#include "../company_func.h"
#include "../engine_base.h"
#include "../palette_func.h"
#include "../roadveh.h"
#include "../train.h"
#include "../safeguards.h"

/**
 * Draw a vehicle with a 3D model, if the package has one for it.
 * @param v The vehicle.
 * @param pack The package of 3D models, or \c nullptr.
 * @param models The 3D models to draw, to add to.
 * @return Whether the vehicle has a model.
 */
static bool AddVehicleModel(const Vehicle *v, const ModelPack *pack, std::vector<ModelInstance> &models)
{
	if (pack == nullptr || !IsCompanyBuildableVehicleType(v) || v->direction >= Direction::End) return false;
	/* The rotors of helicopters are vehicles of their own. */
	if (v->type == VehicleType::Aircraft && !Aircraft::From(v)->IsNormalAircraft()) return false;
	const ModelPack::VehicleModel *model = pack->FindVehicle(Engine::GetIfValid(v->engine_type));
	if (model == nullptr) return false;

	/* Which way each direction points on the map; diagonal ones are of length one too. */
	static constexpr float D = 0.70710678f;
	static constexpr float HEADINGS[8][2] = {
		{-D, -D}, {-1, 0}, {-D, D}, {0, 1}, {D, D}, {1, 0}, {D, -D}, {0, -1},
	};
	const float *heading = HEADINGS[to_underlying(v->direction)];

	/* Nose up or down on slopes: one height level per tile. */
	float climb = 0;
	if (v->IsGroundVehicle()) {
		GroundVehicleFlags flags = v->type == VehicleType::Train ? Train::From(v)->gv_flags : RoadVehicle::From(v)->gv_flags;
		if (flags.Test(GroundVehicleFlag::GoingUp)) climb = HEIGHT_LEVEL_SIZE / TILE_SIZE;
		if (flags.Test(GroundVehicleFlag::GoingDown)) climb = -HEIGHT_LEVEL_SIZE / TILE_SIZE;
	}
	float length = std::sqrt(1 + climb * climb);
	Vec3 front{heading[0] / length, heading[1] / length, climb / length};
	Vec3 left{-heading[1], heading[0], 0};
	Vec3 up = Cross(front, left);

	/* How much longer or shorter than usual the vehicle is drawn. */
	float stretch = 1;
	if (v->type == VehicleType::Train) {
		const Train *t = Train::From(v);
		stretch = t->gcache.cached_veh_length / static_cast<float>(VEHICLE_LENGTH);
		/* On diagonal track each step goes one along both x and y, so the vehicles of a train are further apart. */
		if (!IsDiagonalDirection(v->direction)) stretch *= 1.41421356f;
		/* Some vehicles are put in the train the other way around. */
		if (t->flags.Test(VehicleRailFlag::Flipped) && !EngInfo(v->engine_type)->misc_flags.Test(EngineMiscFlag::RailFlips)) {
			front = {-front.x, -front.y, -front.z};
			left = {-left.x, -left.y, -left.z};
		}
	}

	ModelInstance &instance = models.emplace_back();
	instance.model = model->model;
	float s = model->scale;
	instance.transform.m = {
		front.x * s * stretch, front.y * s * stretch, front.z * s * stretch, 0,
		left.x * s, left.y * s, left.z * s, 0,
		up.x * s, up.y * s, up.z * s, 0,
		static_cast<float>(v->x_pos), static_cast<float>(v->y_pos), v->z_pos * WORLD_Z_SCALE, 1,
	};

	/* The load shows how full the vehicle is; a little cargo shows a little load. */
	instance.load = 0;
	uint count = v->cargo.StoredCount();
	if (count > 0 && v->cargo_cap > 0) instance.load = std::clamp(static_cast<float>(count) / v->cargo_cap, 0.2f, 1.0f);

	instance.tint = {1, 1, 1, 1};
	if (v->owner < MAX_COMPANIES) {
		instance.tint = ModelPack::GetTint(_company_colours[v->owner]);
	}
	return true;
}

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
 * @param pack The package of 3D models; vehicles that have a model there are drawn with it instead of with sprites. May be \c nullptr.
 * @param[out] vertices Four vertices per sprite; previous contents are discarded.
 * @param[out] places Where the vehicles that can be clicked are; previous contents are discarded.
 * @param[out] models The 3D models to draw; previous contents are discarded.
 */
void BuildVehicleSprites(const Camera3D &camera, const Frustum3D &frustum, SpriteAtlas &atlas, const ModelPack *pack, std::vector<SpriteVertex> &vertices, std::vector<VehicleSpritePlace> &places, std::vector<ModelInstance> &models)
{
	static std::vector<SpritePart> parts;
	vertices.clear();
	places.clear();
	models.clear();

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
		/* The sprite still tells where the vehicle can be clicked when a model is drawn in its place. */
		if (!AddVehicleModel(v, pack, models)) AddUprightSprite(sprite, x, y, z, nearer, vertices);
		if (sprite.valid && !v->vehstatus.Test(VehState::Unclickable)) {
			places.push_back({v->index, x, y, z, static_cast<float>(sprite.left), static_cast<float>(sprite.top), static_cast<float>(sprite.width), static_cast<float>(sprite.height)});
		}
	}
}
