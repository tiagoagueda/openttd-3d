/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file lock3d.cpp Canal locks in the 3D viewport, which fill and empty for the ships that pass them. */

#include "../stdafx.h"
#include "lock3d.h"
#include "camera3d.h"
#include "ground_textures.h"
#include "model_pack.h"
#include "../company_func.h"
#include "../direction_func.h"
#include "../map_func.h"
#include "../ship.h"
#include "../tile_map.h"
#include "../water_map.h"

#include <chrono>
#include <unordered_map>

#include "../safeguards.h"

/** What a lock looks like at the moment. */
struct LockState {
	float level = 0; ///< Height of the water in the chamber: 0 at the lower water, 1 at the upper.
	std::array<float, 2> gates{1, 0}; ///< How far the lower and the upper gates are open, 0 to 1.
	std::chrono::steady_clock::time_point updated; ///< When it was last moved on.
	uint frame = 0; ///< The image it was last moved on for.
};

static std::unordered_map<uint32_t, LockState> _locks; ///< The locks that were seen, by their middle tile.
static uint _lock_frame = 0; ///< Counts the images, so each lock moves on once per image.

/** A ship that may be using a lock. */
struct LockShip {
	float x, y; ///< Where it is, in tiles.
	float dx, dy; ///< Which way it goes.
};
static std::vector<LockShip> _lock_ships; ///< The ships on or near locks, for this image.

/** Which way each direction of a lock points on the map. */
static constexpr float LOCK_HEADINGS[4][2] = {{-1, 0}, {0, 1}, {1, 0}, {0, -1}};

/**
 * Whether the model package has what moving locks need: the gates as models of their own.
 * @param pack The model package.
 * @return True when locks can open and close their gates.
 */
bool HasLockAnimation(const ModelPack &pack)
{
	return pack.Find(ModelKind::Lock, 0, LOCK_MODEL_GATE_LOWER) != nullptr && pack.Find(ModelKind::Lock, 0, LOCK_MODEL_GATE_UPPER) != nullptr;
}

/**
 * Get the middle part of the lock that a tile is part of.
 * @param tile A tile of a lock.
 * @return Its middle part.
 */
TileIndex GetLockMiddleTile(TileIndex tile)
{
	DiagDirection dir = GetLockDirection(tile);
	switch (GetLockPart(tile)) {
		case LockPart::Lower: return TileAddByDiagDir(tile, dir);
		case LockPart::Upper: return TileAddByDiagDir(tile, ReverseDiagDir(dir));
		default: return tile;
	}
}

/**
 * Get a place on a lock, on the map.
 * @param middle The middle part of the lock.
 * @param along How far along the lock, in tiles from the middle of its middle part towards the upper part.
 * @param across How far to the left of that, in tiles.
 * @return The place, in tiles.
 */
std::array<float, 2> GetLockPoint(TileIndex middle, float along, float across)
{
	const float *h = LOCK_HEADINGS[to_underlying(GetLockDirection(middle))];
	return {TileX(middle) + 0.5f + along * h[0] - across * h[1], TileY(middle) + 0.5f + along * h[1] + across * h[0]};
}

/** Start a new image: look up where the ships are. */
void BeginLocks3D()
{
	_lock_frame++;
	_lock_ships.clear();
	static constexpr float D = 0.70710678f;
	static constexpr float HEADINGS[8][2] = {{-D, -D}, {-1, 0}, {-D, D}, {0, 1}, {D, D}, {1, 0}, {D, -D}, {0, -1}};
	for (const Ship *v : Ship::Iterate()) {
		if (v->direction >= Direction::End || v->vehstatus.Test(VehState::Hidden)) continue;
		const float *h = HEADINGS[to_underlying(v->direction)];
		_lock_ships.push_back({v->x_pos / static_cast<float>(TILE_SIZE), v->y_pos / static_cast<float>(TILE_SIZE), h[0], h[1]});
	}
}

static float SmoothStep(float edge0, float edge1, float x)
{
	float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
	return t * t * (3 - 2 * t);
}

/** Move a value towards another, by at most a step. */
static void MoveTowards(float &value, float target, float step)
{
	value = target > value ? std::min(target, value + step) : std::max(target, value - step);
}

/**
 * Move a lock on to what it does now, for the ships that come and go.
 * @param middle Its middle part.
 * @param state What it looks like.
 */
static void UpdateLock(TileIndex middle, LockState &state)
{
	auto now = std::chrono::steady_clock::now();
	float dt = state.frame == 0 ? 0.0f : std::clamp(std::chrono::duration<float>(now - state.updated).count(), 0.0f, 0.1f);
	state.updated = now;
	state.frame = _lock_frame;

	const float *h = LOCK_HEADINGS[to_underlying(GetLockDirection(middle))];
	float cx = TileX(middle) + 0.5f, cy = TileY(middle) + 0.5f;

	/* The ship in the chamber that is nearest its middle; otherwise the nearest one that comes towards the lock. */
	const LockShip *inside = nullptr, *coming = nullptr;
	float inside_along = 0, coming_along = 0;
	for (const LockShip &ship : _lock_ships) {
		float along = (ship.x - cx) * h[0] + (ship.y - cy) * h[1];
		float across = -(ship.x - cx) * h[1] + (ship.y - cy) * h[0];
		if (std::abs(across) > 0.6f || std::abs(along) > 3.5f) continue;
		float way = ship.dx * h[0] + ship.dy * h[1];
		if (std::abs(along) < LOCK_GATE) {
			if (inside == nullptr || std::abs(along) < std::abs(inside_along)) {
				inside = &ship;
				inside_along = along;
			}
		} else if (along * way < -0.1f && (coming == nullptr || std::abs(along) < std::abs(coming_along))) {
			coming = &ship;
			coming_along = along;
		}
	}

	if (inside != nullptr) {
		/* The ship is in the chamber: the gates behind it close, the water takes it up or down, and the gates ahead open.
		 * Ships are about a tile long, so all that happens while its middle is in the middle part. */
		bool up = inside->dx * h[0] + inside->dy * h[1] >= 0;
		float s = up ? inside_along : -inside_along;
		float behind = 1 - SmoothStep(-0.85f, -0.65f, s);
		float level = SmoothStep(-0.6f, 0.6f, s);
		float ahead = SmoothStep(0.65f, 0.85f, s);
		float step = 4.0f * dt;
		MoveTowards(state.level, up ? level : 1 - level, step);
		MoveTowards(state.gates[up ? 0 : 1], behind, step);
		MoveTowards(state.gates[up ? 1 : 0], ahead, step);
	} else if (coming != nullptr) {
		/* Get the lock ready for a ship that comes: close the far gates, fill or empty the chamber, then open the gates on its side. */
		int side = coming_along < 0 ? 0 : 1;
		float want = static_cast<float>(side);
		float gate_step = 0.8f * dt, level_step = 0.35f * dt;
		MoveTowards(state.gates[1 - side], 0, gate_step);
		if (std::abs(state.level - want) > 0.001f) {
			MoveTowards(state.gates[side], 0, gate_step);
			if (state.gates[0] <= 0 && state.gates[1] <= 0) MoveTowards(state.level, want, level_step);
		} else {
			state.level = want;
			MoveTowards(state.gates[side], 1, gate_step);
		}
	}
	/* Without ships the lock stays as the last one left it. */
}

/**
 * Add the gates and the water of the chamber of a lock as they are now.
 * @param middle The middle part of the lock.
 * @param pack The model package.
 * @param models The models to add the gates to.
 * @param water The see-through water to add the water of the chamber to.
 */
void AddLockMovingParts(TileIndex middle, const ModelPack &pack, std::vector<ModelInstance> &models, std::vector<TerrainVertex> &water)
{
	if (!IsValidTile(middle) || !IsTileType(middle, TileType::Water) || !IsLock(middle) || GetLockPart(middle) != LockPart::Middle) return;
	const ModelPack::Entry *lower = pack.Find(ModelKind::Lock, 0, LOCK_MODEL_GATE_LOWER);
	const ModelPack::Entry *upper = pack.Find(ModelKind::Lock, 0, LOCK_MODEL_GATE_UPPER);
	if (lower == nullptr || upper == nullptr) return;

	LockState &state = _locks[middle.base()];
	if (state.frame != _lock_frame) UpdateLock(middle, state);

	const DiagDirection dir = GetLockDirection(middle);
	const float *h = LOCK_HEADINGS[to_underlying(dir)];
	const int high = GetTileMaxZ(middle), low = high - 1;
	Owner owner = GetTileOwner(middle);
	std::array<float, 4> tint = ModelPack::GetTint(owner < MAX_COMPANIES ? _company_colours[owner] : Colours::Grey);

	/* Closed, the leaves of a gate meet in the middle, pointing a little upstream; open, they lie along the walls, upstream of their hinges. */
	static const float CLOSED = std::atan2(LOCK_CHANNEL, 0.065f);
	const float heading = std::atan2(h[1], h[0]);
	for (int gate = 0; gate < 2; gate++) {
		const ModelPack::Entry &entry = gate == 0 ? *lower : *upper;
		float along = gate == 0 ? -LOCK_GATE : LOCK_GATE;
		float z = (gate == 0 ? low : high) * HEIGHT_LEVEL_SIZE;
		float open = state.gates[gate];
		open = open * open * (3 - 2 * open);
		for (int leaf = -1; leaf <= 1; leaf += 2) {
			std::array<float, 2> at = GetLockPoint(middle, along, leaf * LOCK_CHANNEL);
			float angle = heading - leaf * CLOSED * (1 - open);
			float s = entry.scale;
			float c = std::cos(angle) * s, n = std::sin(angle) * s;
			ModelInstance &instance = models.emplace_back();
			instance.model = entry.model;
			instance.transform.m = {
				c, n, 0, 0,
				-n, c, 0, 0,
				0, 0, s, 0,
				at[0] * TILE_SIZE, at[1] * TILE_SIZE, z, 1,
			};
			instance.tint = tint;
		}
	}

	/* The water in the chamber, which shows the bed below it while it is low. */
	static constexpr float SUN_Z = 0.6817f;
	const uint8_t lit = static_cast<uint8_t>(std::clamp(255.0f * (0.45f + 0.80f * SUN_Z) * 0.5f, 0.0f, 255.0f));
	const float top = low + state.level;
	const uint8_t depth = static_cast<uint8_t>(std::clamp((1.0f - state.level - LOCK_DEPTH) * 64.0f, 0.0f, 255.0f));
	auto corner = [&](float along, float across) {
		std::array<float, 2> p = GetLockPoint(middle, along, across);
		return TerrainVertex{p[0], p[1], top, lit, lit, lit, depth, static_cast<float>(GROUND_TEXTURE_SHALLOWS), p[0] - std::floor(p[0]), p[1] - std::floor(p[1])};
	};
	water.insert(water.end(), {corner(-LOCK_GATE, -LOCK_CHANNEL), corner(-LOCK_GATE, LOCK_CHANNEL), corner(LOCK_GATE, -LOCK_CHANNEL), corner(LOCK_GATE, LOCK_CHANNEL)});
}

/**
 * Get the height a ship is drawn at while it is in the chamber of a lock: that of the water in it.
 * @param v The vehicle.
 * @param[out] z The height, in world units.
 * @return Whether the vehicle is a ship in the chamber of a lock that is drawn.
 */
bool GetLockShipHeight(const Vehicle *v, float &z)
{
	if (v->type != VehicleType::Ship || !IsValidTile(v->tile) || !IsTileType(v->tile, TileType::Water) || !IsLock(v->tile)) return false;
	TileIndex middle = GetLockMiddleTile(v->tile);
	auto it = _locks.find(middle.base());
	if (it == _locks.end() || it->second.frame != _lock_frame) return false;
	const float *h = LOCK_HEADINGS[to_underlying(GetLockDirection(middle))];
	float along = (v->x_pos / static_cast<float>(TILE_SIZE) - TileX(middle) - 0.5f) * h[0] + (v->y_pos / static_cast<float>(TILE_SIZE) - TileY(middle) - 0.5f) * h[1];
	if (std::abs(along) >= LOCK_GATE) return false;
	z = (GetTileMaxZ(middle) - 1 + it->second.level) * HEIGHT_LEVEL_SIZE;
	return true;
}
