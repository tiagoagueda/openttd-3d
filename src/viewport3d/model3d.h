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
	uint8_t a; ///< What it is made of: 255 for a fixed colour; 0 when the colour is multiplied with the colour of the owner; #LAMP; or #WINDOW plus 8 times the number of the window; #GLASS; or #LEAF for leaves.
	float u, v; ///< Position in the texture, which repeats beyond 0 to 1.
	float texture; ///< Which texture the colour is multiplied with, as given by the #ModelTextureFinder; -1 for none.

	static constexpr uint8_t LAMP = 48; ///< A lamp, which glows at night.
	static constexpr uint8_t WINDOW = 96; ///< The first window; windows may have light behind them at night.
	static constexpr uint8_t NUM_WINDOWS = 8; ///< How many windows of a model can be lit apart from each other.
	static constexpr uint8_t GLASS = 160; ///< A large pane of glass with many rooms behind it, which are lit apart from each other.
	static constexpr uint8_t LEAF = 208; ///< Alpha of leaves that change colour with the seasons; the shaders take 184 to 232 for it.
	static constexpr uint8_t LEAF_EVERGREEN = 216; ///< Alpha of dark leaves while a model is read; ModelPack turns it into #LEAF or 255.
};

/** A 3D model: triangles with a colour per corner. */
struct Model3D {
	std::vector<ModelVertex> vertices; ///< The corners.
	std::vector<uint32_t> indices; ///< Three corners per triangle; the triangles of the load come last.
	size_t num_fixed_indices = 0; ///< Number of indices before the triangles of the load.
	float load_depth = 0; ///< How far the load can sink into the vehicle, in units of the model.
	std::vector<Vec3> lamps; ///< The middle of each lamp that casts light on the ground at night.
};

/** One model to draw somewhere on the map. */
struct ModelInstance {
	uint model; ///< Which model, as given to Renderer3D::SetModel().
	Matrix4 transform; ///< From the frame of the model to world units.
	std::array<float, 4> tint; ///< Colour of the owner, each channel from 0 to 1; alpha below 1 makes the whole model see-through.
	float load = 1; ///< How full the vehicle is, from 0 (the load is not drawn) to 1.

	bool operator==(const ModelInstance &other) const = default;
};

/**
 * Finds the textures of models, so that models can share them.
 * @param uri For an image in a file of its own, the name of that file as the model gives it; otherwise empty.
 * @param data For an image inside the model, the contents of the image file.
 * @return The number of the texture, or -1 when the image cannot be used.
 */
using ModelTextureFinder = std::function<int(std::string_view uri, std::span<const uint8_t> data)>;

bool LoadGlbModel(std::span<const uint8_t> data, Model3D &model, std::string &error, const ModelTextureFinder &find_texture = {});

#endif /* VIEWPORT3D_MODEL3D_H */
