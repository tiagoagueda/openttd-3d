/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file power_widget.h Types related to the power line widgets. */

#ifndef WIDGETS_POWER_WIDGET_H
#define WIDGETS_POWER_WIDGET_H

/** Widgets of the #BuildPowerToolbarWindow class. */
enum BuildPowerToolbarWidgets : WidgetID {
	WID_PT_BUILD_LINE, ///< Build an overhead line.
	WID_PT_BUILD_PYLON, ///< Build a single pylon.
	WID_PT_BUILD_SUBSTATION, ///< Build an overhead substation.
	WID_PT_BUILD_TRANSITION, ///< Build a transition pylon, where an overhead line goes underground.
	WID_PT_BUILD_CABLE, ///< Build an underground cable.
	WID_PT_BUILD_UNDERGROUND_SUBSTATION, ///< Build an underground substation.
	WID_PT_NETWORK_VIEW, ///< Toggle the power network view.
	WID_PT_DEMOLISH, ///< Demolish aka dynamite button.
	WID_PT_REMOVE, ///< Toggle removal of lines.
};

/** Widgets of the #PowerTileWindow class. */
enum PowerTileWidgets : WidgetID {
	WID_PTV_CAPTION, ///< Caption of the window.
	WID_PTV_LOCATION, ///< Button to centre the main view on the tile.
	WID_PTV_INFO, ///< Panel with the figures.
};

#endif /* WIDGETS_POWER_WIDGET_H */
