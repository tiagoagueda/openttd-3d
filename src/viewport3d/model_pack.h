/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file model_pack.h The package of 3D models that replace sprites in the 3D viewport. */

#ifndef VIEWPORT3D_MODEL_PACK_H
#define VIEWPORT3D_MODEL_PACK_H

#include "model3d.h"
#include "../vehicle_type.h"

class Engine;

/**
 * The OpenGFX3D package: 3D models, and which vehicles they are for.
 * Everything without a model keeps the sprites of the 2D game.
 */
class ModelPack {
public:
	/** A model for a vehicle. */
	struct VehicleModel {
		uint model; ///< Index into GetModels().
		float scale; ///< World units per unit of the model.
	};

	ModelPack();

	/** Name of the package, empty when there is none. */
	const std::string &GetName() const { return this->name; }

	/** All models of the package. */
	std::span<const Model3D> GetModels() const { return this->models; }

	const VehicleModel *FindVehicle(const Engine *e) const;

private:
	using VehicleKey = std::tuple<VehicleType, uint32_t, uint16_t>; ///< Kind of vehicle, NewGRF and the number of the vehicle within the NewGRF.

	std::string name; ///< Name of the package.
	std::vector<Model3D> models; ///< The models.
	std::map<VehicleKey, VehicleModel> vehicles; ///< Which vehicles have a model.
};

#endif /* VIEWPORT3D_MODEL_PACK_H */
