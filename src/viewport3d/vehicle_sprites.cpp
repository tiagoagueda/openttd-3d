/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file vehicle_sprites.cpp Vehicles in the 3D viewport. */

#include "../stdafx.h"
#include "vehicle_sprites.h"
#include "lock3d.h"
#include "../effectvehicle_base.h"
#include "../effectvehicle_func.h"
#include "../transparency.h"
#include "../vehicle_base.h"
#include "../vehicle_func.h"
#include "../table/sprites.h"

#include "../aircraft.h"
#include "../company_func.h"
#include "../engine_base.h"
#include "../palette_func.h"
#include "../disaster_vehicle.h"
#include "../roadveh.h"
#include "../train.h"

#include <chrono>
#include <map>

#include "../safeguards.h"

/** What is remembered of a vehicle between images, so that it can move smoothly. */
struct SmoothVehicle {
	float yaw; ///< Direction the model points to, in radians from the x axis.
	float rotor_angle; ///< How far the main rotor of a helicopter has turned.
	uint32_t seen; ///< The last image the vehicle was in.
	float age = 0; ///< Seconds the vehicle has been in view; used for puffs of smoke.
};

static std::map<VehicleID, SmoothVehicle> _smooth_vehicles; ///< Vehicles that were in the last images.
static uint32_t _smooth_image = 0; ///< Number of the image that is being made.
static float _smooth_seconds = 0; ///< Seconds since the previous image.

/**
 * Get the place of a vehicle on the map, without its height.
 * @param v The vehicle.
 * @return Its place in world units.
 */
static inline Vec3 FlatPosition(const Vehicle *v)
{
	return {static_cast<float>(v->x_pos), static_cast<float>(v->y_pos), 0};
}

/**
 * Bring an angle between minus and plus half a turn.
 * @param angle The angle in radians.
 * @return The same direction.
 */
static float WrapAngle(float angle)
{
	static constexpr float PI = 3.14159265f;
	while (angle > PI) angle -= 2 * PI;
	while (angle < -PI) angle += 2 * PI;
	return angle;
}

/**
 * Find which way a wagon of a train points and how much it is stretched, from the wagons in front of and behind it.
 * The game moves trains in steps of 45 degrees; a wagon lying along the line between its neighbours bends with the
 * track through curves instead.
 * @param t The wagon.
 * @param heading Which way the wagon goes on the map, as the game says.
 * @param[out] direction Unit vector along the wagon, in the way it goes.
 * @param[out] stretch How much longer than its model the wagon is drawn.
 * @return Whether the wagon had neighbours to go by.
 */
static bool GetTrainShape(const Train *t, Vec3 heading, Vec3 &direction, float &stretch)
{
	auto usable = [](const Train *u) { return u != nullptr && !u->vehstatus.Test(VehState::Hidden); };
	const Train *prev = t->Previous();
	const Train *next = t->Next();
	if (!usable(prev)) prev = nullptr;
	if (!usable(next)) next = nullptr;
	if (prev == nullptr && next == nullptr) return false;

	float length = t->gcache.cached_veh_length;
	Vec3 front = prev != nullptr ? FlatPosition(prev) : FlatPosition(t);
	Vec3 back = next != nullptr ? FlatPosition(next) : FlatPosition(t);
	/* Steps along the track between the middles of the neighbours. */
	float steps = 0;
	if (prev != nullptr) steps += (prev->gcache.cached_veh_length + length) / 2;
	if (next != nullptr) steps += (next->gcache.cached_veh_length + length) / 2;

	Vec3 along = front - back;
	float distance = std::sqrt(Dot(along, along));
	if (distance < 1.0f || steps <= 0) return false;

	direction = along * (1.0f / distance);
	if (Dot(direction, heading) < 0) direction = direction * -1.0f;
	/* A step along straight track is one world unit; along diagonal track it is longer. */
	stretch = length / VEHICLE_LENGTH * std::clamp(distance / steps, 0.7f, 1.5f);
	return true;
}

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
	/* The rotors of helicopters are vehicles of their own; the model of the helicopter may have one that turns. */
	if (v->type == VehicleType::Aircraft && !Aircraft::From(v)->IsNormalAircraft()) {
		if (v->subtype != AIR_ROTOR) return false;
		const ModelPack::VehicleModel *helicopter = pack->FindVehicle(Engine::GetIfValid(v->First()->engine_type));
		return helicopter != nullptr && helicopter->rotor != UINT_MAX;
	}
	const ModelPack::VehicleModel *model = pack->FindVehicle(Engine::GetIfValid(v->engine_type));
	if (model == nullptr) return false;

	/* Which way each direction points on the map; diagonal ones are of length one too. */
	static constexpr float D = 0.70710678f;
	static constexpr float HEADINGS[8][2] = {
		{-D, -D}, {-1, 0}, {-D, D}, {0, 1}, {D, D}, {1, 0}, {D, -D}, {0, -1},
	};
	const float *heading = HEADINGS[to_underlying(v->direction)];
	Vec3 direction{heading[0], heading[1], 0};

	auto [smooth, inserted] = _smooth_vehicles.try_emplace(v->index, SmoothVehicle{std::atan2(heading[1], heading[0]), 0, 0});
	bool continued = !inserted && smooth->second.seen + 1 == _smooth_image;
	smooth->second.seen = _smooth_image;

	/* How much longer or shorter than usual the vehicle is drawn. */
	float stretch = 1;
	bool flip = false;
	if (v->type == VehicleType::Train) {
		const Train *t = Train::From(v);
		if (!GetTrainShape(t, direction, direction, stretch)) {
			stretch = t->gcache.cached_veh_length / static_cast<float>(VEHICLE_LENGTH);
			/* On diagonal track each step goes one along both x and y, so the vehicles of a train are further apart. */
			if (!IsDiagonalDirection(v->direction)) stretch *= 1.41421356f;
		}
		/* Some vehicles are put in the train the other way around. */
		flip = t->flags.Test(VehicleRailFlag::Flipped) && !EngInfo(v->engine_type)->misc_flags.Test(EngineMiscFlag::RailFlips);
	} else {
		/* Other vehicles turn towards the way they go, a little later than the game turns them. */
		float target = std::atan2(heading[1], heading[0]);
		float &yaw = smooth->second.yaw;
		float turn = WrapAngle(target - yaw);
		float delay = v->type == VehicleType::Road ? 0.08f : (v->type == VehicleType::Ship ? 0.3f : 0.2f);
		if (!continued || std::abs(turn) < 0.002f) {
			yaw = target;
		} else {
			yaw = WrapAngle(yaw + turn * (1 - std::exp(-_smooth_seconds / delay)));
		}
		direction = {std::cos(yaw), std::sin(yaw), 0};
	}

	/* Nose up or down on slopes: one height level per tile. */
	float climb = 0;
	if (v->IsGroundVehicle()) {
		GroundVehicleFlags flags = v->type == VehicleType::Train ? Train::From(v)->gv_flags : RoadVehicle::From(v)->gv_flags;
		if (flags.Test(GroundVehicleFlag::GoingUp)) climb = HEIGHT_LEVEL_SIZE / TILE_SIZE;
		if (flags.Test(GroundVehicleFlag::GoingDown)) climb = -HEIGHT_LEVEL_SIZE / TILE_SIZE;
	}
	float length = std::sqrt(1 + climb * climb);
	Vec3 front{direction.x / length, direction.y / length, climb / length};
	Vec3 left{-direction.y, direction.x, 0};
	Vec3 up = Cross(front, left);
	if (flip) {
		front = front * -1.0f;
		left = left * -1.0f;
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
	/* In the chamber of a lock a ship floats on its water, which rises and falls in its own time. */
	GetLockShipHeight(v, instance.transform.m[14]);

	/* The load shows how full the vehicle is; a little cargo shows a little load. */
	instance.load = 0;
	uint count = v->cargo.StoredCount();
	if (count > 0 && v->cargo_cap > 0) instance.load = std::clamp(static_cast<float>(count) / v->cargo_cap, 0.2f, 1.0f);

	instance.tint = {1, 1, 1, 1};
	if (v->owner < MAX_COMPANIES) {
		instance.tint = ModelPack::GetTint(_company_colours[v->owner]);
	}
	/* Vehicles have light behind all their windows at night; see material.sh. */
	instance.tint[3] = 2;

	/* The main rotor of a helicopter turns as fast as the game turns its sprites. */
	const Vehicle *rotor = v->type == VehicleType::Aircraft && v->Next() != nullptr ? v->Next()->Next() : nullptr;
	if (model->rotor != UINT_MAX && rotor != nullptr && rotor->subtype == AIR_ROTOR) {
		uint ticks_per_step = rotor->cur_speed >> 4;
		if (ticks_per_step != 0) smooth->second.rotor_angle = WrapAngle(smooth->second.rotor_angle + _smooth_seconds * 40.0f / ticks_per_step);
		float c = std::cos(smooth->second.rotor_angle);
		float sn = std::sin(smooth->second.rotor_angle);
		Matrix4 local;
		local.m = {
			c, sn, 0, 0,
			-sn, c, 0, 0,
			0, 0, 1, 0,
			model->rotor_at[0], model->rotor_at[1], model->rotor_at[2], 1,
		};
		ModelInstance &blades = models.emplace_back();
		blades.model = model->rotor;
		blades.transform = models[models.size() - 2].transform * local;
		blades.tint = models[models.size() - 2].tint;
	}
	return true;
}

/**
 * Put a model on the map, turned around the vertical axis.
 * @param entry The model.
 * @param v The vehicle it stands for; the model stands at its position.
 * @param yaw Where the front of the model points, in radians from the x axis.
 * @param models The 3D models to draw, to add to.
 * @return The placed model.
 */
static ModelInstance &PlaceVehicleModel(const ModelPack::Entry &entry, const Vehicle *v, float yaw, std::vector<ModelInstance> &models)
{
	float s = entry.scale;
	float c = std::cos(yaw) * s;
	float n = std::sin(yaw) * s;
	ModelInstance &instance = models.emplace_back();
	instance.model = entry.model;
	instance.transform.m = {
		c, n, 0, 0,
		-n, c, 0, 0,
		0, 0, s, 0,
		static_cast<float>(v->x_pos), static_cast<float>(v->y_pos), v->z_pos * WORLD_Z_SCALE, 1,
	};
	instance.tint = {1, 1, 1, 1};
	instance.load = 0;
	return instance;
}

/**
 * Draw a disaster, such as a zeppelin or a submarine, with a model, when the package has one.
 * Their shadows are left out, like those of aircraft.
 * @param v The disaster vehicle.
 * @param pack The package of 3D models, or \c nullptr.
 * @param models The 3D models to draw, to add to.
 * @return Whether the disaster has a model.
 */
static bool AddDisasterModel(const Vehicle *v, const ModelPack *pack, std::vector<ModelInstance> &models)
{
	if (pack == nullptr) return false;
	const ModelPack::Entry *entry = pack->Find(ModelKind::Disaster, 0, v->subtype);
	if (entry == nullptr) return false;

	auto [smooth, inserted] = _smooth_vehicles.try_emplace(v->index, SmoothVehicle{0, 0, 0});
	bool continued = !inserted && smooth->second.seen + 1 == _smooth_image;
	smooth->second.seen = _smooth_image;

	float yaw = 0;
	if (v->direction < Direction::End) {
		static constexpr float D = 0.70710678f;
		static constexpr float HEADINGS[8][2] = {
			{-D, -D}, {-1, 0}, {-D, D}, {0, 1}, {D, D}, {1, 0}, {D, -D}, {0, -1},
		};
		const float *heading = HEADINGS[to_underlying(v->direction)];
		yaw = std::atan2(heading[1], heading[0]);
	}
	if (v->subtype == ST_HELICOPTER_ROTORS || v->subtype == ST_SMALL_UFO || v->subtype == ST_BIG_UFO) {
		/* Rotors and flying saucers spin. */
		if (continued) smooth->second.rotor_angle = WrapAngle(smooth->second.rotor_angle + _smooth_seconds * (v->subtype == ST_HELICOPTER_ROTORS ? 30.0f : 1.5f));
		yaw = smooth->second.rotor_angle;
	}
	PlaceVehicleModel(*entry, v, yaw, models);
	return true;
}

/**
 * Draw the bulldozer of road works with a model, when the package has one.
 * @param v The effect vehicle of the bulldozer.
 * @param pack The package of 3D models.
 * @param models The 3D models to draw, to add to.
 * @return Whether the bulldozer has a model.
 */
static bool AddBulldozerModel(const Vehicle *v, const ModelPack &pack, std::vector<ModelInstance> &models)
{
	const ModelPack::Entry *entry = pack.Find(ModelKind::Effect, 0, EV_BULLDOZER);
	if (entry == nullptr) return false;
	/* The sprite tells which way it faces: north-east, south-east, south-west or north-west. */
	static constexpr float YAWS[4] = {3.14159265f, 1.57079633f, 0.0f, -1.57079633f};
	uint view = (v->sprite_cache.sprite_seq.seq[0].sprite - SPR_BULLDOZER_NE) & 3;
	PlaceVehicleModel(*entry, v, YAWS[view], models);
	return true;
}

/**
 * Draw smoke, steam and sparks as soft 3D puffs that grow and fade, when the package has a model for them.
 * @param v The effect vehicle.
 * @param pack The package of 3D models, or \c nullptr.
 * @param models The 3D models to draw, to add to.
 * @return Whether the effect has a model.
 */
static bool AddEffectModel(const Vehicle *v, const ModelPack *pack, std::vector<ModelInstance> &models)
{
	struct Puff {
		std::array<float, 4> colour; ///< Colour, and how opaque it starts.
		float size; ///< Size when it appears.
		float growth; ///< How much bigger it gets per second.
		float life; ///< Seconds until it has faded the most and stops growing.
	};
	static const std::map<uint8_t, Puff> PUFFS = {
		{EV_CHIMNEY_SMOKE, {{0.78f, 0.78f, 0.80f, 0.75f}, 0.8f, 0.45f, 4.0f}},
		{EV_STEAM_SMOKE, {{0.96f, 0.96f, 0.97f, 0.8f}, 0.6f, 0.9f, 1.8f}},
		{EV_DIESEL_SMOKE, {{0.32f, 0.32f, 0.33f, 0.55f}, 0.45f, 0.6f, 1.4f}},
		{EV_ELECTRIC_SPARK, {{1.0f, 0.95f, 0.6f, 0.95f}, 0.35f, 0.0f, 0.4f}},
		{EV_CRASH_SMOKE, {{0.22f, 0.21f, 0.20f, 0.75f}, 1.2f, 0.3f, 6.0f}},
		{EV_BREAKDOWN_SMOKE, {{0.30f, 0.30f, 0.30f, 0.7f}, 0.8f, 0.4f, 3.0f}},
		{EV_BREAKDOWN_SMOKE_AIRCRAFT, {{0.30f, 0.30f, 0.30f, 0.7f}, 0.8f, 0.4f, 3.0f}},
		{EV_COPPER_MINE_SMOKE, {{0.62f, 0.52f, 0.46f, 0.75f}, 0.8f, 0.4f, 3.0f}},
		{EV_EXPLOSION_LARGE, {{1.0f, 0.55f, 0.15f, 0.95f}, 1.4f, 1.6f, 1.2f}},
		{EV_EXPLOSION_SMALL, {{1.0f, 0.62f, 0.20f, 0.95f}, 0.6f, 1.2f, 0.8f}},
		{EV_BUBBLE, {{0.80f, 0.92f, 1.0f, 0.6f}, 0.35f, 0.05f, 4.0f}},
	};
	if (pack == nullptr) return false;
	if (v->subtype == EV_BULLDOZER) return AddBulldozerModel(v, *pack, models);
	auto puff = PUFFS.find(v->subtype);
	if (puff == PUFFS.end()) return false;
	const ModelPack::Entry *entry = pack->Find(ModelKind::Effect, 0, v->subtype);
	if (entry == nullptr) return false;

	auto [smooth, inserted] = _smooth_vehicles.try_emplace(v->index, SmoothVehicle{0, 0, 0});
	if (!inserted && smooth->second.seen + 1 == _smooth_image) smooth->second.age += _smooth_seconds;
	smooth->second.seen = _smooth_image;

	const Puff &p = puff->second;
	float t = std::min(smooth->second.age, p.life);
	float s = entry->scale * (p.size + p.growth * t);
	/* Each puff turns its own way, so a trail of them does not look like copies. */
	float yaw = (v->index.base() * 2654435761U >> 20) / 4096.0f * 6.2831853f;
	float c = std::cos(yaw) * s;
	float n = std::sin(yaw) * s;

	ModelInstance &instance = models.emplace_back();
	instance.model = entry->model;
	instance.transform.m = {
		c, n, 0, 0,
		-n, c, 0, 0,
		0, 0, s, 0,
		static_cast<float>(v->x_pos), static_cast<float>(v->y_pos), v->z_pos * WORLD_Z_SCALE, 1,
	};
	instance.tint = p.colour;
	instance.tint[3] = p.colour[3] * (1.0f - 0.7f * t / p.life);
	instance.load = 0;
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
	static std::chrono::steady_clock::time_point last_image;
	auto now = std::chrono::steady_clock::now();
	_smooth_seconds = std::clamp(std::chrono::duration<float>(now - last_image).count(), 0.0f, 0.25f);
	last_image = now;
	_smooth_image++;
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
		bool modelled;
		switch (v->type) {
			case VehicleType::Effect: modelled = AddEffectModel(v, pack, models); break;
			case VehicleType::Disaster: modelled = AddDisasterModel(v, pack, models); break;
			default: modelled = AddVehicleModel(v, pack, models); break;
		}
		if (!modelled) AddUprightSprite(sprite, x, y, z, nearer, vertices);
		if (sprite.valid && !v->vehstatus.Test(VehState::Unclickable)) {
			places.push_back({v->index, x, y, z, static_cast<float>(sprite.left), static_cast<float>(sprite.top), static_cast<float>(sprite.width), static_cast<float>(sprite.height)});
		}
	}

	/* Forget vehicles that went out of view. */
	std::erase_if(_smooth_vehicles, [](const auto &item) { return item.second.seen != _smooth_image; });
}
