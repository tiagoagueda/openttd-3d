/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file lighting3d.h The light of the 3D viewport through the day and the night. */

#ifndef VIEWPORT3D_LIGHTING3D_H
#define VIEWPORT3D_LIGHTING3D_H

#include "camera3d.h"

/** A colour of light; 1 is the full brightness of the screen. */
using LightColour = std::array<float, 3>;

/**
 * How the world is lit at one moment.
 * Without the cycle of day and night it is the fixed light of the original graphics: late morning, sun from the east.
 */
struct Lighting3D {
	/** The light that all terrain has baked into its colours; the shaders take it out again to put in the light of the moment. */
	static constexpr Vec3 REFERENCE_SUN{-0.4683f, 0.5620f, 0.6817f};
	static constexpr float REFERENCE_AMBIENT = 0.45f; ///< Light from all around of the baked light.
	static constexpr float REFERENCE_DIRECT = 0.80f; ///< Light from the sun of the baked light.

	Vec3 direction = REFERENCE_SUN; ///< Unit vector towards the light that casts the shadows: the sun by day, the moon by night.
	LightColour ambient{REFERENCE_AMBIENT, REFERENCE_AMBIENT, REFERENCE_AMBIENT}; ///< Light from all around.
	LightColour direct{REFERENCE_DIRECT, REFERENCE_DIRECT, REFERENCE_DIRECT}; ///< Light from #direction.
	LightColour haze{0.80f, 0.86f, 0.92f}; ///< Colour of the haze in the distance, and of the sky at the horizon.
	LightColour sky{0.40f, 0.60f, 0.86f}; ///< Colour of the sky high up.
	Vec3 sun = REFERENCE_SUN; ///< Unit vector towards the sun, also when it is below the horizon.
	Vec3 moon{0.4683f, -0.5620f, -0.6817f}; ///< Unit vector towards the moon.
	float stars = 0; ///< How well the stars show, 0 to 1.
	float lamps = 0; ///< How much lamps and lit windows glow, 0 (day) to 1 (night).
	float windows = 0; ///< Which share of the windows of buildings has light behind it, 0 to 1.

	bool operator==(const Lighting3D &other) const = default;
};

Lighting3D GetLighting3D(float hour);
float GetView3DHour();
void SetView3DHour(float hour);

#endif /* VIEWPORT3D_LIGHTING3D_H */
