/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file model3d.h 3D models for the 3D viewport. */

#ifndef VIEWPORT3D_MODEL3D_H
#define VIEWPORT3D_MODEL3D_H

#include "camera3d.h"

/**
 * One corner of a triangle of a model.
 * Positions are in the frame of the model: x towards its front, y to its left and z up.
 */
struct ModelVertex {
	float x, y, z; ///< Position.
	float nx, ny, nz; ///< Direction the surface faces, for the light.
	uint8_t r, g, b; ///< Colour.
	uint8_t a; ///< 255 for a fixed colour; 0 when the colour is multiplied with the colour of the owner.
};

/** A 3D model: triangles with a colour per corner. */
struct Model3D {
	std::vector<ModelVertex> vertices; ///< The corners.
	std::vector<uint32_t> indices; ///< Three corners per triangle; the triangles of the load come last.
	size_t num_fixed_indices = 0; ///< Number of indices before the triangles of the load.
	float load_depth = 0; ///< How far the load can sink into the vehicle, in units of the model.
};

/** One model to draw somewhere on the map. */
struct ModelInstance {
	uint model; ///< Which model, as given to Renderer3D::SetModel().
	Matrix4 transform; ///< From the frame of the model to world units.
	std::array<float, 4> tint; ///< Colour of the owner, each channel from 0 to 1.
	float load = 1; ///< How full the vehicle is, from 0 (the load is not drawn) to 1.

	bool operator==(const ModelInstance &other) const = default;
};

bool LoadGlbModel(std::span<const uint8_t> data, Model3D &model, std::string &error);

#endif /* VIEWPORT3D_MODEL3D_H */
