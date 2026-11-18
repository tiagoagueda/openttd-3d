/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file lock3d.h Canal locks in the 3D viewport, which fill and empty for the ships that pass them.
 *
 * In the 3D view the three tiles of a lock are one long chamber between two pairs of gates: the lower gates at the
 * outer end of the lower part, the upper gates at the outer end of the upper part. Ships are longer than a tile,
 * so only such a chamber holds them. The game itself knows nothing of this: a ship just moves up or down the middle
 * tile. The 3D view raises and lowers the water in the chamber, and the ship with it, and opens and closes the gates
 * as the ship goes through. These are looks only.
 *
 * Positions along a lock are measured in tiles from the middle of its middle part, towards where the water is higher;
 * positions across it in tiles to the left of that.
 */

#ifndef VIEWPORT3D_LOCK3D_H
#define VIEWPORT3D_LOCK3D_H

#include "../tile_type.h"
#include "../vehicle_type.h"
#include "terrain_mesh.h"

class ModelPack;

/** Where the hinges of the lower and the upper gates are, along the lock. */
static constexpr float LOCK_GATE = 1.42f;
/** Half the width of the chamber, in tiles. */
static constexpr float LOCK_CHANNEL = 0.4f;
/** How deep the bed of a lock is below its lower water, in height levels; the same as that of canals. */
static constexpr float LOCK_DEPTH = 0.5f;

/** Ids of the models of locks in the model package, after the three parts of the lock. */
enum LockModelId : uint16_t {
	LOCK_MODEL_GATE_LOWER = 3, ///< One leaf of the lower gates, hinged at its origin and reaching along +x.
	LOCK_MODEL_GATE_UPPER = 4, ///< One leaf of the upper gates.
};

bool HasLockAnimation(const ModelPack &pack);
TileIndex GetLockMiddleTile(TileIndex tile);
std::array<float, 2> GetLockPoint(TileIndex middle, float along, float across);
void BeginLocks3D();
void AddLockMovingParts(TileIndex middle, const ModelPack &pack, std::vector<ModelInstance> &models, std::vector<TerrainVertex> &water);
bool GetLockShipHeight(const Vehicle *v, float &z);

#endif /* VIEWPORT3D_LOCK3D_H */
