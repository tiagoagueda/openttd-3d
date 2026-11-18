/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file weather3d.cpp Weather and seasons of the 3D viewport.
 *
 * The season follows the date of the game. The weather is made up from the time the game has run and the seed of the map,
 * so it is the same whenever it is asked for and for every view, without the game having to keep or save anything.
 * Neither changes anything in the game itself.
 */

#include "../stdafx.h"
#include "weather3d.h"
#include "../core/math_func.hpp"
#include "../landscape_type.h"
#include "../openttd.h"
#include "../settings_type.h"
#include "../timer/timer_game_calendar.h"
#include "../timer/timer_game_tick.h"

#include <chrono>

#include "../safeguards.h"

/** How many days one spell of weather lasts. */
static constexpr uint WEATHER_SPELL_DAYS = 24;
/** The part of a spell over which the weather of the spell before turns into its own. */
static constexpr float WEATHER_CHANGE = 0.25f;

static Weather3DKind _weather_override = Weather3DKind::Auto; ///< The weather asked for with the console, or Auto.
static float _cover_override = -1; ///< The snow on the ground asked for with the console, or negative for none.
static int _month_override = -1; ///< The month asked for with the console, from 0, or negative for the month of the game.

/**
 * Ask for one kind of weather, whatever the season; for testing and for pictures.
 * @param kind The weather, or Weather3DKind::Auto to let it change by itself again.
 * @param cover How much snow lies on the ground, from 0 to 1, or negative to let the weather decide.
 */
void SetWeather3DOverride(Weather3DKind kind, float cover)
{
	_weather_override = kind;
	_cover_override = cover;
}

/**
 * Ask for the season of one month, whatever the date of the game; for testing and for pictures.
 * @param month The month, from 0 for January, or negative to follow the date of the game again.
 */
void SetSeason3DOverride(int month)
{
	_month_override = month < 0 ? -1 : month % 12;
}

/**
 * A value that is 1 during a part of the year and goes smoothly to 0 before and after it.
 * @param year How far the year has gone, from 0 to 1.
 * @param rise Where it starts to rise.
 * @param full Where it has reached 1.
 * @param fall Where it starts to fall.
 * @param none Where it is back at 0. All may go beyond 1 for a part of the year that runs into the next year.
 * @return The value.
 */
static float PartOfYear(float year, float rise, float full, float fall, float none)
{
	auto smooth = [](float edge0, float edge1, float x) {
		float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
		return t * t * (3 - 2 * t);
	};
	float result = 0;
	for (float y : {year - 1, year, year + 1}) result = std::max(result, std::min(smooth(rise, full, y), 1 - smooth(fall, none, y)));
	return result;
}

/** A number from 0 to 1 that depends on the map, the spell of weather and what it is for, and on nothing else. */
static float SpellRandom(uint64_t spell, uint salt)
{
	uint64_t x = spell * 0x9E3779B97F4A7C15ULL + _settings_game.game_creation.generation_seed + salt * 0xD1B54A32D192ED03ULL;
	x ^= x >> 30;
	x *= 0xBF58476D1CE4E5B9ULL;
	x ^= x >> 27;
	x *= 0x94D049BB133111EBULL;
	x ^= x >> 31;
	return static_cast<float>(x >> 40) / static_cast<float>(1 << 24);
}

/** The seasons, each from 0 to 1. */
struct Seasons {
	float winter = 0; ///< Cold enough for snow.
	float spring = 0; ///< Fresh green.
	float autumn = 0; ///< Leaves in their autumn colours.
	float bare = 0; ///< Leaves gone.
	float dry = 0; ///< Dry grass: late summer, or the dry season of the tropics.
	float rainy = 0; ///< The rainy parts of the year.
};

/**
 * The seasons at a time of the year, for the climate of the game.
 * @param year How far the year has gone, from 0 to 1.
 * @return The seasons.
 */
static Seasons GetSeasons(float year)
{
	Seasons s;
	switch (_settings_game.game_creation.landscape) {
		case LandscapeType::Temperate:
			s.winter = PartOfYear(year, 0.90f, 0.96f, 1.13f, 1.19f);
			s.spring = PartOfYear(year, 0.19f, 0.27f, 0.34f, 0.44f);
			s.autumn = PartOfYear(year, 0.70f, 0.81f, 0.87f, 0.93f);
			s.bare = PartOfYear(year, 0.86f, 0.93f, 1.19f, 1.27f);
			s.dry = 0.6f * PartOfYear(year, 0.52f, 0.60f, 0.68f, 0.76f);
			s.rainy = std::max(PartOfYear(year, 0.18f, 0.25f, 0.38f, 0.46f), PartOfYear(year, 0.70f, 0.77f, 0.88f, 0.94f));
			break;

		case LandscapeType::Arctic:
			/* A long winter and a short summer. */
			s.winter = PartOfYear(year, 0.84f, 0.91f, 1.18f, 1.25f);
			s.spring = PartOfYear(year, 0.25f, 0.33f, 0.40f, 0.48f);
			s.autumn = PartOfYear(year, 0.66f, 0.75f, 0.82f, 0.88f);
			s.bare = PartOfYear(year, 0.81f, 0.88f, 1.25f, 1.33f);
			s.dry = 0.3f * PartOfYear(year, 0.52f, 0.58f, 0.64f, 0.70f);
			s.rainy = std::max(PartOfYear(year, 0.24f, 0.30f, 0.42f, 0.50f), PartOfYear(year, 0.64f, 0.70f, 0.82f, 0.88f));
			break;

		case LandscapeType::Tropic:
			/* No winter: a dry season and a rainy season. */
			s.dry = PartOfYear(year, 0.33f, 0.44f, 0.76f, 0.87f);
			s.rainy = 1 - s.dry;
			break;

		default:
			/* Toyland has neither seasons nor weather. */
			break;
	}
	return s;
}

/** How likely each kind of weather is; clear weather takes what is left. */
using WeatherChances = std::array<float, to_underlying(Weather3DKind::End)>;

/**
 * How likely each kind of weather is in a season.
 * @param s The seasons.
 * @return The chance of each kind, clear weather left at 0.
 */
static WeatherChances GetWeatherChances(const Seasons &s)
{
	/*                                          Clear Cloudy Rain   Storm  Snow   Fog */
	static constexpr WeatherChances TEMPERATE_WINTER{0, 0.18f, 0.04f, 0.00f, 0.32f, 0.10f};
	static constexpr WeatherChances TEMPERATE_RAINY{0, 0.20f, 0.24f, 0.03f, 0.00f, 0.09f};
	static constexpr WeatherChances TEMPERATE_SUMMER{0, 0.12f, 0.08f, 0.07f, 0.00f, 0.01f};
	static constexpr WeatherChances ARCTIC_WINTER{0, 0.16f, 0.00f, 0.00f, 0.44f, 0.08f};
	static constexpr WeatherChances ARCTIC_RAINY{0, 0.20f, 0.16f, 0.00f, 0.10f, 0.09f};
	static constexpr WeatherChances ARCTIC_SUMMER{0, 0.15f, 0.15f, 0.04f, 0.00f, 0.03f};
	static constexpr WeatherChances TROPIC_RAINY{0, 0.15f, 0.28f, 0.15f, 0.00f, 0.02f};
	static constexpr WeatherChances TROPIC_DRY{0, 0.08f, 0.01f, 0.00f, 0.00f, 0.00f};

	const WeatherChances *winter, *rainy, *other;
	switch (_settings_game.game_creation.landscape) {
		case LandscapeType::Temperate: winter = &TEMPERATE_WINTER; rainy = &TEMPERATE_RAINY; other = &TEMPERATE_SUMMER; break;
		case LandscapeType::Arctic: winter = &ARCTIC_WINTER; rainy = &ARCTIC_RAINY; other = &ARCTIC_SUMMER; break;
		case LandscapeType::Tropic: winter = &TROPIC_DRY; rainy = &TROPIC_RAINY; other = &TROPIC_DRY; break;
		default: return {};
	}

	float w = s.winter;
	float r = std::min(s.rainy, 1 - w);
	float o = std::max(0.0f, 1 - w - r);
	float often = _settings_client.gui.view3d_weather >= to_underlying(View3DWeatherSetting::Frequent) ? 1.0f : 0.55f;
	WeatherChances chances{};
	for (size_t i = 0; i < chances.size(); i++) chances[i] = ((*winter)[i] * w + (*rainy)[i] * r + (*other)[i] * o) * often;
	return chances;
}

/** The weather of one spell. */
struct Spell {
	Weather3DKind kind = Weather3DKind::Clear; ///< What kind of weather.
	float strength = 0; ///< How much of it, from about a half to 1.
	float overcast = 0; ///< How much cloud.
	float rain = 0; ///< How much rain.
	float snow = 0; ///< How much snowfall.
	float fog = 0; ///< How much fog.
	float storm = 0; ///< Whether there is lightning.
	float wind_x = 0; ///< Wind along x.
	float wind_y = 0; ///< Wind along y.
};

/**
 * Fill in what a kind of weather looks like.
 * @param spell The spell, with its kind and strength set.
 */
static void FillSpell(Spell &spell)
{
	float i = spell.strength;
	switch (spell.kind) {
		case Weather3DKind::Cloudy: spell.overcast = 0.3f + 0.5f * i; break;
		case Weather3DKind::Rain: spell.overcast = 0.85f; spell.rain = 0.25f + 0.55f * i; break;
		case Weather3DKind::Storm: spell.overcast = 1.0f; spell.rain = 1.0f; spell.storm = 1.0f; break;
		case Weather3DKind::Snow: spell.overcast = 0.75f; spell.snow = 0.3f + 0.7f * i; break;
		case Weather3DKind::Fog: spell.overcast = 0.45f; spell.fog = 0.5f + 0.5f * i; break;
		default: break;
	}
}

/**
 * Make up the weather of a spell.
 * @param number Which spell, counted from the start of the game.
 * @param chances How likely each kind of weather is.
 * @return The weather.
 */
static Spell GetSpell(uint64_t number, const WeatherChances &chances)
{
	Spell spell;
	float pick = SpellRandom(number, 0);
	for (size_t i = 1; i < chances.size(); i++) {
		if (pick < chances[i]) {
			spell.kind = static_cast<Weather3DKind>(i);
			break;
		}
		pick -= chances[i];
	}
	spell.strength = 0.5f + 0.5f * SpellRandom(number, 1);
	FillSpell(spell);

	float direction = SpellRandom(number, 2) * 6.2831853f;
	float speed = (0.10f + 0.25f * SpellRandom(number, 3)) * (spell.kind == Weather3DKind::Storm ? 2.0f : 1.0f);
	spell.wind_x = std::cos(direction) * speed;
	spell.wind_y = std::sin(direction) * speed;
	return spell;
}

/**
 * The light of lightning at this moment: now and then a flash that flickers and is gone.
 * @return From 0 for none to 1.
 */
static float GetLightning()
{
	static const auto start = std::chrono::steady_clock::now();
	double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	/* Every few seconds there may be a flash, somewhere in the first half of those seconds. */
	static constexpr double SLOT = 4.0;
	uint64_t slot = static_cast<uint64_t>(seconds / SLOT);
	if (SpellRandom(slot, 7) > 0.6f) return 0;
	float since = static_cast<float>(seconds - slot * SLOT) - SpellRandom(slot, 8) * 2.0f;
	if (since < 0 || since > 0.6f) return 0;
	/* Two strokes shortly after each other. */
	float light = std::exp(-since * 14.0f) + (since > 0.18f ? 0.7f * std::exp(-(since - 0.18f) * 18.0f) : 0.0f);
	return std::min(light, 1.0f);
}

/**
 * The weather and the season at this moment of the game.
 * @return What they do to an image of the 3D view.
 */
Weather3D GetWeather3D()
{
	Weather3D weather;
	uint8_t setting = _settings_client.gui.view3d_weather;
	bool forced = _weather_override != Weather3DKind::Auto || _month_override >= 0 || _cover_override >= 0;
	if (setting == to_underlying(View3DWeatherSetting::Off) && !forced) return weather;
	if (_game_mode == GameMode::Bootstrap) return weather;

	/* How far the year has gone. */
	float year;
	if (_month_override >= 0) {
		year = (_month_override + 0.5f) / 12.0f;
	} else {
		TimerGameCalendar::YearMonthDay ymd = TimerGameCalendar::ConvertDateToYMD(TimerGameCalendar::date);
		int day = (TimerGameCalendar::date - TimerGameCalendar::ConvertYMDToDate(ymd.year, 0, 1)).base();
		year = (day + static_cast<float>(TimerGameCalendar::date_fract) / Ticks::DAY_TICKS) / (TimerGameCalendar::IsLeapYear(ymd.year) ? 366.0f : 365.0f);
	}
	Seasons seasons = GetSeasons(year);
	weather.autumn = seasons.autumn;
	weather.bare = seasons.bare;
	weather.spring = seasons.spring;
	weather.dry = seasons.dry;

	/* Without weather, snow lies in the middle of the winter. */
	float winter_snow = _settings_game.game_creation.landscape == LandscapeType::Arctic ? 1.0f : 0.8f;
	weather.cover = winter_snow * std::clamp(seasons.winter * 2 - 1, 0.0f, 1.0f);

	if (_weather_override != Weather3DKind::Auto) {
		Spell spell;
		spell.kind = _weather_override;
		spell.strength = 0.8f;
		FillSpell(spell);
		weather.overcast = spell.overcast;
		weather.rain = spell.rain;
		weather.snow = spell.snow;
		weather.fog = spell.fog;
		weather.flash = spell.storm * GetLightning();
		weather.wind_x = 0.22f;
		weather.wind_y = 0.12f;
		if (spell.rain > 0) weather.wet = 1;
		if (spell.snow > 0) weather.cover = 1;
	} else if (setting >= to_underlying(View3DWeatherSetting::Occasional)) {
		WeatherChances chances = GetWeatherChances(seasons);
		double spells = static_cast<double>(TimerGameTick::counter) / (static_cast<double>(Ticks::DAY_TICKS) * WEATHER_SPELL_DAYS);
		uint64_t number = static_cast<uint64_t>(spells);
		float part = static_cast<float>(spells - number);

		/* The weather of the spell before turns into that of this spell. */
		Spell now = GetSpell(number, chances);
		Spell before = number == 0 ? Spell{} : GetSpell(number - 1, chances);
		float t = std::clamp(part / WEATHER_CHANGE, 0.0f, 1.0f);
		t = t * t * (3 - 2 * t);
		auto mix = [t](float a, float b) { return a + (b - a) * t; };
		weather.overcast = mix(before.overcast, now.overcast);
		weather.rain = mix(before.rain, now.rain);
		weather.snow = mix(before.snow, now.snow);
		weather.fog = mix(before.fog, now.fog);
		weather.wind_x = mix(before.wind_x, now.wind_x);
		weather.wind_y = mix(before.wind_y, now.wind_y);
		float storm = mix(before.storm, now.storm);
		if (storm > 0.3f) weather.flash = storm * GetLightning();

		/* Snow on the ground and wet ground come from the weather of the spells before: snow piles up while it snows
		 * and melts slowly in winter, faster outside it and in the rain; the ground gets wet in the rain and dries after it. */
		static constexpr uint LOOK_BACK = 10;
		float cover = 0;
		float wet = 0;
		for (uint i = LOOK_BACK + 1; i-- > 0;) {
			if (number < i) continue;
			Spell spell = i == 0 ? now : GetSpell(number - i, chances);
			float duration = i == 0 ? part : 1.0f;
			if (spell.snow > 0) {
				cover += 0.9f * spell.snow * duration;
			} else if (spell.rain > 0) {
				cover -= 0.9f * duration;
			} else {
				cover -= (0.12f + 1.3f * (1 - seasons.winter)) * duration;
			}
			cover = std::clamp(cover, 0.0f, 1.0f);
			wet += (spell.rain > 0 ? 3.0f * spell.rain : -1.5f) * duration;
			wet = std::clamp(wet, 0.0f, 1.0f);
		}
		/* In the middle of winter there is always a little snow left. */
		weather.cover = std::max(cover, 0.4f * weather.cover);
		weather.wet = wet;
	}

	if (_cover_override >= 0) weather.cover = std::min(_cover_override, 1.0f);
	return weather;
}
