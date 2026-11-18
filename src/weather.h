/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file weather.h The weather, the seasons and the hour of the day, as the game knows them. */

#ifndef WEATHER_H
#define WEATHER_H

#include "strings_type.h"

/** The kinds of weather. */
enum class WeatherKind : uint8_t {
	Clear, ///< Blue sky.
	Cloudy, ///< A grey sky and soft light.
	Rain, ///< Clouds and rain.
	Storm, ///< Dark clouds, heavy rain and lightning.
	Snow, ///< Clouds and falling snow.
	Fog, ///< Fog that hides what is far away.
	End, ///< Number of kinds.
	Auto = End, ///< Not a kind: let the weather change by itself.
};

/** Values of the game setting economy.weather. */
enum class WeatherSetting : uint8_t {
	Off, ///< Always clear weather; the seasons still change.
	Some, ///< Now and then some weather.
	Much, ///< Weather much of the time.
};

/**
 * The weather, the season and the hour at one moment of the game. The same on every computer in a game: it is made
 * only from the date, the tick counter, the map seed and game settings, with integers. Values from 0 (none) to 255 (full).
 */
struct GameWeather {
	WeatherKind kind = WeatherKind::Clear; ///< The kind of weather of the current spell.
	uint8_t overcast = 0; ///< How much cloud covers the sky.
	uint8_t rain = 0; ///< How hard it rains.
	uint8_t snow = 0; ///< How hard it snows.
	uint8_t fog = 0; ///< How thick the fog is.
	uint8_t storm = 0; ///< How much of a thunderstorm there is.
	uint8_t wet = 0; ///< How wet the ground is.
	uint8_t cover = 0; ///< How much snow lies on the ground.
	uint8_t wind = 0; ///< Wind speed; 255 is a full storm.
	uint8_t wind_direction = 0; ///< Direction the wind blows to, 256 steps for a full turn; 0 is along x.
	uint8_t autumn = 0; ///< How far the leaves have turned to their autumn colours.
	uint8_t bare = 0; ///< How far the leaves are gone.
	uint8_t spring = 0; ///< How fresh the green of spring is.
	uint8_t dry = 0; ///< How dry the land is: late summer, or the dry season of the tropics.
	uint8_t winter = 0; ///< How deep in the winter the year is.
	uint16_t hour = 12 * 256; ///< Hour of the day in 1/256 hours, from 0 to 24 * 256; noon when there is no day and night.
	int16_t sun = 0; ///< Height of the sun, from -256 (straight down) to 256 (straight up); 0 at sunrise and sunset.
	bool has_day = false; ///< Whether there is a day and night (the setting economy.day_length is not 0).
};

const GameWeather &GetGameWeather();
uint8_t GetWeatherSolarFactor(const GameWeather &weather);
uint8_t GetWeatherWindFactor(const GameWeather &weather);
uint8_t GetWeatherRiverFlow(const GameWeather &weather);
uint GetWeatherVehicleSpeedFactor();
uint GetWeatherBreakdownFactor();
uint64_t GetWeatherTicksPerDay();
StringID GetWeatherName(const GameWeather &weather);

#endif /* WEATHER_H */
