/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file weather3d.h Weather and seasons of the 3D viewport. They are only looks: the game itself knows nothing of them. */

#ifndef VIEWPORT3D_WEATHER3D_H
#define VIEWPORT3D_WEATHER3D_H

/** Values of the setting gui.view3d_weather. */
enum class View3DWeatherSetting : uint8_t {
	Off, ///< Always a clear summer day.
	Seasons, ///< The seasons change the colours of the land; no weather.
	Occasional, ///< Seasons, and now and then some weather.
	Frequent, ///< Seasons, and weather much of the time.
};

/** The kinds of weather. */
enum class Weather3DKind : uint8_t {
	Clear, ///< Blue sky.
	Cloudy, ///< A grey sky and soft light.
	Rain, ///< Clouds and rain.
	Storm, ///< Dark clouds, heavy rain and lightning.
	Snow, ///< Clouds and falling snow.
	Fog, ///< Fog that hides what is far away.
	End, ///< Number of kinds.
	Auto = End, ///< Not a kind: let the weather change by itself.
};

/** What the weather and the season do to an image of the 3D view; everything goes from 0 (not at all) to 1 (fully). */
struct Weather3D {
	float overcast = 0; ///< How much cloud covers the sky.
	float rain = 0; ///< How hard it rains.
	float snow = 0; ///< How hard it snows.
	float fog = 0; ///< How thick the fog is.
	float flash = 0; ///< Light of a flash of lightning.
	float wet = 0; ///< How wet the ground is.
	float cover = 0; ///< How much snow lies on the ground.
	float autumn = 0; ///< How far the leaves have turned to their autumn colours.
	float bare = 0; ///< How far the leaves are gone.
	float spring = 0; ///< How fresh the green of spring is.
	float dry = 0; ///< How dry and yellow the grass is.
	float wind_x = 0; ///< How far rain and snow drift along x for each unit they fall.
	float wind_y = 0; ///< The same along y.

	/** Whether something moves by itself, so images have to be made all the time. */
	bool IsMoving() const { return this->rain > 0.005f || this->snow > 0.005f || this->flash > 0.005f; }

	/** Whether an image made with other weather would look the same; what moves by itself is not looked at. */
	bool LooksLike(const Weather3D &other) const
	{
		auto is_near = [](float a, float b) { return std::abs(a - b) < 0.004f; };
		return is_near(this->overcast, other.overcast) && is_near(this->fog, other.fog) && is_near(this->wet, other.wet) && is_near(this->cover, other.cover) &&
				is_near(this->autumn, other.autumn) && is_near(this->bare, other.bare) && is_near(this->spring, other.spring) && is_near(this->dry, other.dry) &&
				is_near(this->rain, other.rain) && is_near(this->snow, other.snow);
	}
};

Weather3D GetWeather3D();
void SetWeather3DOverride(Weather3DKind kind, float cover = -1);
void SetSeason3DOverride(int month);

#endif /* VIEWPORT3D_WEATHER3D_H */
