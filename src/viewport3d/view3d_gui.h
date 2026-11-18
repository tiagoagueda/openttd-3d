/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file view3d_gui.h Window showing the map in 3D. */

#ifndef VIEWPORT3D_VIEW3D_GUI_H
#define VIEWPORT3D_VIEW3D_GUI_H

#include "../core/geometry_type.hpp"
#include "../tile_type.h"
#include "../direction_type.h"
#include "../rail_type.h"
#include "../road_type.h"

/** What a build tool is about to build, so the 3D view can show it before it is built. */
struct View3DPreview {
	enum class Kind : uint8_t {
		None, ///< Nothing to show.
		RailDepot, ///< A train depot facing #direction.
		RoadDepot, ///< A road vehicle depot facing #direction.
		RailStation, ///< Rail station tiles along #axis.
		RoadStop, ///< Road stops; a bay facing #direction, or a drive-through stop along the axis of #direction.
		PowerPylon, ///< A power line pylon.
		PowerSubstation, ///< An overhead power substation.
		PowerLine, ///< An overhead power line from #start to #end.
		Track, ///< Pieces of track of type #rail, where the 2D views show the autorail selection.
		Road, ///< Road or tram pieces along #axis; #road_bits on a single tile.
	};
	Kind kind = Kind::None; ///< What is about to be built.
	DiagDirection direction = DiagDirection::NE; ///< Which way it faces.
	Axis axis = Axis::X; ///< Along which axis it lies.
	bool truck = false; ///< For road stops: a lorry stop rather than a bus stop.
	bool drive_through = false; ///< For road stops: a drive-through stop rather than a bay.
	TileIndex start = INVALID_TILE; ///< For power lines: where the line starts.
	TileIndex end = INVALID_TILE; ///< For power lines: where the line ends.
	bool remove = false; ///< For track and road: the tool removes rather than builds.
	bool tram = false; ///< For road: tram track rather than road.
	RailType rail = INVALID_RAILTYPE; ///< For track: the track type.
	RoadBits road_bits{}; ///< For road: the pieces on a single selected tile; longer selections get whole pieces along #axis.
};

/** A picture of something that can be built, for the buttons of construction windows. */
struct View3DPicture {
	enum class Kind : uint8_t {
		RailStation, ///< A rail station tile along #axis.
		RailWaypoint, ///< A rail waypoint along #axis.
		RailDepot, ///< A train depot facing #direction.
		RoadStop, ///< A road stop; a bay facing #direction, or a drive-through stop along #axis.
		RoadWaypoint, ///< A road waypoint along #axis.
		RoadDepot, ///< A road vehicle depot facing #direction.
		ShipDepot, ///< A ship depot along #axis.
		Object, ///< An object in view #view.
		House, ///< A town building.
	};
	Kind kind = Kind::RailStation; ///< What it is.
	uint32_t grfid = 0; ///< For things a NewGRF may define: the NewGRF, or 0 for the base game.
	uint16_t id = 0; ///< For things a NewGRF may define: its number in the NewGRF, or in the base game.
	DiagDirection direction = DiagDirection::SW; ///< Which way it faces.
	Axis axis = Axis::X; ///< Along which axis it lies.
	uint8_t view = 0; ///< For objects: which of its views.
	uint8_t size_x = 1; ///< Size along x in tiles.
	uint8_t size_y = 1; ///< Size along y in tiles.
	bool truck = false; ///< For road stops: a lorry stop rather than a bus stop.
	bool drive_through = false; ///< For road stops: a drive-through stop rather than a bay.
	RailType rail = INVALID_RAILTYPE; ///< For rail stations and waypoints: the track type.

	void SetSource(const struct GRFFilePropsBase &props, uint16_t base_game_id);
	bool operator==(const View3DPicture &other) const = default;
};

bool DrawView3DPicture(const View3DPicture &picture, int width, int height);

bool GetRailToolPreview(View3DPreview &preview);
bool GetRoadToolPreview(View3DPreview &preview);
bool GetPowerToolPreview(View3DPreview &preview);

void ShowView3DWindow(std::string_view backend = {});
void UpdateView3DMainView();
void MarkTileDirtyForView3D(TileIndex tile);
void MarkAreaDirtyForView3D(uint x_start, uint y_start, uint x_end, uint y_end);
bool GetView3DTileBelowCursor(Point &pt);
bool DrawViewportIn3D(const struct Window &w, int left, int top, int right, int bottom);
bool AreViewportsDrawnIn3D();

#endif /* VIEWPORT3D_VIEW3D_GUI_H */
