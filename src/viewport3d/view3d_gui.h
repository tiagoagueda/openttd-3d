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

void ShowView3DWindow(std::string_view backend = {});
void MarkTileDirtyForView3D(TileIndex tile);
void MarkAreaDirtyForView3D(uint x_start, uint y_start, uint x_end, uint y_end);
bool GetView3DTileBelowCursor(Point &pt);

#endif /* VIEWPORT3D_VIEW3D_GUI_H */
