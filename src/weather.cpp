/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/**
 * @file weather.cpp The weather, the seasons and the hour of the day, as the game knows them.
 *
 * Power plants and vehicles depend on the weather, so it has to be the same on every computer of a game. It is made
 * up, with integers only, from the date, the tick counter, the seed of the map and game settings, so it needs no saving
 * and every computer gets the same answer. The 3D view draws this weather; it only adds what moves by itself, such as
 * the flashes of lightning.
 *
 * Fractions are kept as numbers out of 65536 (Q16) while computing, and handed out as numbers out of 255.
 */

#include "stdafx.h"
#include "weather.h"
#include "landscape_type.h"
#include "openttd.h"
#include "settings_type.h"
#include "timer/timer_game_calendar.h"
#include "timer/timer_game_tick.h"
#include "core/math_func.hpp"

#include "table/strings.h"

#include "safeguards.h"

/** One in Q16. */
static constexpr int64_t ONE = 65536;

/** A fraction written as a decimal number, turned into Q16 by the compiler. */
static consteval int64_t Q(double v)
{
	return static_cast<int64_t>(v * 65536.0 + (v >= 0 ? 0.5 : -0.5));
}

/** How many days one spell of weather lasts. */
static constexpr uint WEATHER_SPELL_DAYS = 24;
/** The part of a spell over which the weather of the spell before turns into its own. */
static constexpr int64_t WEATHER_CHANGE = Q(0.25);
/** The hour of the day a new game starts at. */
static constexpr uint START_HOUR = 9;

/** Goes smoothly from 0 at \a from to ONE at \a to. */
static int64_t Smooth(int64_t from, int64_t to, int64_t x)
{
	int64_t t = Clamp<int64_t>((x - from) * ONE / (to - from), 0, ONE);
	return t * t / ONE * (3 * ONE - 2 * t) / ONE;
}

/**
 * A value that is ONE during a part of the year and goes smoothly to 0 before and after it.
 * @param year How far the year has gone, Q16.
 * @param rise Where it starts to rise.
 * @param full Where it has reached ONE.
 * @param fall Where it starts to fall.
 * @param none Where it is back at 0. All may go beyond ONE for a part of the year that runs into the next year.
 * @return The value, Q16.
 */
static int64_t PartOfYear(int64_t year, int64_t rise, int64_t full, int64_t fall, int64_t none)
{
	int64_t result = 0;
	for (int64_t y : {year - ONE, year, year + ONE}) result = std::max(result, std::min(Smooth(rise, full, y), ONE - Smooth(fall, none, y)));
	return result;
}

/** A number from 0 to ONE - 1 that depends on the map, the spell of weather and what it is for, and on nothing else. */
static int64_t SpellRandom(uint64_t spell, uint salt)
{
	uint64_t x = spell * 0x9E3779B97F4A7C15ULL + _settings_game.game_creation.generation_seed + salt * 0xD1B54A32D192ED03ULL;
	x ^= x >> 30;
	x *= 0xBF58476D1CE4E5B9ULL;
	x ^= x >> 27;
	x *= 0x94D049BB133111EBULL;
	x ^= x >> 31;
	return static_cast<int64_t>(x >> 48);
}

/** The seasons, each Q16. */
struct Seasons {
	int64_t winter = 0; ///< Cold enough for snow.
	int64_t spring = 0; ///< Fresh green.
	int64_t autumn = 0; ///< Leaves in their autumn colours.
	int64_t bare = 0; ///< Leaves gone.
	int64_t dry = 0; ///< Dry grass: late summer, or the dry season of the tropics.
	int64_t rainy = 0; ///< The rainy parts of the year.
};

/**
 * The seasons at a time of the year, for the climate of the game.
 * @param year How far the year has gone, Q16.
 * @return The seasons.
 */
static Seasons GetSeasons(int64_t year)
{
	Seasons s;
	switch (_settings_game.game_creation.landscape) {
		case LandscapeType::Temperate:
			s.winter = PartOfYear(year, Q(0.90), Q(0.96), Q(1.13), Q(1.19));
			s.spring = PartOfYear(year, Q(0.19), Q(0.27), Q(0.34), Q(0.44));
			s.autumn = PartOfYear(year, Q(0.70), Q(0.81), Q(0.87), Q(0.93));
			s.bare = PartOfYear(year, Q(0.86), Q(0.93), Q(1.19), Q(1.27));
			s.dry = Q(0.6) * PartOfYear(year, Q(0.52), Q(0.60), Q(0.68), Q(0.76)) / ONE;
			s.rainy = std::max(PartOfYear(year, Q(0.18), Q(0.25), Q(0.38), Q(0.46)), PartOfYear(year, Q(0.70), Q(0.77), Q(0.88), Q(0.94)));
			break;

		case LandscapeType::Arctic:
			/* A long winter and a short summer. */
			s.winter = PartOfYear(year, Q(0.84), Q(0.91), Q(1.18), Q(1.25));
			s.spring = PartOfYear(year, Q(0.25), Q(0.33), Q(0.40), Q(0.48));
			s.autumn = PartOfYear(year, Q(0.66), Q(0.75), Q(0.82), Q(0.88));
			s.bare = PartOfYear(year, Q(0.81), Q(0.88), Q(1.25), Q(1.33));
			s.dry = Q(0.3) * PartOfYear(year, Q(0.52), Q(0.58), Q(0.64), Q(0.70)) / ONE;
			s.rainy = std::max(PartOfYear(year, Q(0.24), Q(0.30), Q(0.42), Q(0.50)), PartOfYear(year, Q(0.64), Q(0.70), Q(0.82), Q(0.88)));
			break;

		case LandscapeType::Tropic:
			/* No winter: a dry season and a rainy season. */
			s.dry = PartOfYear(year, Q(0.33), Q(0.44), Q(0.76), Q(0.87));
			s.rainy = ONE - s.dry;
			break;

		default:
			/* Toyland has neither seasons nor weather. */
			break;
	}
	return s;
}

/** How likely each kind of weather is, Q16; clear weather takes what is left. */
using WeatherChances = std::array<int64_t, to_underlying(WeatherKind::End)>;

/**
 * How likely each kind of weather is in a season.
 * @param s The seasons.
 * @return The chance of each kind, clear weather left at 0.
 */
static WeatherChances GetWeatherChances(const Seasons &s)
{
	/*                                               Clear Cloudy    Rain      Storm     Snow      Fog */
	static constexpr WeatherChances TEMPERATE_WINTER{0, Q(0.18), Q(0.04), Q(0.00), Q(0.32), Q(0.10)};
	static constexpr WeatherChances TEMPERATE_RAINY{0, Q(0.20), Q(0.24), Q(0.03), Q(0.00), Q(0.09)};
	static constexpr WeatherChances TEMPERATE_SUMMER{0, Q(0.12), Q(0.08), Q(0.07), Q(0.00), Q(0.01)};
	static constexpr WeatherChances ARCTIC_WINTER{0, Q(0.16), Q(0.00), Q(0.00), Q(0.44), Q(0.08)};
	static constexpr WeatherChances ARCTIC_RAINY{0, Q(0.20), Q(0.16), Q(0.00), Q(0.10), Q(0.09)};
	static constexpr WeatherChances ARCTIC_SUMMER{0, Q(0.15), Q(0.15), Q(0.04), Q(0.00), Q(0.03)};
	static constexpr WeatherChances TROPIC_RAINY{0, Q(0.15), Q(0.28), Q(0.15), Q(0.00), Q(0.02)};
	static constexpr WeatherChances TROPIC_DRY{0, Q(0.08), Q(0.01), Q(0.00), Q(0.00), Q(0.00)};

	const WeatherChances *winter, *rainy, *other;
	switch (_settings_game.game_creation.landscape) {
		case LandscapeType::Temperate: winter = &TEMPERATE_WINTER; rainy = &TEMPERATE_RAINY; other = &TEMPERATE_SUMMER; break;
		case LandscapeType::Arctic: winter = &ARCTIC_WINTER; rainy = &ARCTIC_RAINY; other = &ARCTIC_SUMMER; break;
		case LandscapeType::Tropic: winter = &TROPIC_DRY; rainy = &TROPIC_RAINY; other = &TROPIC_DRY; break;
		default: return {};
	}

	int64_t w = s.winter;
	int64_t r = std::min(s.rainy, ONE - w);
	int64_t o = std::max<int64_t>(0, ONE - w - r);
	int64_t often = _settings_game.economy.weather >= to_underlying(WeatherSetting::Much) ? ONE : Q(0.55);
	WeatherChances chances{};
	for (size_t i = 0; i < chances.size(); i++) chances[i] = ((*winter)[i] * w + (*rainy)[i] * r + (*other)[i] * o) / ONE * often / ONE;
	return chances;
}

/** The weather of one spell, Q16. */
struct Spell {
	WeatherKind kind = WeatherKind::Clear; ///< What kind of weather.
	int64_t strength = 0; ///< How much of it, from about a half to ONE.
	int64_t overcast = 0; ///< How much cloud.
	int64_t rain = 0; ///< How much rain.
	int64_t snow = 0; ///< How much snowfall.
	int64_t fog = 0; ///< How much fog.
	int64_t storm = 0; ///< Whether there is lightning.
	int64_t wind = 0; ///< Wind speed; ONE is a full storm.
	int64_t wind_direction = 0; ///< Direction of the wind, ONE for a full turn.
};

/**
 * Fill in what a kind of weather does.
 * @param spell The spell, with its kind and strength set.
 */
static void FillSpell(Spell &spell)
{
	int64_t i = spell.strength;
	switch (spell.kind) {
		case WeatherKind::Cloudy: spell.overcast = Q(0.3) + Q(0.5) * i / ONE; break;
		case WeatherKind::Rain: spell.overcast = Q(0.85); spell.rain = Q(0.25) + Q(0.55) * i / ONE; break;
		case WeatherKind::Storm: spell.overcast = ONE; spell.rain = ONE; spell.storm = ONE; break;
		case WeatherKind::Snow: spell.overcast = Q(0.75); spell.snow = Q(0.3) + Q(0.7) * i / ONE; break;
		case WeatherKind::Fog: spell.overcast = Q(0.45); spell.fog = Q(0.5) + Q(0.5) * i / ONE; break;
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
	int64_t pick = SpellRandom(number, 0);
	for (size_t i = 1; i < chances.size(); i++) {
		if (pick < chances[i]) {
			spell.kind = static_cast<WeatherKind>(i);
			break;
		}
		pick -= chances[i];
	}
	spell.strength = Q(0.5) + SpellRandom(number, 1) / 2;
	FillSpell(spell);

	spell.wind_direction = SpellRandom(number, 2);
	/* Fog comes with calm air, rain with a fresh wind, and a storm blows hard. */
	int64_t r = SpellRandom(number, 3);
	switch (spell.kind) {
		case WeatherKind::Fog: spell.wind = Q(0.02) + Q(0.08) * r / ONE; break;
		case WeatherKind::Rain: case WeatherKind::Snow: spell.wind = Q(0.25) + Q(0.35) * r / ONE; break;
		case WeatherKind::Storm: spell.wind = Q(0.85) + Q(0.15) * r / ONE; break;
		default: spell.wind = Q(0.05) + Q(0.50) * r / ONE; break;
	}
	return spell;
}

/** Ticks a day and a night of the game last, or 0 when there is no day and night. */
uint64_t GetWeatherTicksPerDay()
{
	return static_cast<uint64_t>(_settings_game.economy.day_length) * 60 * 1000 / MILLISECONDS_PER_TICK;
}

/**
 * Sine of an angle, with integers.
 * @param angle The angle, ONE for a full turn.
 * @return The sine, Q16.
 */
static int64_t IntSin(int64_t angle)
{
	angle %= ONE;
	if (angle < 0) angle += ONE;
	bool negative = angle >= ONE / 2;
	if (negative) angle -= ONE / 2;
	/* Bhaskara's approximation on the half turn: 16 x (pi - x) / (5 pi^2 - 4 x (pi - x)), with pi as ONE / 2. */
	int64_t p = angle * (ONE / 2 - angle) / ONE;
	int64_t s = 16 * p * ONE / (5 * (ONE / 2) * (ONE / 2) / ONE - 4 * p);
	return negative ? -s : s;
}

/**
 * Work out the weather, the season and the hour at this moment.
 * @return The weather.
 */
static GameWeather ComputeGameWeather()
{
	GameWeather weather;
	if (_game_mode == GameMode::Bootstrap) return weather;

	/* How far the year has gone. */
	TimerGameCalendar::YearMonthDay ymd = TimerGameCalendar::ConvertDateToYMD(TimerGameCalendar::date);
	int64_t day = (TimerGameCalendar::date - TimerGameCalendar::ConvertYMDToDate(ymd.year, 0, 1)).base();
	int64_t days = TimerGameCalendar::IsLeapYear(ymd.year) ? 366 : 365;
	int64_t year = (day * ONE + static_cast<int64_t>(TimerGameCalendar::date_fract) * ONE / Ticks::DAY_TICKS) / days;
	Seasons seasons = GetSeasons(year);

	auto out = [](int64_t v) { return static_cast<uint8_t>(Clamp<int64_t>(v * 255 / ONE, 0, 255)); };
	weather.autumn = out(seasons.autumn);
	weather.bare = out(seasons.bare);
	weather.spring = out(seasons.spring);
	weather.dry = out(seasons.dry);
	weather.winter = out(seasons.winter);

	/* The hour: a day of a number of minutes of play, starting in the morning. */
	uint64_t ticks_per_day = GetWeatherTicksPerDay();
	weather.has_day = ticks_per_day != 0;
	if (weather.has_day) {
		uint64_t now = TimerGameTick::counter % ticks_per_day;
		weather.hour = static_cast<uint16_t>((START_HOUR * 256 + now * 24 * 256 / ticks_per_day) % (24 * 256));
		/* The sun rises at six and sets at six; its path leans to the south, so at noon it stands 55 degrees high. */
		int64_t angle = (static_cast<int64_t>(weather.hour) - 6 * 256) * ONE / (24 * 256);
		weather.sun = static_cast<int16_t>(IntSin(angle) * Q(0.819) / ONE * 256 / ONE);
	} else {
		weather.sun = static_cast<int16_t>(Q(0.819) * 256 / ONE);
	}

	/* Without weather, snow lies in the middle of the winter. */
	int64_t winter_snow = _settings_game.game_creation.landscape == LandscapeType::Arctic ? ONE : Q(0.8);
	int64_t cover = winter_snow * Clamp<int64_t>(seasons.winter * 2 - ONE, 0, ONE) / ONE;
	weather.cover = out(cover);
	if (_settings_game.economy.weather == to_underlying(WeatherSetting::Off)) {
		weather.wind = out(Q(0.3));
		return weather;
	}

	WeatherChances chances = GetWeatherChances(seasons);
	uint64_t spell_ticks = static_cast<uint64_t>(Ticks::DAY_TICKS) * WEATHER_SPELL_DAYS;
	uint64_t number = TimerGameTick::counter / spell_ticks;
	int64_t part = static_cast<int64_t>(TimerGameTick::counter % spell_ticks) * ONE / static_cast<int64_t>(spell_ticks);

	/* The weather of the spell before turns into that of this spell. */
	Spell now = GetSpell(number, chances);
	Spell before = number == 0 ? Spell{} : GetSpell(number - 1, chances);
	int64_t t = Smooth(0, WEATHER_CHANGE, part);
	auto mix = [t](int64_t a, int64_t b) { return a + (b - a) * t / ONE; };
	weather.kind = t >= ONE / 2 ? now.kind : before.kind;
	weather.overcast = out(mix(before.overcast, now.overcast));
	weather.rain = out(mix(before.rain, now.rain));
	weather.snow = out(mix(before.snow, now.snow));
	weather.fog = out(mix(before.fog, now.fog));
	weather.storm = out(mix(before.storm, now.storm));
	weather.wind = out(mix(before.wind, now.wind));
	/* The wind turns the short way round. */
	int64_t turn = now.wind_direction - before.wind_direction;
	if (turn > ONE / 2) turn -= ONE;
	if (turn < -ONE / 2) turn += ONE;
	weather.wind_direction = static_cast<uint8_t>(((before.wind_direction + turn * t / ONE) % ONE + ONE) % ONE * 256 / ONE);

	/* Snow on the ground and wet ground come from the weather of the spells before: snow piles up while it snows
	 * and melts slowly in winter, faster outside it and in the rain; the ground gets wet in the rain and dries after it. */
	static constexpr uint LOOK_BACK = 10;
	int64_t lying = 0;
	int64_t wet = 0;
	for (uint i = LOOK_BACK + 1; i-- > 0;) {
		if (number < i) continue;
		Spell spell = i == 0 ? now : GetSpell(number - i, chances);
		int64_t duration = i == 0 ? part : ONE;
		if (spell.snow > 0) {
			lying += Q(0.9) * spell.snow / ONE * duration / ONE;
		} else if (spell.rain > 0) {
			lying -= Q(0.9) * duration / ONE;
		} else {
			lying -= (Q(0.12) + Q(1.3) * (ONE - seasons.winter) / ONE) * duration / ONE;
		}
		lying = Clamp<int64_t>(lying, 0, ONE);
		wet += (spell.rain > 0 ? 3 * spell.rain : -Q(1.5)) * duration / ONE;
		wet = Clamp<int64_t>(wet, 0, ONE);
	}
	/* In the middle of winter there is always a little snow left. */
	weather.cover = out(std::max(lying, Q(0.4) * cover / ONE));
	weather.wet = out(wet);
	return weather;
}

/**
 * Get the weather, the season and the hour at this moment of the game.
 * @return The weather; the same on every computer of the game.
 */
const GameWeather &GetGameWeather()
{
	static GameWeather cache;
	static uint64_t cache_tick = UINT64_MAX;
	static TimerGameCalendar::Date cache_date{};
	static uint32_t cache_seed = 0;
	static std::array<uint8_t, 3> cache_settings{};
	std::array<uint8_t, 3> settings{_settings_game.economy.weather, _settings_game.economy.day_length, to_underlying(_settings_game.game_creation.landscape)};
	if (cache_tick != TimerGameTick::counter || cache_date != TimerGameCalendar::date || cache_seed != _settings_game.game_creation.generation_seed || cache_settings != settings) {
		cache = ComputeGameWeather();
		cache_tick = TimerGameTick::counter;
		cache_date = TimerGameCalendar::date;
		cache_seed = _settings_game.game_creation.generation_seed;
		cache_settings = settings;
	}
	return cache;
}

/**
 * How much of its capacity a solar plant gives in some weather: the height of the sun, less under clouds and fog,
 * nothing at night or while snow lies on the panels. Without day and night, the average of a day.
 * @param weather The weather.
 * @return From 0 to 255.
 */
uint8_t GetWeatherSolarFactor(const GameWeather &weather)
{
	int sun = weather.has_day ? std::max<int>(0, weather.sun) * 255 / 210 : 82;
	if (weather.cover > 128) return 0;
	/* Full overcast leaves about a quarter; fog takes a little more. */
	int light = 256 - weather.overcast * 3 / 4 - weather.fog / 8;
	return static_cast<uint8_t>(std::clamp(sun * light / 256, 0, 255));
}

/**
 * How much of its capacity a wind turbine gives in some weather: nothing in a calm, rising to full in a fresh wind,
 * and nothing in a storm, when turbines are stopped to protect them.
 * @param weather The weather.
 * @return From 0 to 255.
 */
uint8_t GetWeatherWindFactor(const GameWeather &weather)
{
	static constexpr int CUT_IN = 20;
	static constexpr int RATED = 150;
	static constexpr int CUT_OUT = 225;
	int wind = weather.wind;
	if (wind < CUT_IN || wind >= CUT_OUT) return 0;
	if (wind >= RATED) return 255;
	/* Power grows with the cube of the wind speed. */
	int64_t f = static_cast<int64_t>(wind - CUT_IN) * 255 / (RATED - CUT_IN);
	return static_cast<uint8_t>(f * f * f / (255 * 255));
}

/**
 * How much water the rivers carry: more after rain and in the spring thaw, less in a dry season and in a hard winter.
 * @param weather The weather.
 * @return From about 100 (low) through 170 (normal) to 255 (high).
 */
uint8_t GetWeatherRiverFlow(const GameWeather &weather)
{
	int flow = 170 + weather.wet * 50 / 255 + weather.spring * 35 / 255 - weather.dry * 60 / 255;
	if (_settings_game.game_creation.landscape == LandscapeType::Arctic) flow -= weather.winter * 60 / 255;
	return static_cast<uint8_t>(std::clamp(flow, 100, 255));
}

/**
 * How fast vehicles may go in the weather now, when the setting economy.weather_slows_vehicles is on.
 * @return Speed factor out of 256.
 */
uint GetWeatherVehicleSpeedFactor()
{
	if (!_settings_game.economy.weather_slows_vehicles) return 256;
	const GameWeather &weather = GetGameWeather();
	/* Up to 15% slower in heavy rain, 25% in a thunderstorm and 30% in a snowstorm. */
	uint loss = std::max({weather.rain * 38u / 255, weather.storm * 64u / 255, weather.snow * 77u / 255});
	return 256 - loss;
}

/**
 * How much faster vehicles wear in the weather now, when the setting economy.weather_slows_vehicles is on.
 * @return Factor out of 256 for the daily loss of reliability.
 */
uint GetWeatherBreakdownFactor()
{
	if (!_settings_game.economy.weather_slows_vehicles) return 256;
	const GameWeather &weather = GetGameWeather();
	/* Up to half as fast again in heavy rain, and twice as fast in a thunderstorm or a snowstorm. */
	uint extra = std::max({weather.rain * 128u / 255, weather.storm * 256u / 255, weather.snow * 256u / 255});
	return 256 + extra;
}

/**
 * The name of the weather, as the status bar shows it.
 * @param weather The weather.
 * @return The name.
 */
StringID GetWeatherName(const GameWeather &weather)
{
	auto grade = [](uint8_t v, StringID light, StringID normal, StringID heavy) { return v < 90 ? light : (v < 180 ? normal : heavy); };
	switch (weather.kind) {
		case WeatherKind::Storm: if (weather.storm >= 128) return STR_WEATHER_THUNDERSTORM; [[fallthrough]];
		case WeatherKind::Rain: if (weather.rain > 20) return grade(weather.rain, STR_WEATHER_LIGHT_RAIN, STR_WEATHER_RAIN, STR_WEATHER_HEAVY_RAIN); break;
		case WeatherKind::Snow: if (weather.snow > 20) return grade(weather.snow, STR_WEATHER_LIGHT_SNOW, STR_WEATHER_SNOW, STR_WEATHER_HEAVY_SNOW); break;
		case WeatherKind::Fog: if (weather.fog > 60) return STR_WEATHER_FOG; break;
		default: break;
	}
	if (weather.overcast >= 140) return STR_WEATHER_CLOUDY;
	if (weather.has_day && weather.sun <= 0) return STR_WEATHER_CLEAR;
	return STR_WEATHER_SUNNY;
}
