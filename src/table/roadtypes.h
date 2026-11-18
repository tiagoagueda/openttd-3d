/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file roadtypes.h All the roadtype-specific information is stored here. */

#ifndef ROADTYPES_H
#define ROADTYPES_H

#include "table/strings.h"

/**
 * Global Roadtype definition
 */
static const RoadTypeInfo _original_roadtypes[] = {
	/* Road */
	{
		/* GUI sprites */
		{
			SPR_IMG_ROAD_X_DIR,
			SPR_IMG_ROAD_Y_DIR,
			SPR_IMG_AUTOROAD,
			SPR_IMG_ROAD_DEPOT,
			SPR_IMG_ROAD_TUNNEL,
			SPR_IMG_CONVERT_ROAD,
		},

		{
			SPR_CURSOR_ROAD_NESW,
			SPR_CURSOR_ROAD_NWSE,
			SPR_CURSOR_AUTOROAD,
			SPR_CURSOR_ROAD_DEPOT,
			SPR_CURSOR_ROAD_TUNNEL,
			SPR_CURSOR_CONVERT_ROAD,
		},

		/* strings */
		{
			STR_ROAD_NAME_ROAD,
			STR_ROAD_TOOLBAR_ROAD_CONSTRUCTION_CAPTION,
			STR_ROAD_MENU_ROAD_CONSTRUCTION,
			STR_BUY_VEHICLE_ROAD_VEHICLE_CAPTION,
			STR_REPLACE_ROAD_VEHICLES,
			STR_ENGINE_PREVIEW_ROAD_VEHICLE,

			STR_ERROR_CAN_T_BUILD_ROAD_HERE,
			STR_ERROR_CAN_T_REMOVE_ROAD_FROM,
			STR_ERROR_CAN_T_BUILD_ROAD_DEPOT,
			{ STR_ERROR_CAN_T_BUILD_BUS_STATION,         STR_ERROR_CAN_T_BUILD_TRUCK_STATION },
			{ STR_ERROR_CAN_T_REMOVE_BUS_STATION,        STR_ERROR_CAN_T_REMOVE_TRUCK_STATION },
			STR_ERROR_CAN_T_CONVERT_ROAD,
			{ STR_STATION_BUILD_BUS_ORIENTATION,         STR_STATION_BUILD_TRUCK_ORIENTATION },
			{ STR_STATION_BUILD_BUS_ORIENTATION_TOOLTIP, STR_STATION_BUILD_TRUCK_ORIENTATION_TOOLTIP },
		},

		/* Powered roadtypes: buses and lorries also drive on electrified road */
		{ROADTYPE_ROAD, ROADTYPE_TROLLEY},

		/* flags */
		{RoadTypeFlag::TownBuild},

		/* cost multiplier */
		8,

		/* maintenance cost multiplier */
		16,

		/* max speed */
		0,

		/* road type label */
		ROADTYPE_LABEL_ROAD,

		/* alternate labels */
		{},

		/* map colour */
		PC_BLACK,

		/* introduction date */
		CalendarTime::MIN_DATE,

		/* roadtypes required for this to be introduced */
		{},

		/* introduction road types */
		{ROADTYPE_ROAD},

		/* sort order */
		0x07,

		{ nullptr },
		{ nullptr },
		{},
	},

	/* Electrified Tram */
	{
		/* GUI sprites */
		{
			SPR_IMG_TRAMWAY_X_DIR,
			SPR_IMG_TRAMWAY_Y_DIR,
			SPR_IMG_AUTOTRAM,
			SPR_IMG_ROAD_DEPOT,
			SPR_IMG_ROAD_TUNNEL,
			SPR_IMG_CONVERT_TRAM,
		},

		{
			SPR_CURSOR_TRAMWAY_NESW,
			SPR_CURSOR_TRAMWAY_NWSE,
			SPR_CURSOR_AUTOTRAM,
			SPR_CURSOR_ROAD_DEPOT,
			SPR_CURSOR_ROAD_TUNNEL,
			SPR_CURSOR_CONVERT_TRAM,
		},

		/* strings */
		{
			STR_ROAD_NAME_TRAM,
			STR_ROAD_TOOLBAR_TRAM_CONSTRUCTION_CAPTION,
			STR_ROAD_MENU_TRAM_CONSTRUCTION,
			STR_BUY_VEHICLE_TRAM_VEHICLE_CAPTION,
			STR_REPLACE_TRAM_VEHICLES,
			STR_ENGINE_PREVIEW_TRAM_VEHICLE,

			STR_ERROR_CAN_T_BUILD_TRAMWAY_HERE,
			STR_ERROR_CAN_T_REMOVE_TRAMWAY_FROM,
			STR_ERROR_CAN_T_BUILD_TRAM_DEPOT,
			{ STR_ERROR_CAN_T_BUILD_PASSENGER_TRAM_STATION,         STR_ERROR_CAN_T_BUILD_CARGO_TRAM_STATION },
			{ STR_ERROR_CAN_T_REMOVE_PASSENGER_TRAM_STATION,        STR_ERROR_CAN_T_REMOVE_CARGO_TRAM_STATION },
			STR_ERROR_CAN_T_CONVERT_TRAMWAY,
			{ STR_STATION_BUILD_PASSENGER_TRAM_ORIENTATION,         STR_STATION_BUILD_CARGO_TRAM_ORIENTATION },
			{ STR_STATION_BUILD_PASSENGER_TRAM_ORIENTATION_TOOLTIP, STR_STATION_BUILD_CARGO_TRAM_ORIENTATION_TOOLTIP },
		},

		/* Powered roadtypes: electric trams run on street and reserved track */
		{ROADTYPE_TRAM, ROADTYPE_TRAM_RESERVED},

		/* flags */
		{RoadTypeFlag::Catenary, RoadTypeFlag::NoHouses},

		/* cost multiplier */
		16,

		/* maintenance cost multiplier */
		24,

		/* max speed: 60 km/h in the street */
		120,

		/* road type label */
		ROADTYPE_LABEL_TRAM,

		/* alternate labels */
		{},

		/* map colour */
		PC_BLACK,

		/* introduction date */
		CalendarTime::INVALID_DATE,

		/* roadtypes required for this to be introduced */
		{},

		/* introduction road types */
		{ROADTYPE_TRAM},

		/* sort order */
		0x17,

		{ nullptr },
		{ nullptr },
		{},
	},

	/* Electrified road, for trolleybuses */
	{
		/* GUI sprites */
		{
			SPR_IMG_ROAD_X_DIR,
			SPR_IMG_ROAD_Y_DIR,
			SPR_IMG_AUTOROAD,
			SPR_IMG_ROAD_DEPOT,
			SPR_IMG_ROAD_TUNNEL,
			SPR_IMG_CONVERT_ROAD,
		},

		{
			SPR_CURSOR_ROAD_NESW,
			SPR_CURSOR_ROAD_NWSE,
			SPR_CURSOR_AUTOROAD,
			SPR_CURSOR_ROAD_DEPOT,
			SPR_CURSOR_ROAD_TUNNEL,
			SPR_CURSOR_CONVERT_ROAD,
		},

		/* strings */
		{
			STR_ROAD_NAME_TROLLEY,
			STR_ROAD_TOOLBAR_TROLLEY_CONSTRUCTION_CAPTION,
			STR_ROAD_MENU_TROLLEY_CONSTRUCTION,
			STR_BUY_VEHICLE_ROAD_VEHICLE_CAPTION,
			STR_REPLACE_ROAD_VEHICLES,
			STR_ENGINE_PREVIEW_ROAD_VEHICLE,

			STR_ERROR_CAN_T_BUILD_ROAD_HERE,
			STR_ERROR_CAN_T_REMOVE_ROAD_FROM,
			STR_ERROR_CAN_T_BUILD_ROAD_DEPOT,
			{ STR_ERROR_CAN_T_BUILD_BUS_STATION,         STR_ERROR_CAN_T_BUILD_TRUCK_STATION },
			{ STR_ERROR_CAN_T_REMOVE_BUS_STATION,        STR_ERROR_CAN_T_REMOVE_TRUCK_STATION },
			STR_ERROR_CAN_T_CONVERT_ROAD,
			{ STR_STATION_BUILD_BUS_ORIENTATION,         STR_STATION_BUILD_TRUCK_ORIENTATION },
			{ STR_STATION_BUILD_BUS_ORIENTATION_TOOLTIP, STR_STATION_BUILD_TRUCK_ORIENTATION_TOOLTIP },
		},

		/* Powered roadtypes: trolleybuses need the wires */
		{ROADTYPE_TROLLEY},

		/* flags */
		{RoadTypeFlag::Catenary},

		/* cost multiplier: road plus two wires and poles */
		20,

		/* maintenance cost multiplier */
		32,

		/* max speed */
		0,

		/* road type label */
		ROADTYPE_LABEL_TROLLEY,

		/* alternate labels */
		{},

		/* map colour */
		PC_BLACK,

		/* introduction date: the first trolleybuses */
		TimerGameCalendar::DateAtStartOfYear(TimerGameCalendar::Year{1930}),

		/* roadtypes required for this to be introduced */
		{},

		/* introduction road types */
		{ROADTYPE_TROLLEY},

		/* sort order */
		0x0B,

		{ nullptr },
		{ nullptr },
		{},
	},

	/* Horse tramway: tram track without wires */
	{
		/* GUI sprites */
		{
			SPR_IMG_TRAMWAY_X_DIR,
			SPR_IMG_TRAMWAY_Y_DIR,
			SPR_IMG_AUTOTRAM,
			SPR_IMG_ROAD_DEPOT,
			SPR_IMG_ROAD_TUNNEL,
			SPR_IMG_CONVERT_TRAM,
		},

		{
			SPR_CURSOR_TRAMWAY_NESW,
			SPR_CURSOR_TRAMWAY_NWSE,
			SPR_CURSOR_AUTOTRAM,
			SPR_CURSOR_ROAD_DEPOT,
			SPR_CURSOR_ROAD_TUNNEL,
			SPR_CURSOR_CONVERT_TRAM,
		},

		/* strings */
		{
			STR_ROAD_NAME_HORSE_TRAM,
			STR_ROAD_TOOLBAR_HORSE_TRAM_CONSTRUCTION_CAPTION,
			STR_ROAD_MENU_HORSE_TRAM_CONSTRUCTION,
			STR_BUY_VEHICLE_TRAM_VEHICLE_CAPTION,
			STR_REPLACE_TRAM_VEHICLES,
			STR_ENGINE_PREVIEW_TRAM_VEHICLE,

			STR_ERROR_CAN_T_BUILD_TRAMWAY_HERE,
			STR_ERROR_CAN_T_REMOVE_TRAMWAY_FROM,
			STR_ERROR_CAN_T_BUILD_TRAM_DEPOT,
			{ STR_ERROR_CAN_T_BUILD_PASSENGER_TRAM_STATION,         STR_ERROR_CAN_T_BUILD_CARGO_TRAM_STATION },
			{ STR_ERROR_CAN_T_REMOVE_PASSENGER_TRAM_STATION,        STR_ERROR_CAN_T_REMOVE_CARGO_TRAM_STATION },
			STR_ERROR_CAN_T_CONVERT_TRAMWAY,
			{ STR_STATION_BUILD_PASSENGER_TRAM_ORIENTATION,         STR_STATION_BUILD_CARGO_TRAM_ORIENTATION },
			{ STR_STATION_BUILD_PASSENGER_TRAM_ORIENTATION_TOOLTIP, STR_STATION_BUILD_CARGO_TRAM_ORIENTATION_TOOLTIP },
		},

		/* Powered roadtypes: horses pull trams on any tram track */
		{ROADTYPE_HORSE_TRAM, ROADTYPE_TRAM, ROADTYPE_TRAM_RESERVED},

		/* flags */
		{RoadTypeFlag::NoHouses},

		/* cost multiplier: rails without wires */
		12,

		/* maintenance cost multiplier */
		18,

		/* max speed */
		0,

		/* road type label */
		ROADTYPE_LABEL_HORSE_TRAM,

		/* alternate labels */
		{},

		/* map colour */
		PC_BLACK,

		/* introduction date */
		CalendarTime::INVALID_DATE,

		/* roadtypes required for this to be introduced */
		{},

		/* introduction road types */
		{ROADTYPE_HORSE_TRAM},

		/* sort order */
		0x13,

		{ nullptr },
		{ nullptr },
		{},
	},

	/* Reserved tramway: electric tram track on its own bed */
	{
		/* GUI sprites */
		{
			SPR_IMG_TRAMWAY_X_DIR,
			SPR_IMG_TRAMWAY_Y_DIR,
			SPR_IMG_AUTOTRAM,
			SPR_IMG_ROAD_DEPOT,
			SPR_IMG_ROAD_TUNNEL,
			SPR_IMG_CONVERT_TRAM,
		},

		{
			SPR_CURSOR_TRAMWAY_NESW,
			SPR_CURSOR_TRAMWAY_NWSE,
			SPR_CURSOR_AUTOTRAM,
			SPR_CURSOR_ROAD_DEPOT,
			SPR_CURSOR_ROAD_TUNNEL,
			SPR_CURSOR_CONVERT_TRAM,
		},

		/* strings */
		{
			STR_ROAD_NAME_TRAM_RESERVED,
			STR_ROAD_TOOLBAR_TRAM_RESERVED_CONSTRUCTION_CAPTION,
			STR_ROAD_MENU_TRAM_RESERVED_CONSTRUCTION,
			STR_BUY_VEHICLE_TRAM_VEHICLE_CAPTION,
			STR_REPLACE_TRAM_VEHICLES,
			STR_ENGINE_PREVIEW_TRAM_VEHICLE,

			STR_ERROR_CAN_T_BUILD_TRAMWAY_HERE,
			STR_ERROR_CAN_T_REMOVE_TRAMWAY_FROM,
			STR_ERROR_CAN_T_BUILD_TRAM_DEPOT,
			{ STR_ERROR_CAN_T_BUILD_PASSENGER_TRAM_STATION,         STR_ERROR_CAN_T_BUILD_CARGO_TRAM_STATION },
			{ STR_ERROR_CAN_T_REMOVE_PASSENGER_TRAM_STATION,        STR_ERROR_CAN_T_REMOVE_CARGO_TRAM_STATION },
			STR_ERROR_CAN_T_CONVERT_TRAMWAY,
			{ STR_STATION_BUILD_PASSENGER_TRAM_ORIENTATION,         STR_STATION_BUILD_CARGO_TRAM_ORIENTATION },
			{ STR_STATION_BUILD_PASSENGER_TRAM_ORIENTATION_TOOLTIP, STR_STATION_BUILD_CARGO_TRAM_ORIENTATION_TOOLTIP },
		},

		/* Powered roadtypes */
		{ROADTYPE_TRAM, ROADTYPE_TRAM_RESERVED},

		/* flags */
		{RoadTypeFlag::Catenary, RoadTypeFlag::NoHouses},

		/* cost multiplier: 1.2 times street track */
		19,

		/* maintenance cost multiplier */
		28,

		/* max speed: 80 km/h on its own bed */
		160,

		/* road type label */
		ROADTYPE_LABEL_TRAM_RESERVED,

		/* alternate labels */
		{},

		/* map colour */
		PC_BLACK,

		/* introduction date: reserved track comes to companies that run electric trams */
		TimerGameCalendar::DateAtStartOfYear(TimerGameCalendar::Year{1920}),

		/* roadtypes required for this to be introduced */
		{ROADTYPE_TRAM},

		/* introduction road types */
		{ROADTYPE_TRAM_RESERVED},

		/* sort order */
		0x1B,

		{ nullptr },
		{ nullptr },
		{},
	},
};

#endif /* ROADTYPES_H */
