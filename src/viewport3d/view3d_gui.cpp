/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file view3d_gui.cpp Window showing the map in 3D. */

#include "../stdafx.h"
#include "view3d_gui.h"
#include "camera3d.h"
#include "renderer3d.h"
#include "vehicle_sprites.h"
#include "terrain_mesh.h"
#include "tile_sprites.h"
#include "../blitter/factory.hpp"
#include "../gfx_func.h"
#include "../landscape.h"
#include "../map_func.h"
#include "../palette_func.h"
#include "../settings_type.h"
#include "../strings_func.h"
#include "../tile_map.h"
#include "../vehicle_base.h"
#include "../core/backup_type.hpp"
#include "../viewport_type.h"
#include "../tilehighlight_func.h"
#include "../window_func.h"
#include "../window_gui.h"

#include "table/strings.h"

#include <map>

#include "../safeguards.h"

/** Widgets of the #View3DWindow class. */
enum View3DWidgets : WidgetID {
	WID_V3D_CAPTION, ///< Caption of the window.
	WID_V3D_VIEW, ///< The 3D image.
	WID_V3D_STATUS, ///< Status bar below the image.
};

class View3DWindow;
static std::string _view3d_backend; ///< Graphics API the user asked for; empty to pick automatically.
static View3DWindow *_view3d_window = nullptr; ///< The open 3D view, if any, so tile changes can find it quickly.

/** Window showing the map in 3D. */
class View3DWindow : public Window {
	/** What the window knows about one chunk of terrain. */
	struct ChunkState {
		bool built = false; ///< Whether the chunk is on the GPU.
		bool dirty = false; ///< Whether a tile in the chunk changed since it was built.
		bool slow = false; ///< Whether grass, fields or trees in the chunk grew since it was built.
		std::chrono::steady_clock::time_point slow_since; ///< When the first of those slow changes happened.
		uint64_t hash = 0; ///< Hash of the geometry on the GPU.
		uint min_height = 0; ///< Lowest height level in the chunk; only valid when built.
		uint max_height = 0; ///< Highest height level in the chunk; only valid when built.
		std::vector<AnimatedPart> parts; ///< Parts of models in the chunk that move.
		std::map<TileIndex, std::vector<ModelInstance>> live; ///< Models in the chunk that change often, by tile.
	};
	std::map<TileIndex, uint64_t> tile_keys; ///< Rail tiles and level crossings in built chunks, by GetTileChangeKey().
	std::vector<uint16_t> built_tiles; ///< Type and height of every tile when its chunk was last built, by tile index.
	static constexpr std::chrono::seconds SLOW_CHANGE_DELAY{3}; ///< How long slow changes wait before their chunk is rebuilt.

	/**
	 * Pack what tells a slow change of a tile from a fast one.
	 * @param tile The tile.
	 * @return Its type and height.
	 */
	static uint16_t GetBuiltTileState(TileIndex tile)
	{
		return static_cast<uint16_t>(to_underlying(GetTileType(tile)) << 8 | TileHeight(tile));
	}

	/**
	 * Whether a tile with this state only changes slowly and on its own: grass, fields, rough land, snow and trees.
	 * @param state The state from GetBuiltTileState().
	 * @return True for bare land and trees.
	 */
	static bool IsSlowTileState(uint16_t state)
	{
		TileType type = static_cast<TileType>(state >> 8);
		return type == TileType::Clear || type == TileType::Trees;
	}

	/** What a tile with a 3D model was last drawn with. */
	struct TileModels {
		std::vector<ModelInstance> models; ///< The models.
		std::vector<AnimatedPart> parts; ///< Their moving parts.
	};
	std::map<TileIndex, TileModels> tile_models; ///< Tiles with a model that changed since the 3D view opened.
	std::chrono::steady_clock::time_point opened = std::chrono::steady_clock::now(); ///< When the view opened; moving parts move by the time since.

	std::unique_ptr<Renderer3D> renderer; ///< The GPU renderer; created on the first frame.
	bool renderer_tried = false; ///< Whether creating the renderer has been attempted.
	std::string error; ///< Why the renderer could not be created.

	Camera3D camera; ///< Where we look from.
	/* The camera glides towards these, so zooming and turning are smooth. */
	float goal_distance = Camera3D{}.distance; ///< Distance the camera zooms to.
	float goal_yaw = Camera3D{}.yaw; ///< Direction the camera turns to.
	float goal_pitch = Camera3D{}.pitch; ///< Angle the camera tilts to.
	bool zoom_anchored = false; ///< Whether zooming keeps #zoom_anchor in place on the screen.
	Vec3 zoom_anchor{}; ///< Spot on the ground below the cursor when zooming started.
	bool view_changed = true; ///< Whether the camera or image size changed since the last render.
	bool terrain_changed = true; ///< Whether chunks need to be (re)built.

	uint chunks_x = 0; ///< Number of chunks along the x axis.
	uint chunks_y = 0; ///< Number of chunks along the y axis.
	std::vector<ChunkState> chunks; ///< State of all chunks, row by row.
	std::vector<Renderer3D::ChunkID> visible_chunks; ///< Chunks to draw this frame.
	TerrainChunkMesh scratch_mesh; ///< Reused while building chunks.
	std::unique_ptr<GroundTextures> ground_textures; ///< The ground textures that the chunks use.
	std::unique_ptr<SpriteAtlas> atlas; ///< The sprites of the 2D game that the chunks use.
	std::unique_ptr<ModelPack> model_pack; ///< The package of 3D models, if there is one.
	std::vector<ModelInstance> vehicle_models; ///< The vehicles in view that are drawn with a 3D model.
	std::vector<SpriteVertex> vehicle_sprites; ///< The vehicles in view.
	std::vector<VehicleSpritePlace> vehicle_places; ///< Where the vehicles in view are, to find the one below the cursor.
	uint64_t vehicle_hash = 0; ///< Hash of the vehicles in the last image, to see whether any moved.

	std::chrono::steady_clock::time_point textures_reset{}; ///< When the ground textures were last started afresh.

public:
	View3DWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc)
	{
		this->InitNested(window_number);

		this->chunks_x = CeilDiv(Map::MaxX(), TERRAIN_CHUNK_SIZE);
		this->chunks_y = CeilDiv(Map::MaxY(), TERRAIN_CHUNK_SIZE);
		this->chunks.resize(static_cast<size_t>(this->chunks_x) * this->chunks_y);

		_view3d_window = this;
	}

	~View3DWindow() override
	{
		if (_view3d_window == this) _view3d_window = nullptr;
	}

	void Close([[maybe_unused]] int data = 0) override
	{
		if (_view3d_window == this) _view3d_window = nullptr;
		this->Window::Close();
	}

	/**
	 * A tile changed; remember to rebuild the terrain around it.
	 * @param tile The changed tile.
	 */
	void MarkTileDirty(TileIndex tile)
	{
		/* Trains change rail tiles and level crossings all the time, mostly in ways that do not change the terrain
		 * chunk: then only the signals and barriers of the tile are looked up again. */
		uint64_t key;
		if (this->model_pack != nullptr && !this->model_pack->IsEmpty() && IsValidTile(tile) && GetTileChangeKey(tile, key)) {
			auto [it, inserted] = this->tile_keys.try_emplace(tile, key);
			uint cx = TileX(tile) / TERRAIN_CHUNK_SIZE, cy = TileY(tile) / TERRAIN_CHUNK_SIZE;
			ChunkState *chunk = (cx < this->chunks_x && cy < this->chunks_y) ? &this->chunks[cy * this->chunks_x + cx] : nullptr;
			if (!inserted && it->second == key && chunk != nullptr && chunk->built && !chunk->dirty) {
				if (HasLiveTileModels(tile)) {
					GetLiveTileModels(tile, *this->model_pack, chunk->live[tile]);
				}
				return;
			}
			it->second = key;
		}

		/* Grass, fields and trees grow all over the map all the time. They do not have to show at once: their chunk is
		 * rebuilt a little later, together with the other growth in it. */
		if (IsValidTile(tile) && tile.base() < this->built_tiles.size()) {
			uint16_t before = this->built_tiles[tile.base()];
			uint16_t now = GetBuiltTileState(tile);
			/* Trees also spread onto bare land and die back to it, on the same height. */
			if ((before & 0xFF) == (now & 0xFF) && IsSlowTileState(before) && IsSlowTileState(now)) {
				uint cx = TileX(tile) / TERRAIN_CHUNK_SIZE, cy = TileY(tile) / TERRAIN_CHUNK_SIZE;
				if (cx < this->chunks_x && cy < this->chunks_y) {
					ChunkState &chunk = this->chunks[cy * this->chunks_x + cx];
					if (chunk.built && !chunk.slow) {
						chunk.slow = true;
						chunk.slow_since = std::chrono::steady_clock::now();
					}
					return;
				}
			}
		}

		/* Animated buildings tell about every frame of their animation. When they are drawn with a model that stays
		 * the same, nothing needs to be rebuilt; the moving parts of models move by themselves. */
		if (this->model_pack != nullptr && !this->model_pack->IsEmpty() && IsValidTile(tile)) {
			static TileModels now;
			if (GetTileModels(tile, *this->model_pack, now.models, now.parts)) {
				auto [it, inserted] = this->tile_models.try_emplace(tile, now);
				if (!inserted) {
					if (it->second.models == now.models && it->second.parts == now.parts) return;
					it->second = now;
				}
			} else {
				this->tile_models.erase(tile);
			}
		}

		uint x = TileX(tile);
		uint y = TileY(tile);
		/* The height of a tile is the height of its northern corner, which is shared with three neighbours. */
		for (uint ty : {y > 0 ? y - 1 : y, y}) {
			for (uint tx : {x > 0 ? x - 1 : x, x}) {
				uint cx = tx / TERRAIN_CHUNK_SIZE;
				uint cy = ty / TERRAIN_CHUNK_SIZE;
				if (cx >= this->chunks_x || cy >= this->chunks_y) continue;
				this->chunks[cy * this->chunks_x + cx].dirty = true;
			}
		}
		this->terrain_changed = true;
	}

	/**
	 * Remember the state of the rail tiles and level crossings of a chunk that was just built, to tell later which
	 * changes to them need the chunk to be built again.
	 * @param cx X coordinate of the chunk, in chunks.
	 * @param cy Y coordinate of the chunk, in chunks.
	 */
	void RememberTileKeys(uint cx, uint cy)
	{
		if (this->built_tiles.size() != Map::Size()) this->built_tiles.assign(Map::Size(), 0);
		uint x_end = std::min((cx + 1) * TERRAIN_CHUNK_SIZE, Map::SizeX());
		uint y_end = std::min((cy + 1) * TERRAIN_CHUNK_SIZE, Map::SizeY());
		for (uint y = cy * TERRAIN_CHUNK_SIZE; y < y_end; y++) {
			for (uint x = cx * TERRAIN_CHUNK_SIZE; x < x_end; x++) {
				uint64_t key;
				if (GetTileChangeKey(TileXY(x, y), key)) this->tile_keys[TileXY(x, y)] = key;
				this->built_tiles[TileXY(x, y).base()] = GetBuiltTileState(TileXY(x, y));
			}
		}
	}

	/**
	 * An area of tiles changed, for example the selection of a build tool; remember to rebuild the terrain there.
	 * @param x_start First tile along the x axis.
	 * @param y_start First tile along the y axis.
	 * @param x_end Last tile along the x axis.
	 * @param y_end Last tile along the y axis.
	 */
	void MarkAreaDirty(uint x_start, uint y_start, uint x_end, uint y_end)
	{
		uint cx_end = std::min(x_end / TERRAIN_CHUNK_SIZE, this->chunks_x - 1);
		uint cy_end = std::min(y_end / TERRAIN_CHUNK_SIZE, this->chunks_y - 1);
		for (uint cy = y_start / TERRAIN_CHUNK_SIZE; cy <= cy_end; cy++) {
			for (uint cx = x_start / TERRAIN_CHUNK_SIZE; cx <= cx_end; cx++) {
				this->chunks[cy * this->chunks_x + cx].dirty = true;
			}
		}
		this->terrain_changed = true;
	}

	/**
	 * Find the spot on the ground below the cursor.
	 * @param[out] pt The spot in world coordinates, or -1, -1 when the cursor is not over the map.
	 * @return Whether the cursor is over this window, so no 2D view has to be asked.
	 */
	bool GetTileBelowCursor(Point &pt) const
	{
		if (FindWindowFromPt(_cursor.pos.x, _cursor.pos.y) != this) return false;

		pt = {-1, -1};
		Vec3 eye, direction;
		if (!this->GetCursorLine(eye, direction)) return true;
		float start, end;
		float high = this->FindGroundOnLine(eye, direction, start, end);
		if (start > end) return true;

		/* A building in front of that spot is picked rather than the ground it hides. */
		float building = this->FindBuildingOnLine(eye, direction, start, high >= 0 ? high : end);
		if (building >= 0) high = building;
		if (high < 0) return true;

		Vec3 hit = eye + direction * high;
		if (hit.x < 0 || hit.y < 0 || hit.x >= Map::MaxX() * TILE_SIZE || hit.y >= Map::MaxY() * TILE_SIZE) return true;
		pt = {static_cast<int>(hit.x), static_cast<int>(hit.y)};
		return true;
	}

	/**
	 * Get the line of sight through the cursor.
	 * @param[out] eye Where the line starts.
	 * @param[out] direction Its direction, of length one.
	 * @return Whether the cursor is over the 3D image and the line goes down towards the ground.
	 */
	bool GetCursorLine(Vec3 &eye, Vec3 &direction) const
	{
		if (this->renderer == nullptr) return false;
		const NWidgetBase *view = this->GetWidget<NWidgetBase>(WID_V3D_VIEW);
		int x = _cursor.pos.x - this->left - view->pos_x;
		int y = _cursor.pos.y - this->top - view->pos_y;
		if (x < 0 || y < 0 || x >= static_cast<int>(view->current_x) || y >= static_cast<int>(view->current_y)) return false;

		eye = this->camera.GetEye();
		Vec3 forward = Normalise(this->camera.target - eye);
		Vec3 side = Normalise(Cross(forward, Vec3{0, 0, 1}));
		Vec3 up = Cross(side, forward);
		float half_height = std::tan(Camera3D::FOV_Y / 2);
		float half_width = half_height * view->current_x / view->current_y;
		direction = Normalise(forward
				+ side * (((x + 0.5f) / view->current_x * 2 - 1) * half_width)
				+ up * ((1 - (y + 0.5f) / view->current_y * 2) * half_height));
		return direction.z < -0.001f; // Otherwise it looks at the sky.
	}

	/**
	 * Find where a line of sight meets the ground.
	 * @param eye Where the line starts.
	 * @param direction Its direction, of length one, going down.
	 * @param[out] start Distance along the line where it gets low enough to meet the ground; more than \p end when the line starts below the ground.
	 * @param[out] end Distance along the line where it reaches height 0.
	 * @return Distance along the line to the ground, or -1 when it does not meet the ground on the map.
	 */
	static float FindGroundOnLine(const Vec3 &eye, const Vec3 &direction, float &start, float &end)
	{
		/* Walk along the line where it is low enough to meet the ground, until it goes below the ground. */
		static constexpr float STEP = 2.0f;
		float max_height = _settings_game.construction.map_height_limit * HEIGHT_LEVEL_SIZE;
		start = std::max(0.0f, (eye.z - max_height) / -direction.z);
		end = eye.z / -direction.z;
		auto above_ground = [&](float distance) {
			Vec3 p = eye + direction * distance;
			return p.z - GetGroundHeight(p.x, p.y);
		};
		if (above_ground(start) < 0) {
			end = start - 1;
			return -1;
		}

		float low = start;
		float high = -1;
		for (float distance = start + STEP; distance < end + STEP; distance += STEP) {
			if (above_ground(distance) < 0) {
				high = distance;
				break;
			}
			low = distance;
		}
		if (high < 0) return -1;

		/* Narrow it down to where the line meets the ground. */
		for (int i = 0; i < 10; i++) {
			float middle = (low + high) / 2;
			(above_ground(middle) < 0 ? high : low) = middle;
		}
		return high;
	}

	/**
	 * Find the first building with a 3D model along a line of sight.
	 * @param eye Where the line starts.
	 * @param direction Its direction, of length one.
	 * @param start Distance along the line from where to look.
	 * @param end Distance along the line where to stop looking, such as where it meets the ground.
	 * @return Distance along the line to a point just inside the box around the first building, or -1 when there is none.
	 */
	float FindBuildingOnLine(const Vec3 &eye, const Vec3 &direction, float start, float end) const
	{
		if (this->model_pack == nullptr || this->model_pack->IsEmpty() || end <= start) return -1;

		/* The tiles the line passes over, and their northern neighbours, whose models can reach over them. */
		static std::vector<TileIndex> tiles;
		tiles.clear();
		for (float distance = start; distance <= end + TILE_SIZE; distance += TILE_SIZE * 0.25f) {
			Vec3 p = eye + direction * std::min(distance, end);
			if (p.x < 0 || p.y < 0) continue;
			uint tx = static_cast<uint>(p.x) / TILE_SIZE, ty = static_cast<uint>(p.y) / TILE_SIZE;
			for (uint dy = 0; dy < 2; dy++) {
				for (uint dx = 0; dx < 2; dx++) {
					if (tx < dx || ty < dy || tx - dx >= Map::MaxX() || ty - dy >= Map::MaxY()) continue;
					TileIndex t = TileXY(tx - dx, ty - dy);
					if (std::ranges::find(tiles, t) == tiles.end()) tiles.push_back(t);
				}
			}
		}

		static std::vector<ModelInstance> models;
		static std::vector<AnimatedPart> parts;
		float best = -1;
		for (TileIndex t : tiles) {
			if (!GetTileModels(t, *this->model_pack, models, parts)) continue;
			for (const ModelInstance &instance : models) {
				/* The box around the model in world units, along the axes of the world. */
				const std::array<float, 6> &b = this->model_pack->GetBounds(instance.model);
				const auto &m = instance.transform.m;
				Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
				for (uint c = 0; c < 8; c++) {
					float x = b[(c & 1) ? 3 : 0], y = b[(c & 2) ? 4 : 1], z = b[(c & 4) ? 5 : 2];
					Vec3 w{m[0] * x + m[4] * y + m[8] * z + m[12], m[1] * x + m[5] * y + m[9] * z + m[13], m[2] * x + m[6] * y + m[10] * z + m[14]};
					lo = {std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)};
					hi = {std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z)};
				}
				/* Where the line enters and leaves the box. */
				float enter = start, leave = end;
				const float o[3] = {eye.x, eye.y, eye.z}, d[3] = {direction.x, direction.y, direction.z};
				const float l[3] = {lo.x, lo.y, lo.z}, h[3] = {hi.x, hi.y, hi.z};
				for (int i = 0; i < 3 && enter <= leave; i++) {
					if (std::abs(d[i]) < 1e-6f) {
						if (o[i] < l[i] || o[i] > h[i]) enter = leave + 1;
						continue;
					}
					float t0 = (l[i] - o[i]) / d[i], t1 = (h[i] - o[i]) / d[i];
					if (t0 > t1) std::swap(t0, t1);
					enter = std::max(enter, t0);
					leave = std::min(leave, t1);
				}
				if (enter > leave) continue;
				/* A little way in, so the spot is on the tiles of the building and not on the edge of its box. */
				float inside = std::min(enter + 0.5f, (enter + leave) / 2);
				if (best < 0 || inside < best) best = inside;
			}
		}
		return best;
	}

	void DrawWidget(const Rect &r, WidgetID widget) const override
	{
		switch (widget) {
			case WID_V3D_VIEW:
				this->DrawView(r);
				break;

			case WID_V3D_STATUS:
				if (this->renderer != nullptr) {
					Rect text = r.Shrink(WidgetDimensions::scaled.framerect);
					DrawString(text.left, text.right, text.top, GetString(STR_VIEW3D_STATUS, this->renderer->GetBackendName()));
				}
				break;
		}
	}

	void OnResize() override
	{
		this->view_changed = true;
	}

	void OnClick(Point, WidgetID widget, int) override
	{
		/* The left mouse button does what it does in the 2D views: use the active build tool, or click what is there. */
		if (widget != WID_V3D_VIEW || this->renderer == nullptr) return;
		const NWidgetBase *view = this->GetWidget<NWidgetBase>(WID_V3D_VIEW);
		if (view->current_x == 0 || view->current_y == 0) return;

		Point click{_cursor.pos.x - this->left - view->pos_x, _cursor.pos.y - this->top - view->pos_y};
		Projection projection = this->GetProjection(view->current_x, view->current_y);
		HandleView3DClicked(this->GetVehicleAt(click, projection), click, this->UseSmallSigns(view->current_y), projection);
	}

	bool OnRightClick(Point, WidgetID widget) override
	{
		if (widget != WID_V3D_VIEW || _scrolling_viewport) return false;
		/* Dragging with the right mouse button moves the map like in the 2D views; OnScroll() gets the movement. */
		_scrolling_viewport = true;
		_cursor.fix_at = (_settings_client.gui.scroll_mode == ViewportScrollMode::ViewportRMBFixed ||
				_settings_client.gui.scroll_mode == ViewportScrollMode::MapRMBFixed);
		return true;
	}

	void OnScroll(Point delta) override
	{
		if (IsCtrlPressed()) {
			/* Look around. */
			static constexpr float RADIANS_PER_PIXEL = 0.006f;
			this->camera.yaw -= delta.x * RADIANS_PER_PIXEL;
			this->camera.pitch += delta.y * RADIANS_PER_PIXEL;
			this->goal_yaw = this->camera.yaw;
			this->goal_pitch = Clamp(this->camera.pitch, Camera3D::MIN_PITCH, Camera3D::MAX_PITCH);
		} else {
			/* Move over the map: one pixel is this many world units at the target. */
			const NWidgetBase *view = this->GetWidget<NWidgetBase>(WID_V3D_VIEW);
			float scale = this->camera.distance * 2.0f * std::tan(Camera3D::FOV_Y / 2) / std::max<uint>(view->current_y, 1);
			/* The ground is seen at an angle, so vertical movement covers more of it. */
			float forward_scale = scale / std::max(0.2f, std::sin(this->camera.pitch));
			this->camera.target = this->camera.target + this->camera.GetRight() * (delta.x * scale) - this->camera.GetForward() * (delta.y * forward_scale);
			this->zoom_anchored = false;
		}
		this->ClampCamera();
	}

	void OnMouseWheel(int wheel, WidgetID widget) override
	{
		if (widget != WID_V3D_VIEW) return;
		this->goal_distance = Clamp(this->goal_distance * std::pow(1.25f, static_cast<float>(wheel)), Camera3D::MIN_DISTANCE, Camera3D::MAX_DISTANCE);
		/* Zoom towards the spot below the cursor, like maps do. */
		Vec3 eye, direction;
		float start, end;
		float ground = this->GetCursorLine(eye, direction) ? FindGroundOnLine(eye, direction, start, end) : -1;
		this->zoom_anchored = ground >= 0;
		if (this->zoom_anchored) this->zoom_anchor = eye + direction * ground;
	}

	EventState OnKeyPress([[maybe_unused]] char32_t key, uint16_t keycode) override
	{
		/* The directions the 2D view can be seen from: north at the top, and turned by quarters. */
		static constexpr float QUARTER = 1.5707963f;
		static constexpr float CLASSIC = 0.7853982f;
		float nearest = CLASSIC + std::round((this->goal_yaw - CLASSIC) / QUARTER) * QUARTER;
		switch (keycode) {
			case WKC_PAGEUP: // Turn a quarter to the left.
				this->goal_yaw = nearest + QUARTER;
				return EventState::Handled;
			case WKC_PAGEDOWN: // Turn a quarter to the right.
				this->goal_yaw = nearest - QUARTER;
				return EventState::Handled;
			case WKC_HOME: // Back to the view of the 2D game.
				this->goal_yaw = CLASSIC + std::round((this->goal_yaw - CLASSIC) / (4 * QUARTER)) * (4 * QUARTER);
				this->goal_pitch = Camera3D{}.pitch;
				return EventState::Handled;
			default:
				return EventState::NotHandled;
		}
	}

	/** Add see-through models of what the active build tool is about to build, on the tiles it has selected. */
	void AddPreviewModels()
	{
		if (this->model_pack == nullptr || this->model_pack->IsEmpty()) return;
		if ((_thd.drawstyle & HT_DRAG_MASK) != HT_RECT || _thd.pos.x < 0 || _thd.pos.y < 0) return;
		View3DPreview preview;
		if (!GetRailToolPreview(preview) && !GetRoadToolPreview(preview)) return;

		uint x0 = _thd.pos.x / TILE_SIZE, y0 = _thd.pos.y / TILE_SIZE;
		uint w = std::max(1, _thd.size.x / static_cast<int>(TILE_SIZE)), h = std::max(1, _thd.size.y / static_cast<int>(TILE_SIZE));
		/* A large drag would put a lot of models on the screen; the selection on the ground still shows it. */
		if (w * h > 256) return;
		for (uint y = y0; y < y0 + h && y < Map::SizeY(); y++) {
			for (uint x = x0; x < x0 + w && x < Map::SizeX(); x++) {
				AddPreviewModel(preview, TileXY(x, y), preview.axis == Axis::X ? y - y0 : x - x0, *this->model_pack, this->vehicle_models);
			}
		}
	}

	/**
	 * Let the camera glide towards where it should be.
	 * @param delta_ms Milliseconds since the last time.
	 */
	void MoveCamera(uint delta_ms)
	{
		float alpha = 1.0f - std::exp(-static_cast<float>(delta_ms) / 90.0f);
		bool moved = false;

		float ratio = this->goal_distance / this->camera.distance;
		if (std::abs(ratio - 1) > 0.001f) {
			float distance = std::abs(ratio - 1) < 0.002f ? this->goal_distance : this->camera.distance * std::pow(ratio, alpha);
			if (this->zoom_anchored) {
				/* Keep the spot below the cursor where it is on the screen. */
				float f = distance / this->camera.distance;
				this->camera.target.x = this->zoom_anchor.x + (this->camera.target.x - this->zoom_anchor.x) * f;
				this->camera.target.y = this->zoom_anchor.y + (this->camera.target.y - this->zoom_anchor.y) * f;
			}
			this->camera.distance = distance;
			moved = true;
		} else {
			this->zoom_anchored = false;
		}

		for (auto [value, goal] : {std::pair<float *, float>{&this->camera.yaw, this->goal_yaw}, {&this->camera.pitch, this->goal_pitch}}) {
			float diff = goal - *value;
			if (std::abs(diff) < 0.0005f) continue;
			*value = std::abs(diff) < 0.001f ? goal : *value + diff * alpha;
			moved = true;
		}

		/* Follow the height of the ground below the target smoothly, rather than jumping from tile to tile. */
		float ground = std::max(0.0f, GetGroundHeight(this->camera.target.x, this->camera.target.y));
		if (std::abs(ground - this->camera.target.z) > 0.01f) {
			this->camera.target.z += (ground - this->camera.target.z) * alpha;
			moved = true;
		}

		if (moved) this->ClampCamera();
	}

	void OnRealtimeTick(uint delta_ms) override
	{
		if (this->IsShaded()) return;

		if (!this->renderer_tried) {
			/* A window that was just closed may still own the renderer; wait for it to go away. */
			if (Renderer3D::Exists()) return;
			/* Created here rather than in the constructor, so it always happens on the thread that draws. */
			this->renderer_tried = true;
			this->CentreOnMainView();
			this->renderer = Renderer3D::Create(_view3d_backend, this->error);
			if (this->renderer != nullptr) {
				this->ground_textures = std::make_unique<GroundTextures>(this->renderer->GetMaxGroundTextures());
				this->atlas = std::make_unique<SpriteAtlas>();
				this->model_pack = std::make_unique<ModelPack>();
				uint id = 0;
				for (const Model3D &model : this->model_pack->GetModels()) {
					this->renderer->SetFarModel(id, this->model_pack->GetFarModel(id));
					this->renderer->SetModel(id++, model);
				}
			}
			this->SetDirty();
		}
		if (this->renderer == nullptr) return;

		this->MoveCamera(delta_ms);
		if (this->renderer->Poll()) this->SetWidgetDirty(WID_V3D_VIEW);
		if (this->renderer->IsBusy()) return;

		const NWidgetBase *view = this->GetWidget<NWidgetBase>(WID_V3D_VIEW);
		if (view->current_x == 0 || view->current_y == 0) return;

		/* Selections and new buildings keep adding ground textures. When there is no room left, start afresh
		 * with only the ones in use now; not too often, in case a single view needs more than there is room for. */
		auto now = std::chrono::steady_clock::now();
		if (this->ground_textures->IsFull() && now - this->textures_reset > std::chrono::seconds(10)) {
			this->textures_reset = now;
			this->ground_textures = std::make_unique<GroundTextures>(this->renderer->GetMaxGroundTextures());
			for (ChunkState &chunk : this->chunks) chunk.dirty = true;
			this->terrain_changed = true;
		}

		bool geometry_changed = (this->view_changed || this->terrain_changed) && this->UpdateChunks(view->current_x, view->current_y);

		/* Vehicles move all the time, so they are looked up for every image. */
		Matrix4 view_proj = this->camera.GetProjectionMatrix(static_cast<float>(view->current_x) / view->current_y, this->renderer->HomogeneousDepth()) * this->camera.GetViewMatrix();
		BuildVehicleSprites(this->camera, Frustum3D(view_proj), *this->atlas, this->model_pack.get(), this->vehicle_sprites, this->vehicle_places, this->vehicle_models);
		/* Moving parts of buildings are drawn anew for every image, like vehicles. */
		float seconds = std::chrono::duration<float>(now - this->opened).count();
		for (Renderer3D::ChunkID id : this->visible_chunks) {
			for (const AnimatedPart &part : this->chunks[id].parts) this->vehicle_models.push_back(part.Place(seconds));
			for (const auto &[tile, models] : this->chunks[id].live) this->vehicle_models.insert(this->vehicle_models.end(), models.begin(), models.end());
		}
		this->AddPreviewModels();
		uint64_t hash = 0xcbf29ce484222325ULL;
		const uint8_t *data = reinterpret_cast<const uint8_t *>(this->vehicle_sprites.data());
		for (size_t i = 0; i < this->vehicle_sprites.size() * sizeof(SpriteVertex); i++) {
			hash = (hash ^ data[i]) * 0x100000001b3ULL;
		}
		for (const ModelInstance &instance : this->vehicle_models) {
			hash = (hash ^ instance.model) * 0x100000001b3ULL;
			for (float f : instance.transform.m) hash = (hash ^ std::bit_cast<uint32_t>(f)) * 0x100000001b3ULL;
		}
		bool vehicles_changed = hash != this->vehicle_hash;

		this->renderer->UpdateGroundTextures(*this->ground_textures);
		this->renderer->UpdateSpriteAtlas(*this->atlas);
		if (!this->view_changed && !geometry_changed && !vehicles_changed) return;

		this->renderer->Render(view->current_x, view->current_y, this->camera, this->visible_chunks, this->vehicle_sprites, this->vehicle_models);
		this->view_changed = false;
		this->vehicle_hash = hash;
		this->renderer->Poll();
	}

private:
	/** Tells where a point of the map is in the image. */
	struct Projection {
		Matrix4 view_proj; ///< World to clip space.
		float width; ///< Width of the image in pixels.
		float height; ///< Height of the image in pixels.

		/**
		 * Find a point in the image.
		 * @param x Position along the x axis in world units.
		 * @param y Position along the y axis in world units.
		 * @param z Height in world units.
		 * @param[out] px Horizontal position in the image in pixels.
		 * @param[out] py Vertical position in the image in pixels.
		 * @return Whether the point is in front of the camera.
		 */
		bool Project(float x, float y, float z, float &px, float &py) const
		{
			const auto &m = this->view_proj.m;
			float w = m[3] * x + m[7] * y + m[11] * z + m[15];
			if (w < 1.0f) return false;
			px = ((m[0] * x + m[4] * y + m[8] * z + m[12]) / w * 0.5f + 0.5f) * this->width;
			py = (0.5f - (m[1] * x + m[5] * y + m[9] * z + m[13]) / w * 0.5f) * this->height;
			return true;
		}

		/** For the 2D game: the same with the height in z units of the game, leaving out what is far outside of the image. */
		bool operator()(int x, int y, int z, Point &pt) const
		{
			float px, py;
			if (!this->Project(static_cast<float>(x), static_cast<float>(y), z * WORLD_Z_SCALE, px, py)) return false;
			if (px < -this->width || px > 2 * this->width || py < -this->height || py > 2 * this->height) return false;
			pt = {static_cast<int>(px), static_cast<int>(py)};
			return true;
		}
	};

	/**
	 * Get where the points of the map are in the image.
	 * @param width Width of the image in pixels.
	 * @param height Height of the image in pixels.
	 */
	Projection GetProjection(uint width, uint height) const
	{
		Matrix4 view_proj = this->camera.GetProjectionMatrix(static_cast<float>(width) / height, this->renderer->HomogeneousDepth()) * this->camera.GetViewMatrix();
		return {view_proj, static_cast<float>(width), static_cast<float>(height)};
	}

	/**
	 * Whether names and signs are small, like in 2D views that are zoomed out four times or more.
	 * @param height Height of the image in pixels.
	 */
	bool UseSmallSigns(uint height) const
	{
		/* Size of a pixel of the 2D game in the middle of the image, in pixels. */
		float scale = height / (2.0f * this->camera.distance * std::tan(Camera3D::FOV_Y / 2)) * SPRITE_PIXEL_SIZE;
		return scale <= 0.25f;
	}

	/**
	 * Find the vehicle at a place in the image.
	 * @param pt The place in pixels.
	 * @param projection Where the points of the map are in the image.
	 * @return The vehicle of which the middle is closest, or \c nullptr when there is none.
	 */
	const Vehicle *GetVehicleAt(Point pt, const Projection &projection) const
	{
		const Vehicle *found = nullptr;
		float best = std::numeric_limits<float>::max();
		Vec3 right = this->camera.GetRight() * SPRITE_PIXEL_SIZE;

		for (const VehicleSpritePlace &place : this->vehicle_places) {
			/* The same corners as the sprite is drawn with. */
			float z = place.z * WORLD_Z_SCALE;
			float x1, y1, x2, y2;
			Vec3 top_left = Vec3{place.x, place.y, z - place.top * WORLD_Z_SCALE} + right * place.left;
			Vec3 bottom_right = Vec3{place.x, place.y, z - (place.top + place.height) * WORLD_Z_SCALE} + right * (place.left + place.width);
			if (!projection.Project(top_left.x, top_left.y, top_left.z, x1, y1)) continue;
			if (!projection.Project(bottom_right.x, bottom_right.y, bottom_right.z, x2, y2)) continue;
			if (pt.x < x1 || pt.x > x2 || pt.y < y1 || pt.y > y2) continue;

			float distance = std::max(std::abs((x1 + x2) / 2 - pt.x), std::abs((y1 + y2) / 2 - pt.y));
			if (distance >= best) continue;
			const Vehicle *v = Vehicle::GetIfValid(place.vehicle);
			if (v == nullptr) continue;
			found = v;
			best = distance;
		}
		return found;
	}

	/**
	 * Get the height of the ground as the 3D view draws it.
	 * @param x Position along the x axis in world units.
	 * @param y Position along the y axis in world units.
	 * @return The height in world units; outside of the map it is so low that nothing is below it.
	 */
	static float GetGroundHeight(float x, float y)
	{
		if (x < 0 || y < 0 || x >= Map::MaxX() * TILE_SIZE || y >= Map::MaxY() * TILE_SIZE) return -1.0e9f;

		uint tx = static_cast<uint>(x) / TILE_SIZE;
		uint ty = static_cast<uint>(y) / TILE_SIZE;
		float fx = x / TILE_SIZE - tx;
		float fy = y / TILE_SIZE - ty;
		int hn = TileHeight(TileXY(tx, ty));
		int hw = TileHeight(TileXY(tx + 1, ty));
		int he = TileHeight(TileXY(tx, ty + 1));
		int hs = TileHeight(TileXY(tx + 1, ty + 1));

		/* The same two triangles as BuildTerrainChunkMesh() makes. */
		float height;
		if (abs(hn - hs) <= abs(hw - he)) {
			if (fx >= fy) {
				height = hn + (hw - hn) * (fx - fy) + (hs - hn) * fy;
			} else {
				height = hn + (he - hn) * (fy - fx) + (hs - hn) * fx;
			}
		} else {
			if (fx + fy <= 1) {
				height = hn + (hw - hn) * fx + (he - hn) * fy;
			} else {
				height = hs + (he - hs) * (1 - fx) + (hw - hs) * (1 - fy);
			}
		}
		return height * HEIGHT_LEVEL_SIZE;
	}

	/**
	 * Look at the same spot as the main view.
	 * This is done on the first frame rather than in the constructor: when the window
	 * is opened from a start-up script, the main view is not in place yet.
	 */
	void CentreOnMainView()
	{
		Point centre{static_cast<int>(Map::SizeX() * TILE_SIZE / 2), static_cast<int>(Map::SizeY() * TILE_SIZE / 2)};
		const Window *main_window = GetMainWindow();
		if (main_window != nullptr && main_window->viewport != nullptr) {
			const Viewport &vp = *main_window->viewport;
			centre = InverseRemapCoords(vp.virtual_left + vp.virtual_width / 2, vp.virtual_top + vp.virtual_height / 2);
		}
		this->camera.target = {static_cast<float>(centre.x), static_cast<float>(centre.y), 0};
		this->ClampCamera();
		this->camera.target.z = std::max(0.0f, GetGroundHeight(this->camera.target.x, this->camera.target.y));
	}

	/** Keep the camera within sensible limits and on the ground. */
	void ClampCamera()
	{
		this->camera.pitch = Clamp(this->camera.pitch, Camera3D::MIN_PITCH, Camera3D::MAX_PITCH);
		this->camera.distance = Clamp(this->camera.distance, Camera3D::MIN_DISTANCE, Camera3D::MAX_DISTANCE);
		this->camera.target.x = Clamp(this->camera.target.x, 0.0f, static_cast<float>(Map::MaxX() * TILE_SIZE));
		this->camera.target.y = Clamp(this->camera.target.y, 0.0f, static_cast<float>(Map::MaxY() * TILE_SIZE));

		this->view_changed = true;
	}

	/**
	 * Work out which chunks are in view, and build or rebuild those that need it.
	 * Building is spread over several frames when there is a lot to do.
	 * @param width Width of the image in pixels.
	 * @param height Height of the image in pixels.
	 * @return Whether the geometry of any visible chunk changed.
	 */
	bool UpdateChunks(uint width, uint height)
	{
		static constexpr std::chrono::milliseconds BUILD_BUDGET{6};
		auto start = std::chrono::steady_clock::now();

		Matrix4 view_proj = this->camera.GetProjectionMatrix(static_cast<float>(width) / height, this->renderer->HomogeneousDepth()) * this->camera.GetViewMatrix();
		Frustum3D frustum(view_proj);
		/* Room above the ground for what stands on it. */
		static constexpr float HEADROOM = 48.0f;
		float max_height = _settings_game.construction.map_height_limit * HEIGHT_LEVEL_SIZE + HEADROOM;

		this->visible_chunks.clear();
		bool changed = false;
		bool out_of_time = false;
		/* Only one chunk a frame is rebuilt for slow changes, so they never stall a frame. */
		bool slow_built = false;
		auto now = start;
		bool slow_waiting = false;

		for (uint cy = 0; cy < this->chunks_y; cy++) {
			for (uint cx = 0; cx < this->chunks_x; cx++) {
				Renderer3D::ChunkID id = cy * this->chunks_x + cx;
				ChunkState &chunk = this->chunks[id];

				Vec3 min{static_cast<float>(cx * TERRAIN_CHUNK_SIZE * TILE_SIZE), static_cast<float>(cy * TERRAIN_CHUNK_SIZE * TILE_SIZE), 0};
				Vec3 max{min.x + TERRAIN_CHUNK_SIZE * TILE_SIZE, min.y + TERRAIN_CHUNK_SIZE * TILE_SIZE, max_height};
				if (chunk.built && !chunk.dirty) {
					min.z = chunk.min_height * HEIGHT_LEVEL_SIZE;
					max.z = chunk.max_height * HEIGHT_LEVEL_SIZE + HEADROOM;
				}
				if (!frustum.IntersectsBox(min, max)) continue;

				bool slow_due = chunk.slow && now - chunk.slow_since > SLOW_CHANGE_DELAY;
				if (chunk.slow && !slow_due) slow_waiting = true;
				if (!chunk.built || chunk.dirty || slow_due) {
					if (out_of_time || (slow_due && !chunk.dirty && slow_built)) {
						/* Draw what we have for now; the rest follows in the next frames. */
						if (chunk.built) this->visible_chunks.push_back(id);
						continue;
					}

					BuildTerrainChunkMesh(cx, cy, *this->ground_textures, *this->atlas, this->model_pack.get(), this->scratch_mesh);
					if (!chunk.built || chunk.hash != this->scratch_mesh.hash) {
						this->renderer->SetChunk(id, this->scratch_mesh);
						changed = true;
					}
					chunk.built = true;
					if (slow_due && !chunk.dirty) slow_built = true;
					chunk.dirty = false;
					chunk.slow = false;
					chunk.hash = this->scratch_mesh.hash;
					chunk.min_height = this->scratch_mesh.min_height;
					chunk.max_height = this->scratch_mesh.max_height;
					chunk.parts = this->scratch_mesh.parts;
					chunk.live = this->scratch_mesh.live;
					this->RememberTileKeys(cx, cy);

					if (std::chrono::steady_clock::now() - start > BUILD_BUDGET) out_of_time = true;
				}
				this->visible_chunks.push_back(id);
			}
		}

		/* Chunks that are dirty but out of view stay dirty, and are rebuilt when they come into view. */
		this->terrain_changed = out_of_time || slow_waiting || slow_built;
		return changed;
	}

	/**
	 * Draw the most recent 3D image into the window.
	 * @param r Area of the window to draw in.
	 */
	void DrawView(const Rect &r) const
	{
		/* Fill with palette index 0. Blitters with an animation buffer take the colour of a pixel
		 * from the palette unless its index is 0, and the 3D image has no palette indices. */
		GfxFillRect(r, PixelColour{0});

		Blitter *blitter = BlitterFactory::GetCurrentBlitter();
		if (blitter->GetScreenDepth() != 32) {
			DrawStringMultiLine(r.Shrink(WidgetDimensions::scaled.frametext), GetString(STR_VIEW3D_ERROR_BLITTER), TextColour::White, AlignmentH::Centre);
			return;
		}
		if (this->renderer == nullptr) {
			if (this->renderer_tried) DrawStringMultiLine(r.Shrink(WidgetDimensions::scaled.frametext), GetString(STR_VIEW3D_ERROR_NO_RENDERER, this->error), TextColour::White, AlignmentH::Centre);
			return;
		}

		/* Copy the image straight into the 32bpp screen buffer, clipped to both the widget and the area being redrawn.
		 * The image can briefly be of a different size than the widget while the window is being resized. */
		std::span<const uint32_t> image = this->renderer->GetImage();
		int image_width = this->renderer->GetImageWidth();
		int image_height = this->renderer->GetImageHeight();
		if (image.empty()) return;

		const DrawPixelInfo *dpi = _cur_dpi;
		int left = std::max(r.left, dpi->left);
		int top = std::max(r.top, dpi->top);
		int right = std::min({r.right + 1, r.left + image_width, dpi->left + dpi->width});
		int bottom = std::min({r.bottom + 1, r.top + image_height, dpi->top + dpi->height});
		if (left >= right || top >= bottom) return;

		for (int y = top; y < bottom; y++) {
			const uint32_t *src = image.data() + static_cast<size_t>(y - r.top) * image_width + (left - r.left);
			uint32_t *dst = static_cast<uint32_t *>(blitter->MoveTo(dpi->dst_ptr, left - dpi->left, y - dpi->top));
			std::copy_n(src, right - left, dst);
		}

		/* Names and signs go on top, drawn by the 2D game and of the same size wherever they are, like in the 2D views. */
		DrawPixelInfo sign_dpi;
		if (!FillDrawPixelInfo(&sign_dpi, r)) return;
		AutoRestoreBackup dpi_backup(_cur_dpi, &sign_dpi);
		DrawView3DSigns(this->UseSmallSigns(r.Height()), this->GetProjection(r.Width(), r.Height()));
	}
};

static constexpr std::initializer_list<NWidgetPart> _nested_view3d_widgets = {
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_CLOSEBOX, Colours::Grey),
		NWidget(WWT_CAPTION, Colours::Grey, WID_V3D_CAPTION), SetStringTip(STR_VIEW3D_CAPTION, STR_TOOLTIP_WINDOW_TITLE_DRAG_THIS),
		NWidget(WWT_SHADEBOX, Colours::Grey),
		NWidget(WWT_DEFSIZEBOX, Colours::Grey),
		NWidget(WWT_STICKYBOX, Colours::Grey),
	EndContainer(),
	NWidget(WWT_PANEL, Colours::Grey),
		NWidget(WWT_EMPTY, Colours::Invalid, WID_V3D_VIEW), SetMinimalSize(320, 200), SetFill(1, 1), SetResize(1, 1), SetPadding(2),
	EndContainer(),
	NWidget(NWID_HORIZONTAL),
		NWidget(WWT_PANEL, Colours::Grey, WID_V3D_STATUS), SetMinimalTextLines(1, WidgetDimensions::unscaled.framerect.Vertical()), SetFill(1, 0), SetResize(1, 0),
		EndContainer(),
		NWidget(WWT_RESIZEBOX, Colours::Grey),
	EndContainer(),
};

/** Window definition for the 3D view. */
static WindowDesc _view3d_desc(
	WindowPosition::Automatic, "view3d", 640, 440,
	WindowClass::View3D, WindowClass::None,
	{},
	_nested_view3d_widgets
);

/**
 * Open the 3D view, or bring it to the front.
 * @param backend Graphics API to use ("vulkan", "opengl" or "metal"); empty to pick automatically.
 */
void ShowView3DWindow(std::string_view backend)
{
	if (backend != _view3d_backend) {
		/* Switching graphics API needs a new renderer. */
		CloseWindowById(WindowClass::View3D, 0);
		_view3d_backend = backend;
	}
	AllocateWindowDescFront<View3DWindow>(_view3d_desc, 0);
}

/**
 * Tell the 3D view that a tile changed.
 * This is called very often, so it must stay cheap when the 3D view is closed.
 * @param tile The tile that changed.
 */
void MarkTileDirtyForView3D(TileIndex tile)
{
	if (_view3d_window != nullptr) _view3d_window->MarkTileDirty(tile);
}

/**
 * Tell the 3D view that an area of tiles changed.
 * @param x_start First tile along the x axis.
 * @param y_start First tile along the y axis.
 * @param x_end Last tile along the x axis.
 * @param y_end Last tile along the y axis.
 */
void MarkAreaDirtyForView3D(uint x_start, uint y_start, uint x_end, uint y_end)
{
	if (_view3d_window != nullptr) _view3d_window->MarkAreaDirty(x_start, y_start, x_end, y_end);
}

/**
 * Find the spot on the ground below the cursor, when the cursor is over the 3D view.
 * @param[out] pt The spot in world coordinates, or -1, -1 when the cursor is not over the map.
 * @return Whether the cursor is over the 3D view; \a pt is only set when it is.
 */
bool GetView3DTileBelowCursor(Point &pt)
{
	return _view3d_window != nullptr && _view3d_window->GetTileBelowCursor(pt);
}
