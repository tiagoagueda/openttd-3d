/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file power_gui.cpp The toolbar for building power lines. */

#include "stdafx.h"
#include "window_gui.h"
#include "window_func.h"
#include "viewport_func.h"
#include "tilehighlight_func.h"
#include "company_func.h"
#include "company_base.h"
#include "command_func.h"
#include "sound_func.h"
#include "hotkeys.h"
#include "gui.h"
#include "road_cmd.h"
#include "power_cmd.h"
#include "power_func.h"

#include "widgets/power_widget.h"

#include "table/strings.h"
#include "table/sprites.h"

#include "safeguards.h"

/**
 * Snap the end of a dragged line to the nearest of the eight directions from its start.
 * @param start Start tile.
 * @param end Tile the mouse was released on.
 * @return The end tile on a straight or diagonal line from the start.
 */
static TileIndex SnapPowerLineEnd(TileIndex start, TileIndex end)
{
	int dx = static_cast<int>(TileX(end)) - static_cast<int>(TileX(start));
	int dy = static_cast<int>(TileY(end)) - static_cast<int>(TileY(start));
	int ax = std::abs(dx);
	int ay = std::abs(dy);

	if (ax >= 2 * ay) {
		dy = 0;
	} else if (ay >= 2 * ax) {
		dx = 0;
	} else {
		/* Diagonal: as long as the shorter side allows, so the line stays on the map. */
		int len = std::min(ax, ay);
		dx = dx < 0 ? -len : len;
		dy = dy < 0 ? -len : len;
	}
	return TileXY(TileX(start) + dx, TileY(start) + dy);
}

/** Toolbar window for building power lines. */
struct BuildPowerToolbarWindow : Window {
	WidgetID last_user_action = INVALID_WIDGET; ///< Last started user action.

	BuildPowerToolbarWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc)
	{
		this->InitNested(window_number);
	}

	/** Is the remove toggle down? */
	bool IsRemoving() const
	{
		return this->IsWidgetLowered(WID_PT_REMOVE);
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		switch (widget) {
			case WID_PT_BUILD_LINE:
				HandlePlacePushButton(this, WID_PT_BUILD_LINE, SPR_CURSOR_AUTOELRAIL, HT_POINT | HT_DIAGONAL);
				this->last_user_action = widget;
				break;

			case WID_PT_BUILD_PYLON:
				HandlePlacePushButton(this, WID_PT_BUILD_PYLON, SPR_CURSOR_TRANSMITTER, HT_RECT);
				this->last_user_action = widget;
				break;

			case WID_PT_BUILD_SUBSTATION:
				HandlePlacePushButton(this, WID_PT_BUILD_SUBSTATION, SPR_CURSOR_RAIL_STATION, HT_RECT);
				this->last_user_action = widget;
				break;

			case WID_PT_DEMOLISH:
				HandlePlacePushButton(this, WID_PT_DEMOLISH, ANIMCURSOR_DEMOLISH, HT_RECT | HT_DIAGONAL);
				this->last_user_action = widget;
				break;

			case WID_PT_REMOVE:
				this->ToggleWidgetLoweredState(WID_PT_REMOVE);
				this->SetWidgetDirty(WID_PT_REMOVE);
				SndClickBeep();
				break;

			default: break;
		}
	}

	void OnPlaceObject([[maybe_unused]] Point pt, TileIndex tile) override
	{
		switch (this->last_user_action) {
			case WID_PT_BUILD_LINE:
				VpStartPlaceSizing(tile, VPM_X_AND_Y, this->IsRemoving() ? DDSP_REMOVE_POWER_LINE : DDSP_BUILD_POWER_LINE);
				break;

			case WID_PT_BUILD_PYLON:
				Command<Commands::BuildPowerPylon>::Post(STR_ERROR_CAN_T_BUILD_POWER_PYLON, CcPlaySound_CONSTRUCTION_OTHER, tile);
				break;

			case WID_PT_BUILD_SUBSTATION:
				Command<Commands::BuildPowerSubstation>::Post(STR_ERROR_CAN_T_BUILD_POWER_SUBSTATION, CcPlaySound_CONSTRUCTION_OTHER, tile, PowerTileKind::OverheadSubstation);
				break;

			case WID_PT_DEMOLISH:
				PlaceProc_DemolishArea(tile);
				break;

			default: NOT_REACHED();
		}
	}

	void OnPlaceDrag(ViewportPlaceMethod select_method, [[maybe_unused]] ViewportDragDropSelectionProcess select_proc, [[maybe_unused]] Point pt) override
	{
		VpSelectTilesWithMethod(pt.x, pt.y, select_method);
	}

	void OnPlaceMouseUp([[maybe_unused]] ViewportPlaceMethod select_method, ViewportDragDropSelectionProcess select_proc, Point pt, TileIndex start_tile, TileIndex end_tile) override
	{
		if (pt.x == -1) return;

		switch (select_proc) {
			case DDSP_DEMOLISH_AREA:
				GUIPlaceProcDragXY(select_proc, start_tile, end_tile);
				break;

			case DDSP_BUILD_POWER_LINE:
				Command<Commands::BuildPowerLine>::Post(STR_ERROR_CAN_T_BUILD_POWER_LINE, CcPlaySound_CONSTRUCTION_OTHER,
					SnapPowerLineEnd(start_tile, end_tile), start_tile);
				break;

			case DDSP_REMOVE_POWER_LINE:
				Command<Commands::RemovePowerLine>::Post(STR_ERROR_CAN_T_REMOVE_POWER_LINE, CcPlaySound_CONSTRUCTION_OTHER,
					SnapPowerLineEnd(start_tile, end_tile), start_tile);
				break;

			default: break;
		}
	}

	void OnPlaceObjectAbort() override
	{
		this->RaiseWidget(WID_PT_BUILD_LINE);
		this->RaiseWidget(WID_PT_BUILD_PYLON);
		this->RaiseWidget(WID_PT_BUILD_SUBSTATION);
		this->RaiseWidget(WID_PT_DEMOLISH);
		this->SetDirty();
	}

	Point OnInitialPosition(int16_t sm_width, [[maybe_unused]] int16_t sm_height, [[maybe_unused]] int window_number) override
	{
		return AlignInitialConstructionToolbar(sm_width);
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_power_toolbar_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen), SetStringTip(STR_POWER_TOOLBAR_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_STICKYBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_PT_BUILD_LINE), SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_AUTOELRAIL, STR_POWER_TOOLBAR_TOOLTIP_BUILD_LINE),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_PT_BUILD_PYLON), SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_TRANSMITTER, STR_POWER_TOOLBAR_TOOLTIP_BUILD_PYLON),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_PT_BUILD_SUBSTATION), SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_RAIL_STATION, STR_POWER_TOOLBAR_TOOLTIP_BUILD_SUBSTATION),
		NWidget(WWT_PANEL, Colours::DarkGreen), SetToolbarSpacerMinimalSize(), SetFill(1, 1), EndContainer(),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_PT_DEMOLISH), SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_DYNAMITE, STR_TOOLTIP_DEMOLISH_BUILDINGS_ETC),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_PT_REMOVE), SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_REMOVE, STR_POWER_TOOLBAR_TOOLTIP_REMOVE),
	EndContainer(),
};

static WindowDesc _power_toolbar_desc(
	WindowPosition::Manual, "toolbar_power", 0, 0,
	WindowClass::BuildToolbar, WindowClass::None,
	WindowDefaultFlag::Construction,
	_nested_power_toolbar_widgets
);

/** Window number of the power line toolbar among the construction toolbars, after the transport types. */
static const int POWER_TOOLBAR_NUMBER = 16;

/**
 * Open the toolbar for building power lines.
 * @return The toolbar, or \c nullptr if it could not be opened.
 */
Window *ShowBuildPowerToolbar()
{
	if (!Company::IsValidID(_local_company)) return nullptr;

	CloseWindowByClass(WindowClass::BuildToolbar);
	return AllocateWindowDescFront<BuildPowerToolbarWindow>(_power_toolbar_desc, POWER_TOOLBAR_NUMBER);
}

#ifdef WITH_VIEWPORT3D
#include "viewport3d/view3d_gui.h"

/**
 * Find what the power line toolbar is about to build, for the 3D view to show.
 * @param[out] preview What is about to be built.
 * @return Whether the toolbar is building something with a preview.
 */
bool GetPowerToolPreview(View3DPreview &preview)
{
	const BuildPowerToolbarWindow *w = dynamic_cast<const BuildPowerToolbarWindow *>(_thd.GetCallbackWnd());
	if (w == nullptr || w->IsRemoving()) return false;
	switch (w->last_user_action) {
		case WID_PT_BUILD_PYLON:
			preview.kind = View3DPreview::Kind::PowerPylon;
			return true;
		case WID_PT_BUILD_SUBSTATION:
			preview.kind = View3DPreview::Kind::PowerSubstation;
			return true;
		case WID_PT_BUILD_LINE: {
			if (_special_mouse_mode != SpecialMouseMode::Sizing || _thd.select_proc != DDSP_BUILD_POWER_LINE) return false;
			if (_thd.selstart.x < 0 || _thd.selstart.y < 0 || _thd.selend.x < 0 || _thd.selend.y < 0) return false;
			TileIndex start = TileVirtXY(_thd.selstart.x, _thd.selstart.y);
			TileIndex end = TileVirtXY(_thd.selend.x, _thd.selend.y);
			if (!IsValidTile(start) || !IsValidTile(end)) return false;
			preview.kind = View3DPreview::Kind::PowerLine;
			preview.start = start;
			preview.end = SnapPowerLineEnd(start, end);
			return true;
		}
		default:
			return false;
	}
}
#endif /* WITH_VIEWPORT3D */
