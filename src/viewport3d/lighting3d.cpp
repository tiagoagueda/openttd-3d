/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file lighting3d.cpp The light of the 3D viewport through the day and the night. */

#include "../stdafx.h"
#include "lighting3d.h"
#include "../gfx_type.h"
#include "../settings_type.h"
#include "../timer/timer_game_tick.h"

#include "../safeguards.h"

/** The hour of the day a new game starts at. */
static constexpr float START_HOUR = 9.0f;

/** The hour the view is held at, or negative to follow the clock. */
static float _view3d_fixed_hour = -1.0f;

/** Goes smoothly from 0 at \a from to 1 at \a to; \a from may be the larger of the two. */
static float Smooth(float from, float to, float value)
{
	float t = std::clamp((value - from) / (to - from), 0.0f, 1.0f);
	return t * t * (3 - 2 * t);
}

/** A colour between two others. */
static LightColour Mix(const LightColour &a, const LightColour &b, float t)
{
	return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

/** A colour made brighter or darker. */
static LightColour Scale(const LightColour &a, float f)
{
	return {a[0] * f, a[1] * f, a[2] * f};
}

/**
 * Hold the 3D view at an hour of the day, or let it follow the clock again.
 * @param hour The hour, from 0 to 24; negative to follow the clock.
 */
void SetView3DHour(float hour)
{
	_view3d_fixed_hour = hour < 0 ? -1.0f : std::fmod(hour, 24.0f);
}

/**
 * The hour of the day in the 3D view.
 * The game has no clock, and its days last a few seconds, so the 3D view keeps its own: a day of
 * a number of minutes of play (the setting gui.view3d_day_length), which stands still while the game is paused.
 * @return The hour, from 0 to 24; negative when there is no day and night.
 */
float GetView3DHour()
{
	if (_view3d_fixed_hour >= 0) return _view3d_fixed_hour;
	uint minutes = _settings_client.gui.view3d_day_length;
	if (minutes == 0) return -1.0f;

	uint64_t ticks_per_day = static_cast<uint64_t>(minutes) * 60 * 1000 / MILLISECONDS_PER_TICK;
	double day = static_cast<double>(TimerGameTick::counter % ticks_per_day) / static_cast<double>(ticks_per_day);
	return static_cast<float>(std::fmod(START_HOUR + day * 24.0, 24.0));
}

/**
 * The light at an hour of the day.
 * The sun rises in the east at six, stands in the south at noon and sets in the west at six; the moon takes over for the night.
 * @param hour The hour, from 0 to 24; negative for the fixed light of the original graphics.
 * @return The light.
 */
Lighting3D GetLighting3D(float hour)
{
	Lighting3D light;
	if (hour < 0) return light;

	/* x runs to the south-west and y to the south-east. */
	static constexpr float R = 0.70710678f;
	static constexpr Vec3 EAST{-R, R, 0};
	static constexpr Vec3 SOUTH{R, R, 0};
	/* The sun's path leans 35 degrees to the south, so at noon it stands 55 degrees high. */
	static const Vec3 top = SOUTH * std::sin(0.6109f) + Vec3{0, 0, 1} * std::cos(0.6109f);

	float angle = (hour - 6.0f) / 12.0f * 3.14159265f;
	light.sun = Normalise(EAST * std::cos(angle) + top * std::sin(angle));
	/* The moon is up when the sun is down, and takes the same path. */
	light.moon = Normalise(EAST * -std::cos(angle) + top * -std::sin(angle));
	float height = light.sun.z;

	/* The sun: weak and orange near the horizon, white higher up. */
	static constexpr LightColour SUN_LOW{1.00f, 0.52f, 0.22f};
	static constexpr LightColour SUN_HIGH{1.00f, 1.00f, 1.00f};
	static constexpr LightColour MOON{0.50f, 0.64f, 1.00f};
	if (height >= 0) {
		light.direction = light.sun;
		light.direct = Scale(Mix(SUN_LOW, SUN_HIGH, Smooth(0.04f, 0.42f, height)), Lighting3D::REFERENCE_DIRECT * Smooth(0.0f, 0.20f, height));
	} else {
		light.direction = light.moon;
		light.direct = Scale(MOON, 0.36f * Smooth(0.0f, 0.20f, light.moon.z));
	}
	/* Just above the horizon shadows would be endless; the light is too weak by then to show that it comes from higher than it should. */
	if (light.direction.z < 0.2f) {
		light.direction.z = 0.2f;
		light.direction = Normalise(light.direction);
	}

	/* Light from all around: blue at night, a little purple at dusk and dawn. */
	static constexpr LightColour AMBIENT_NIGHT{0.24f, 0.29f, 0.44f};
	static constexpr LightColour AMBIENT_DUSK{0.54f, 0.47f, 0.52f};
	static constexpr LightColour AMBIENT_DAY{Lighting3D::REFERENCE_AMBIENT, Lighting3D::REFERENCE_AMBIENT, Lighting3D::REFERENCE_AMBIENT};
	light.ambient = Mix(Mix(AMBIENT_NIGHT, AMBIENT_DUSK, Smooth(-0.25f, 0.0f, height)), AMBIENT_DAY, Smooth(0.0f, 0.30f, height));

	/* The sky, and the haze that takes its colour at the horizon; red around sunrise and sunset. */
	static constexpr LightColour HAZE_NIGHT{0.06f, 0.08f, 0.15f};
	static constexpr LightColour HAZE_DAY{0.80f, 0.86f, 0.92f};
	static constexpr LightColour HAZE_GLOW{0.94f, 0.62f, 0.42f};
	static constexpr LightColour SKY_NIGHT{0.015f, 0.025f, 0.07f};
	static constexpr LightColour SKY_DUSK{0.26f, 0.28f, 0.50f};
	static constexpr LightColour SKY_DAY{0.40f, 0.60f, 0.86f};
	float glow = std::exp(-(height - 0.02f) * (height - 0.02f) / (0.15f * 0.15f));
	light.haze = Mix(Mix(HAZE_NIGHT, HAZE_DAY, Smooth(-0.14f, 0.25f, height)), HAZE_GLOW, 0.8f * glow);
	light.sky = Mix(Mix(SKY_NIGHT, SKY_DUSK, Smooth(-0.25f, 0.0f, height)), SKY_DAY, Smooth(0.0f, 0.30f, height));

	light.stars = Smooth(0.0f, -0.20f, height);
	/* Lamps come on a little before the sun is gone. */
	light.lamps = Smooth(0.14f, -0.04f, height);

	/* People are home and awake in the evening, and some early in the morning. */
	static constexpr std::array<std::array<float, 2>, 8> AWAKE{{{0, 0.30f}, {1.5f, 0.14f}, {4.5f, 0.12f}, {6, 0.45f}, {8, 0.45f}, {17, 0.75f}, {21.5f, 0.75f}, {24, 0.30f}}};
	for (size_t i = 1; i < AWAKE.size(); i++) {
		if (hour > AWAKE[i][0]) continue;
		float t = (hour - AWAKE[i - 1][0]) / (AWAKE[i][0] - AWAKE[i - 1][0]);
		light.windows = AWAKE[i - 1][1] + (AWAKE[i][1] - AWAKE[i - 1][1]) * t;
		break;
	}
	return light;
}
