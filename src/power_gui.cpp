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
#include "power_map.h"
#include "zoom_func.h"
#include "strings_func.h"
#include "timer/timer_window.h"
#include "timer/timer.h"
#include "transparency.h"
#include "slope_func.h"
#include "landscape.h"

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

/**
 * Mouse cursor of a power tool.
 * @param cursor The cursor from power.grf.
 * @param stand_in The cursor of the base set to use without power.grf.
 * @return The cursor to use.
 */
static CursorID PowerCursor(CursorID cursor, CursorID stand_in)
{
	return _power_sprites_loaded ? cursor : stand_in;
}

/** Toolbar window for building power lines. */
struct BuildPowerToolbarWindow : Window {
	WidgetID last_user_action = INVALID_WIDGET; ///< Last started user action.

	BuildPowerToolbarWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc)
	{
		this->CreateNestedTree();
		if (!_power_sprites_loaded) {
			/* Without power.grf, stand-ins from the base set. */
			static const std::pair<WidgetID, SpriteID> STAND_INS[] = {
				{WID_PT_BUILD_LINE, SPR_IMG_AUTOELRAIL}, {WID_PT_BUILD_PYLON, SPR_IMG_TRANSMITTER},
				{WID_PT_BUILD_SUBSTATION, SPR_IMG_RAIL_STATION}, {WID_PT_BUILD_TRANSITION, SPR_IMG_CONVERT_ELRAIL},
				{WID_PT_BUILD_CABLE, SPR_IMG_TUNNEL_RAIL}, {WID_PT_BUILD_UNDERGROUND_SUBSTATION, SPR_IMG_TRUCK_BAY},
				{WID_PT_NETWORK_VIEW, SPR_IMG_SHOW_ROUTES},
			};
			for (auto [widget, sprite] : STAND_INS) this->GetWidget<NWidgetCore>(widget)->SetSprite(sprite);
		}
		this->FinishInitNested(window_number);
		this->SetWidgetLoweredState(WID_PT_NETWORK_VIEW, _power_network_view);
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
				HandlePlacePushButton(this, WID_PT_BUILD_LINE, PowerCursor(SPR_CURSOR_POWER_LINE, SPR_CURSOR_AUTOELRAIL), HT_RECT);
				this->last_user_action = widget;
				break;

			case WID_PT_BUILD_PYLON:
				HandlePlacePushButton(this, WID_PT_BUILD_PYLON, PowerCursor(SPR_CURSOR_POWER_PYLON, SPR_CURSOR_TRANSMITTER), HT_RECT);
				this->last_user_action = widget;
				break;

			case WID_PT_BUILD_SUBSTATION:
			case WID_PT_BUILD_UNDERGROUND_SUBSTATION:
				if (HandlePlacePushButton(this, widget, widget == WID_PT_BUILD_SUBSTATION ? PowerCursor(SPR_CURSOR_POWER_SUBSTATION, SPR_CURSOR_RAIL_STATION) : PowerCursor(SPR_CURSOR_POWER_UNDERGROUND_SUBSTATION, SPR_CURSOR_TRUCK_STATION), HT_RECT)) {
					/* Show the reach of the substation around the cursor. */
					int rad = _settings_game.economy.power_catchment;
					SetTileSelectBigSize(-rad, -rad, 2 * rad, 2 * rad);
				}
				this->last_user_action = widget;
				break;

			case WID_PT_BUILD_TRANSITION:
				HandlePlacePushButton(this, WID_PT_BUILD_TRANSITION, PowerCursor(SPR_CURSOR_POWER_TRANSITION, SPR_CURSOR_TRANSMITTER), HT_RECT);
				this->last_user_action = widget;
				break;

			case WID_PT_BUILD_CABLE:
				if (HandlePlacePushButton(this, WID_PT_BUILD_CABLE, PowerCursor(SPR_CURSOR_POWER_CABLE, SPR_CURSOR_TUNNEL_RAIL), HT_RECT)) {
					/* Cables are hidden; show them while building them. */
					SetPowerNetworkView(true);
					this->SetWidgetLoweredState(WID_PT_NETWORK_VIEW, true);
					this->SetWidgetDirty(WID_PT_NETWORK_VIEW);
				}
				this->last_user_action = widget;
				break;

			case WID_PT_NETWORK_VIEW:
				SetPowerNetworkView(!_power_network_view);
				this->SetWidgetLoweredState(WID_PT_NETWORK_VIEW, _power_network_view);
				this->SetWidgetDirty(WID_PT_NETWORK_VIEW);
				SndClickBeep();
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
				Command<Commands::BuildPowerPylon>::Post(STR_ERROR_CAN_T_BUILD_POWER_PYLON, CcPlaySound_CONSTRUCTION_OTHER, tile, PowerTileKind::Pylon);
				break;

			case WID_PT_BUILD_SUBSTATION:
				Command<Commands::BuildPowerSubstation>::Post(STR_ERROR_CAN_T_BUILD_POWER_SUBSTATION, CcPlaySound_CONSTRUCTION_OTHER, tile, PowerTileKind::OverheadSubstation);
				break;

			case WID_PT_BUILD_UNDERGROUND_SUBSTATION:
				Command<Commands::BuildPowerSubstation>::Post(STR_ERROR_CAN_T_BUILD_POWER_SUBSTATION, CcPlaySound_CONSTRUCTION_OTHER, tile, PowerTileKind::UndergroundSubstation);
				break;

			case WID_PT_BUILD_TRANSITION:
				Command<Commands::BuildPowerPylon>::Post(STR_ERROR_CAN_T_BUILD_POWER_PYLON, CcPlaySound_CONSTRUCTION_OTHER, tile, PowerTileKind::TransitionPylon);
				break;

			case WID_PT_BUILD_CABLE:
				VpStartPlaceSizing(tile, VPM_X_AND_Y, this->IsRemoving() ? DDSP_REMOVE_POWER_CABLE : DDSP_BUILD_POWER_CABLE);
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

			case DDSP_BUILD_POWER_CABLE:
				Command<Commands::BuildPowerCable>::Post(STR_ERROR_CAN_T_BUILD_POWER_CABLE, CcPlaySound_CONSTRUCTION_OTHER,
					SnapPowerLineEnd(start_tile, end_tile), start_tile);
				break;

			case DDSP_REMOVE_POWER_CABLE:
				Command<Commands::RemovePowerCable>::Post(STR_ERROR_CAN_T_REMOVE_POWER_CABLE, CcPlaySound_CONSTRUCTION_OTHER,
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
		this->RaiseWidget(WID_PT_BUILD_TRANSITION);
		this->RaiseWidget(WID_PT_BUILD_CABLE);
		this->RaiseWidget(WID_PT_BUILD_UNDERGROUND_SUBSTATION);
		this->RaiseWidget(WID_PT_DEMOLISH);
		this->SetDirty();
	}

	Point OnInitialPosition(int16_t sm_width, [[maybe_unused]] int16_t sm_height, [[maybe_unused]] int window_number) override
	{
		return AlignInitialConstructionToolbar(sm_width);
	}
};

/** The plan of the overhead line being dragged, kept while the drag does not change. */
struct PowerLineDragPlan {
	TileIndex start = INVALID_TILE; ///< Start of the planned line.
	TileIndex end = INVALID_TILE; ///< End of the planned line.
	std::vector<TileIndex> pylons; ///< Tiles that carry the ends of spans.
	bool buildable = false; ///< Whether the whole line can be built.
};
static PowerLineDragPlan _power_line_drag_plan;

/**
 * Get the plan of an overhead line, worked out again only when the line changes.
 * @param start Start of the line.
 * @param end End of the line.
 * @return The plan.
 */
static const PowerLineDragPlan &GetPowerLineDragPlan(TileIndex start, TileIndex end)
{
	PowerLineDragPlan &plan = _power_line_drag_plan;
	if (plan.start != start || plan.end != end) {
		plan.start = start;
		plan.end = end;
		StringID error;
		plan.buildable = GetPowerLinePreviewPlan(start, end, plan.pylons, error);
	}
	return plan;
}

/**
 * Get where the line tool puts its pylons on an overhead line, for the 3D preview.
 * @param start Start of the line.
 * @param end End of the line.
 * @param[out] pylons The tiles that carry the ends of spans, as far as the line can be built.
 * @return Whether the whole line can be built.
 */
bool GetPowerLinePlanPylons(TileIndex start, TileIndex end, std::vector<TileIndex> &pylons)
{
	const PowerLineDragPlan &plan = GetPowerLineDragPlan(start, end);
	pylons = plan.pylons;
	return plan.buildable;
}

/**
 * Find the line or cable being dragged with the power toolbar.
 * @param[out] start Start of the drag.
 * @param[out] end End of the drag, snapped to one of the eight directions.
 * @param[out] proc What the drag does.
 * @return Whether a line or cable is being dragged.
 */
static bool GetPowerLineDrag(TileIndex &start, TileIndex &end, ViewportDragDropSelectionProcess &proc)
{
	if (_special_mouse_mode != SpecialMouseMode::Sizing) return false;
	if (dynamic_cast<const BuildPowerToolbarWindow *>(_thd.GetCallbackWnd()) == nullptr) return false;
	proc = _thd.select_proc;
	if (proc != DDSP_BUILD_POWER_LINE && proc != DDSP_REMOVE_POWER_LINE && proc != DDSP_BUILD_POWER_CABLE && proc != DDSP_REMOVE_POWER_CABLE) return false;
	if (_thd.selstart.x < 0 || _thd.selstart.y < 0 || _thd.selend.x < 0 || _thd.selend.y < 0) return false;
	start = TileVirtXY(_thd.selstart.x, _thd.selstart.y);
	end = TileVirtXY(_thd.selend.x, _thd.selend.y);
	if (!IsValidTile(start) || !IsValidTile(end)) return false;
	end = SnapPowerLineEnd(start, end);
	return true;
}

/**
 * Whether a line or cable is being dragged with the power toolbar.
 * @return True if so.
 */
bool IsPowerLineDragActive()
{
	TileIndex start, end;
	ViewportDragDropSelectionProcess proc;
	return GetPowerLineDrag(start, end, proc);
}

/**
 * Where a tile lies on a straight or diagonal line.
 * @param start Start of the line.
 * @param end End of the line, on one of the eight directions from the start.
 * @param tile The tile.
 * @return The number of steps from the start, or -1 when the tile is not on the line.
 */
static int GetStepOnLine(TileIndex start, TileIndex end, TileIndex tile)
{
	int dx = static_cast<int>(TileX(end)) - static_cast<int>(TileX(start));
	int dy = static_cast<int>(TileY(end)) - static_cast<int>(TileY(start));
	int tx = static_cast<int>(TileX(tile)) - static_cast<int>(TileX(start));
	int ty = static_cast<int>(TileY(tile)) - static_cast<int>(TileY(start));
	int sx = Clamp(dx, -1, 1);
	int sy = Clamp(dy, -1, 1);
	int len = std::max(std::abs(dx), std::abs(dy));
	int k = sx != 0 ? tx * sx : ty * sy;
	if (k < 0 || k > len || tx != sx * k || ty != sy * k) return -1;
	return k;
}

/**
 * Draw the preview of the line or cable being dragged with the power toolbar on a tile:
 * the tiles of the line, with the pylons where the line tool will put them.
 * Tiles the line does not reach turn red where the line cannot be built.
 * @param ti The tile.
 * @param[out] highlight The palette of the selection square to draw on the tile, if any.
 * @return Whether a power line or cable is being dragged; the usual tile selection is then not drawn.
 */
bool GetPowerLineDragHighlight(const TileInfo *ti, std::optional<PaletteID> &highlight)
{
	highlight.reset();
	TileIndex start, end;
	ViewportDragDropSelectionProcess proc;
	if (!GetPowerLineDrag(start, end, proc)) return false;

	int step = GetStepOnLine(start, end, ti->tile);
	if (step < 0) return true;

	switch (proc) {
		case DDSP_REMOVE_POWER_LINE:
		case DDSP_REMOVE_POWER_CABLE:
			highlight = PALETTE_SEL_TILE_RED;
			return true;
		case DDSP_BUILD_POWER_CABLE:
			highlight = PAL_NONE;
			return true;
		default: break;
	}

	const PowerLineDragPlan &plan = GetPowerLineDragPlan(start, end);
	bool pylon = std::ranges::find(plan.pylons, ti->tile) != plan.pylons.end();
	/* Without a plan to here, the line stops before this tile. */
	int reached = 0;
	for (TileIndex t : plan.pylons) reached = std::max(reached, GetStepOnLine(start, end, t));
	if (!plan.buildable && step > reached) {
		highlight = PALETTE_SEL_TILE_RED;
		return true;
	}
	highlight = pylon ? PAL_NONE : PALETTE_SEL_TILE_BLUE;

	if (pylon && _power_sprites_loaded && !IsPowerTile(ti->tile) && !IsInvisibilitySet(TransparencyOption::Structures)) {
		static const SpriteBounds PYLON_BOUNDS{{6, 6, 0}, {4, 4, 40}, {-6, -6, 0}};
		int dx = std::abs(static_cast<int>(TileX(end)) - static_cast<int>(TileX(start)));
		int dy = std::abs(static_cast<int>(TileY(end)) - static_cast<int>(TileY(start)));
		uint dir;
		if (dy == 0) {
			dir = 0;
		} else if (dx == 0) {
			dir = 1;
		} else {
			dir = ((TileX(end) > TileX(start)) == (TileY(end) > TileY(start))) ? 2 : 3;
		}
		AddSortableSpriteToDraw(SPR_POWER_PYLON + dir, PAL_NONE, ti->x, ti->y, ti->z + GetSlopeMaxPixelZ(ti->tileh), PYLON_BOUNDS, false);
	}
	return true;
}

static constexpr std::initializer_list<NWidgetPart> _nested_power_toolbar_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen), SetStringTip(STR_POWER_TOOLBAR_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_STICKYBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_PT_BUILD_LINE), SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_POWER_LINE, STR_POWER_TOOLBAR_TOOLTIP_BUILD_LINE),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_PT_BUILD_PYLON), SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_POWER_PYLON, STR_POWER_TOOLBAR_TOOLTIP_BUILD_PYLON),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_PT_BUILD_SUBSTATION), SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_POWER_SUBSTATION, STR_POWER_TOOLBAR_TOOLTIP_BUILD_SUBSTATION),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_PT_BUILD_TRANSITION), SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_POWER_TRANSITION, STR_POWER_TOOLBAR_TOOLTIP_BUILD_TRANSITION),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_PT_BUILD_CABLE), SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_POWER_CABLE, STR_POWER_TOOLBAR_TOOLTIP_BUILD_CABLE),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_PT_BUILD_UNDERGROUND_SUBSTATION), SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_POWER_UNDERGROUND_SUBSTATION, STR_POWER_TOOLBAR_TOOLTIP_BUILD_UNDERGROUND_SUBSTATION),
		NWidget(WWT_PANEL, Colours::DarkGreen), SetToolbarSpacerMinimalSize(), SetFill(1, 1), EndContainer(),
		NWidget(WWT_IMGBTN, Colours::DarkGreen, WID_PT_NETWORK_VIEW), SetFill(0, 1), SetToolbarMinimalSize(1), SetSpriteTip(SPR_IMG_POWER_NETWORK_VIEW, STR_POWER_TOOLBAR_TOOLTIP_NETWORK_VIEW),
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

/** Window with what a substation, or a pylon joining a power plant to the grid, does. */
struct PowerTileWindow : Window {
	static constexpr uint LINES = 8; ///< Most lines of text.

	PowerTileWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc)
	{
		this->InitNested(window_number);
		this->owner = GetTileOwner(this->GetTile());
	}

	/** The tile the window is about. */
	TileIndex GetTile() const
	{
		return TileIndex{static_cast<uint32_t>(this->window_number)};
	}

	std::string GetWidgetString(WidgetID widget, StringID stringid) const override
	{
		if (widget != WID_PTV_CAPTION) return this->Window::GetWidgetString(widget, stringid);
		TileIndex tile = this->GetTile();
		if (IsPowerTile(tile) && IsPowerSubstation(tile)) {
			return GetString(GetPowerTileKind(tile) == PowerTileKind::UndergroundSubstation ? STR_POWER_TILE_CAPTION_UNDERGROUND_SUBSTATION : STR_POWER_TILE_CAPTION_SUBSTATION);
		}
		return GetString(STR_POWER_TILE_CAPTION_PLANT_LINK);
	}

	void UpdateWidgetSize(WidgetID widget, Dimension &size, const Dimension &padding, [[maybe_unused]] Dimension &fill, [[maybe_unused]] Dimension &resize) override
	{
		if (widget != WID_PTV_INFO) return;
		size.width = std::max<uint>(size.width, ScaleGUITrad(380)) + padding.width;
		size.height = (LINES + 1) * GetCharacterHeight(FontSize::Normal) + padding.height + WidgetDimensions::scaled.framerect.Vertical();
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		if (widget != WID_PTV_INFO) return;
		TileIndex tile = this->GetTile();
		if (!IsPowerTile(tile)) return;
		Rect ir = r.Shrink(WidgetDimensions::scaled.frametext, WidgetDimensions::scaled.framerect);
		auto line = [&](const std::string &text) {
			ir.top = DrawStringMultiLine(ir, text);
		};
		line(GetString(STR_POWER_TILE_OWNER, GetTileOwner(tile)));
		PowerTileFigures f = GetPowerTileFigures(tile);
		switch (f.status) {
			case PowerTileStatus::Flowing: line(GetString(STR_POWER_TILE_STATUS_FLOWING)); break;
			case PowerTileStatus::Idle: line(GetString(STR_POWER_TILE_STATUS_IDLE)); break;
			case PowerTileStatus::Unconnected: line(GetString(STR_POWER_TILE_STATUS_UNCONNECTED)); break;
			default: line(GetString(STR_POWER_TILE_STATUS_UNKNOWN)); return;
		}
		if (f.plant_link) line(GetString(STR_POWER_TILE_PLANT_LINK_INFO));
		line(GetString(STR_POWER_TILE_COVERS, f.plants, f.consumers));
		line(GetString(STR_POWER_TILE_SENT, f.sent, 1, f.supply, 1));
		if (!f.plant_link) line(GetString(STR_POWER_TILE_RECEIVED, f.received, 1, f.demand, 1));
		line(GetString(STR_POWER_TILE_CAPACITY, f.capacity, 1));
		PowerNetworkFigures n = GetPowerNetworkFigures(tile);
		line(GetString(STR_POWER_TILE_NETWORK, n.substations));
		line(GetString(STR_POWER_TILE_NETWORK_FLOW, n.supply, 1, n.demand, 1, n.received, 1));
	}

	void OnClick([[maybe_unused]] Point pt, WidgetID widget, [[maybe_unused]] int click_count) override
	{
		if (widget != WID_PTV_LOCATION) return;
		if (IsCtrlPressed()) {
			ShowExtraViewportWindow(this->GetTile());
		} else {
			ScrollMainWindowToTile(this->GetTile());
		}
	}

	/** The figures change at every balancing step. */
	const IntervalTimer<TimerWindow> refresh_interval = {std::chrono::seconds(1), [this](auto) {
		if (!IsPowerTile(this->GetTile())) {
			this->Close();
			return;
		}
		this->SetWidgetDirty(WID_PTV_INFO);
	}};
};

static constexpr std::initializer_list<NWidgetPart> _nested_power_tile_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::DarkGreen),
		NWidget(WWT_CAPTION, Colours::DarkGreen, WID_PTV_CAPTION),
		NWidget(WWT_PUSHIMGBTN, Colours::DarkGreen, WID_PTV_LOCATION), SetAspect(WidgetDimensions::ASPECT_LOCATION), SetSpriteTip(SPR_GOTO_LOCATION, STR_POWER_TILE_LOCATION_TOOLTIP),
		NWidget(WWT_SHADEBOX, Colours::DarkGreen),
		NWidget(WWT_STICKYBOX, Colours::DarkGreen),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::DarkGreen, WID_PTV_INFO), EndContainer(),
};

static WindowDesc _power_tile_desc(
	WindowPosition::Automatic, "view_power_tile", 0, 0,
	WindowClass::PowerTileView, WindowClass::None,
	{},
	_nested_power_tile_widgets
);

/**
 * Open the window of a substation, or of a pylon joining a power plant to the grid.
 * @param tile The power tile.
 */
void ShowPowerTileWindow(TileIndex tile)
{
	AllocateWindowDescFront<PowerTileWindow>(_power_tile_desc, tile.base());
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
