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

/** What a build tool is about to build, so the 3D view can show it before it is built. */
struct View3DPreview {
	enum class Kind : uint8_t {
		None, ///< Nothing to show.
		RailDepot, ///< A train depot facing #direction.
		RoadDepot, ///< A road vehicle depot facing #direction.
		RailStation, ///< Rail station tiles along #axis.
		RoadStop, ///< Road stops; a bay facing #direction, or a drive-through stop along the axis of #direction.
	};
	Kind kind = Kind::None; ///< What is about to be built.
	DiagDirection direction = DiagDirection::NE; ///< Which way it faces.
	Axis axis = Axis::X; ///< Along which axis it lies.
	bool truck = false; ///< For road stops: a lorry stop rather than a bus stop.
	bool drive_through = false; ///< For road stops: a drive-through stop rather than a bay.
};

bool GetRailToolPreview(View3DPreview &preview);
bool GetRoadToolPreview(View3DPreview &preview);

void ShowView3DWindow(std::string_view backend = {});
void UpdateView3DMainView();
void MarkTileDirtyForView3D(TileIndex tile);
void MarkAreaDirtyForView3D(uint x_start, uint y_start, uint x_end, uint y_end);
bool GetView3DTileBelowCursor(Point &pt);
bool DrawViewportIn3D(const struct Window &w, int left, int top, int right, int bottom);

#endif /* VIEWPORT3D_VIEW3D_GUI_H */
