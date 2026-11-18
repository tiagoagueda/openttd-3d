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
#include "../viewport_func.h"
#include "../zoom_func.h"
#include "../error.h"
#include "../openttd.h"
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

	bool main_view = false; ///< Whether this is the 3D view in place of the main view, rather than a window of its own.
	Point main_scrollpos{INT_MIN, INT_MIN}; ///< Where the 2D main view was when we last looked; it moves us when it moves.
	ZoomLevel main_zoom = ZoomLevel::Normal; ///< The zoom of the 2D main view when we last looked.
	std::array<float, 2> pushed_target{-1, -1}; ///< The camera target that the 2D main view was last moved to.
	bool terrain_changed = true; ///< Whether chunks need to be (re)built.

	uint chunks_x = 0; ///< Number of chunks along the x axis.
	uint chunks_y = 0; ///< Number of chunks along the y axis.
	std::vector<ChunkState> chunks; ///< State of all chunks, row by row.
	std::vector<Renderer3D::ChunkID> visible_chunks; ///< Chunks to draw this frame.
	TerrainChunkMesh scratch_mesh; ///< Reused while building chunks.
	std::unique_ptr<GroundTextures> ground_textures; ///< The ground textures that the chunks use.
	bool terrain_textures = false; ///< Whether the landscape has the ground textures of the model package.
	std::unique_ptr<SpriteAtlas> atlas; ///< The sprites of the 2D game that the chunks use.
	std::unique_ptr<ModelPack> model_pack; ///< The package of 3D models, if there is one.
	std::vector<ModelInstance> vehicle_models; ///< The vehicles in view that are drawn with a 3D model.
	std::vector<SpriteVertex> vehicle_sprites; ///< The vehicles in view.
	std::vector<VehicleSpritePlace> vehicle_places; ///< Where the vehicles in view are, to find the one below the cursor.
	uint64_t vehicle_hash = 0; ///< Hash of the vehicles in the last image, to see whether any moved.

	/** Where a label is in the last image, to find the one below the cursor. */
	struct LabelPlace {
		View3DLabel label; ///< The label.
		float left, top, right, bottom; ///< Its corners in the image, in pixels.
		float depth; ///< Distance from the camera, to pick the nearest of labels that overlap.
	};
	std::vector<View3DLabel> labels; ///< The town names, station names and signs; reused for every image.
	std::vector<LabelPlace> label_places; ///< Where the labels are in the last image.
	std::chrono::steady_clock::time_point labels_found{}; ///< When the labels were last looked up.

	std::chrono::steady_clock::time_point textures_reset{}; ///< When the ground textures were last started afresh.

	/** A viewport of another window, such as a news message, a town, vehicle or industry window or an extra viewport, that is drawn in 3D. */
	struct SmallView {
		WindowClass window_class; ///< Class of the window.
		WindowNumber window_number; ///< Number of the window.
		uint target; ///< Id of its image in the renderer.
		Camera3D camera; ///< Where it looks from.
		uint width = 0; ///< Width of the viewport in pixels.
		uint height = 0; ///< Height of the viewport in pixels.
		bool changed = true; ///< Whether the camera or size changed since the last image.
		uint64_t hash = 0; ///< Hash of the vehicles in the last image.
		std::chrono::steady_clock::time_point rendered{}; ///< When it was last looked at for a new image.
	};
	std::vector<SmallView> small_views; ///< The viewports of other windows.
	uint next_small_target = 1; ///< Id for the image of the next new small view; 0 is our own.
	size_t next_small_view = 0; ///< Small view to look at first for the next image, so they all get their turn.
	bool small_turn = false; ///< Whether the next image is for a small view rather than our own.
	std::vector<Renderer3D::ChunkID> small_chunks; ///< Chunks in a small view; reused.
	std::vector<SpriteVertex> small_sprites; ///< Vehicles in a small view; reused.
	std::vector<VehicleSpritePlace> small_places; ///< Places of the vehicles in a small view; not used, but needed to find them.
	std::vector<ModelInstance> small_models; ///< Models in a small view; reused.

public:
	View3DWindow(WindowDesc &desc, WindowNumber window_number) : Window(desc), main_view(desc.cls == WindowClass::View3DMain)
	{
		this->InitNested(window_number);
		if (this->main_view) {
			/* Cover the whole screen, like the main window does. */
			this->left = 0;
			this->top = 0;
			ResizeWindow(this, _screen.width - this->width, _screen.height - this->height);
			const Window *main_window = GetMainWindow();
			if (main_window != nullptr && main_window->viewport != nullptr) this->main_zoom = main_window->viewport->zoom;
			/* Start as close to the map as the 2D view is: one pixel of a sprite covers as many pixels of the screen. */
			float distance = _screen.height * SPRITE_PIXEL_SIZE / (2.0f * std::tan(Camera3D::FOV_Y / 2)) * std::pow(2.0f, static_cast<float>(to_underlying(this->main_zoom) - to_underlying(ZoomLevel::Normal)));
			this->goal_distance = this->camera.distance = Clamp(distance, Camera3D::MIN_DISTANCE, Camera3D::MAX_DISTANCE);
		}

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
		HandleView3DClicked(this->GetVehicleAt(click, projection), this->GetLabelAt(click));
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

	/**
	 * Go where the 2D main view goes: the arrow keys, the minimap, following a vehicle and "go to location" all move
	 * that view, and its zoom buttons zoom us.
	 */
	void FollowMainView()
	{
		const Window *main_window = GetMainWindow();
		if (main_window == nullptr || main_window->viewport == nullptr) return;
		const ViewportData &vp = *main_window->viewport;

		if (vp.zoom != this->main_zoom) {
			this->goal_distance = Clamp(this->goal_distance * std::pow(2.0f, static_cast<float>(to_underlying(vp.zoom) - to_underlying(this->main_zoom))), Camera3D::MIN_DISTANCE, Camera3D::MAX_DISTANCE);
			this->zoom_anchored = false;
			this->main_zoom = vp.zoom;
		}

		Point pos{vp.scrollpos_x, vp.scrollpos_y};
		if (pos == this->main_scrollpos) return;
		this->main_scrollpos = pos;
		Point centre = InverseRemapCoords2(pos.x + vp.virtual_width / 2, pos.y + vp.virtual_height / 2, true);
		this->camera.target.x = static_cast<float>(centre.x);
		this->camera.target.y = static_cast<float>(centre.y);
		this->zoom_anchored = false;
		this->ClampCamera();
		this->pushed_target = {this->camera.target.x, this->camera.target.y};
	}

	/** Keep the 2D main view where the camera looks, so the minimap and everything else that asks it stay right. */
	void MoveMainView()
	{
		if (this->pushed_target[0] == this->camera.target.x && this->pushed_target[1] == this->camera.target.y) return;
		this->pushed_target = {this->camera.target.x, this->camera.target.y};
		Window *main_window = GetMainWindow();
		if (main_window == nullptr || main_window->viewport == nullptr) return;
		ScrollWindowTo(static_cast<int>(this->camera.target.x), static_cast<int>(this->camera.target.y), -1, main_window, true);
		this->main_scrollpos = {main_window->viewport->scrollpos_x, main_window->viewport->scrollpos_y};
	}

	/**
	 * Stand the town names, station names and signs on the map: each on a board on a post, facing the camera.
	 * They keep about the size of the 2D views at the spot the camera looks at; nearer ones are larger and further ones smaller.
	 * @param view_proj World to clip space.
	 * @param width Width of the image in pixels.
	 * @param height Height of the image in pixels.
	 */
	void AddLabels(const Matrix4 &view_proj, uint width, uint height)
	{
		this->label_places.clear();
		/* Names change seldom, and looking them all up takes a while on big maps, so only a few times a second. */
		auto now = std::chrono::steady_clock::now();
		if (now - this->labels_found > std::chrono::milliseconds(400)) {
			this->labels_found = now;
			this->atlas->BeginLabels();
			GetView3DLabels(this->labels);
		}
		if (this->labels.empty()) return;

		/* Size of a pixel of the 2D game at the target, in pixels of the image. */
		float scale = height / (2.0f * this->camera.distance * std::tan(Camera3D::FOV_Y / 2)) * SPRITE_PIXEL_SIZE;
		/* Zoomed out far, the 2D views use small labels; here they shrink a little instead. */
		float grow = std::max(0.75f, (scale <= 0.25f ? 0.7f : 1.0f) / scale);

		Frustum3D frustum(view_proj);
		Projection projection{view_proj, static_cast<float>(width), static_cast<float>(height)};
		Vec3 right = this->camera.GetRight() * (SPRITE_PIXEL_SIZE * grow);
		Vec3 eye = this->camera.GetEye();
		const ModelPack::Entry *post = this->model_pack != nullptr ? this->model_pack->Find(ModelKind::Interface, 0, 0) : nullptr;

		for (size_t i = 0; i < this->labels.size(); i++) {
			const View3DLabel &label = this->labels[i];
			/* How high the board stands above the ground, in z units: town names above the houses, stations above their buildings. */
			static constexpr int LIFT[] = {56, 18, 28}; // town, sign, station
			float x = static_cast<float>(label.x) + TILE_SIZE / 2.0f;
			float y = static_cast<float>(label.y) + TILE_SIZE / 2.0f;
			float ground = std::max(0.0f, GetGroundHeight(x, y));
			float z = (label.z + LIFT[to_underlying(label.kind)]) * WORLD_Z_SCALE;
			float top = z + label.height * WORLD_Z_SCALE * grow;

			Vec3 half = right * (label.width * 0.5f);
			Vec3 lo{x - std::abs(half.x), y - std::abs(half.y), ground};
			Vec3 hi{x + std::abs(half.x), y + std::abs(half.y), top};
			if (!frustum.IntersectsBox(lo, hi)) continue;

			const AtlasSprite &sprite = this->atlas->GetLabel(label.key, label.width, label.height, [i]() { DrawView3DLabel(i); });
			if (!sprite.valid) continue;

			/* The board: the offsets are in pixels of the 2D game, made larger or smaller by grow. */
			float vx = x / TILE_SIZE, vy = y / TILE_SIZE, vz = z / HEIGHT_LEVEL_SIZE;
			float l = sprite.left * grow, r = (sprite.left + sprite.width) * grow, t = sprite.height * grow;
			/* Labels count as a little nearer, so the buildings they stand among do not cut into them; 1000 marks them as labels. */
			static constexpr float NEARER = 1000.0f + 24.0f;
			this->vehicle_sprites.insert(this->vehicle_sprites.end(), {
				{vx, vy, vz, l, t, sprite.u0, sprite.v0, NEARER},
				{vx, vy, vz, r, t, sprite.u1, sprite.v0, NEARER},
				{vx, vy, vz, l, 0, sprite.u0, sprite.v1, NEARER},
				{vx, vy, vz, r, 0, sprite.u1, sprite.v1, NEARER},
			});

			/* The post, from the ground up to the board. */
			if (post != nullptr && z > ground) {
				ModelInstance &instance = this->vehicle_models.emplace_back();
				instance.model = post->model;
				float w = grow;
				instance.transform.m = {
					w, 0, 0, 0,
					0, w, 0, 0,
					0, 0, z - ground + 0.5f, 0,
					x, y, ground, 1,
				};
				instance.tint = {1, 1, 1, 1};
			}

			/* Remember where it is in the image, for clicks. */
			Vec3 top_left = Vec3{x, y, top} - half;
			Vec3 bottom_right = Vec3{x, y, z} + half;
			float x1, y1, x2, y2;
			if (!projection.Project(top_left.x, top_left.y, top_left.z, x1, y1)) continue;
			if (!projection.Project(bottom_right.x, bottom_right.y, bottom_right.z, x2, y2)) continue;
			Vec3 d = Vec3{x, y, z} - eye;
			this->label_places.push_back({label, std::min(x1, x2), std::min(y1, y2), std::max(x1, x2), std::max(y1, y2), Dot(d, d)});
		}
	}

	/**
	 * Get the height of a corner of a tile.
	 * @param x X coordinate of the corner, which is the northern corner of tile (x, y).
	 * @param y Y coordinate of the corner.
	 * @return The height in world units.
	 */
	static float GetCornerHeight(uint x, uint y)
	{
		return TileHeight(TileXY(std::min(x, Map::MaxX()), std::min(y, Map::MaxY()))) * HEIGHT_LEVEL_SIZE;
	}

	/**
	 * Add one part of the cursor: a model stretched from one corner of a tile to another, following the ground.
	 * @param entry The model, one unit long along x.
	 * @param ax X coordinate of the first corner.
	 * @param ay Y coordinate of the first corner.
	 * @param bx X coordinate of the second corner.
	 * @param by Y coordinate of the second corner.
	 * @param tint Colour of the model.
	 */
	void AddCursorEdge(const ModelPack::Entry &entry, uint ax, uint ay, uint bx, uint by, const std::array<float, 4> &tint)
	{
		static constexpr float LIFT = 0.15f;
		Vec3 a{static_cast<float>(ax * TILE_SIZE), static_cast<float>(ay * TILE_SIZE), GetCornerHeight(ax, ay) + LIFT};
		Vec3 b{static_cast<float>(bx * TILE_SIZE), static_cast<float>(by * TILE_SIZE), GetCornerHeight(bx, by) + LIFT};
		Vec3 along = b - a;
		Vec3 side = Normalise(Cross(Vec3{0, 0, 1}, along));
		ModelInstance &instance = this->vehicle_models.emplace_back();
		instance.model = entry.model;
		instance.transform.m = {
			along.x, along.y, along.z, 0,
			side.x, side.y, side.z, 0,
			0, 0, 1, 0,
			a.x, a.y, a.z, 1,
		};
		instance.tint = tint;
	}

	/**
	 * Add one part of the cursor that stands upright.
	 * @param entry The model.
	 * @param x Position along the x axis in world units.
	 * @param y Position along the y axis in world units.
	 * @param z Height in world units.
	 * @param yaw How far it is turned around the vertical axis, in radians.
	 * @param tint Colour of the model.
	 */
	void AddCursorPart(const ModelPack::Entry &entry, float x, float y, float z, float yaw, const std::array<float, 4> &tint)
	{
		float c = std::cos(yaw), s = std::sin(yaw);
		ModelInstance &instance = this->vehicle_models.emplace_back();
		instance.model = entry.model;
		instance.transform.m = {
			c, s, 0, 0,
			-s, c, 0, 0,
			0, 0, 1, 0,
			x, y, z, 1,
		};
		instance.tint = tint;
	}

	/**
	 * Add the cursor in the world: a frame around the tiles that a tool has selected, with an arrow that
	 * hovers over the tile below the mouse; without a tool, brackets on the corners of that tile.
	 * @param seconds Seconds since the window opened, for the movement of the arrow.
	 */
	void AddCursorModels(float seconds)
	{
		if (this->model_pack == nullptr || FindWindowFromPt(_cursor.pos.x, _cursor.pos.y) != this) return;
		const ModelPack::Entry *edge = this->model_pack->Find(ModelKind::Interface, 0, 1);
		const ModelPack::Entry *corner = this->model_pack->Find(ModelKind::Interface, 0, 2);
		const ModelPack::Entry *arrow = this->model_pack->Find(ModelKind::Interface, 0, 3);
		const ModelPack::Entry *bracket = this->model_pack->Find(ModelKind::Interface, 0, 4);

		Point below;
		if (!this->GetTileBelowCursor(below) || below.x < 0 || below.y < 0) return;
		uint tx = below.x / TILE_SIZE, ty = below.y / TILE_SIZE;
		if (tx >= Map::MaxX() || ty >= Map::MaxY()) return;

		static const std::array<float, 4> WHITE = {1.0f, 1.0f, 1.0f, 1.0f};
		static const std::array<float, 4> YELLOW = {1.0f, 0.82f, 0.25f, 1.0f};
		HighLightStyle style = _thd.drawstyle & HT_DRAG_MASK;
		bool tool = _thd.place_mode != HT_NONE || style != HT_NONE;

		if (!tool) {
			if (bracket == nullptr) return;
			/* Brackets on the four corners of the tile, pointing inwards. */
			static constexpr int CORNERS[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
			for (int i = 0; i < 4; i++) {
				uint cx = tx + CORNERS[i][0], cy = ty + CORNERS[i][1];
				float yaw = i * 1.5707963f;
				this->AddCursorPart(*bracket, static_cast<float>(cx * TILE_SIZE), static_cast<float>(cy * TILE_SIZE), GetCornerHeight(cx, cy) + 0.1f, yaw, WHITE);
			}
			return;
		}

		/* The frame around a rectangle of selected tiles; for tools that build along lines, around the tile below the mouse. */
		if (edge != nullptr && corner != nullptr) {
			uint x0 = tx, y0 = ty, x1 = tx + 1, y1 = ty + 1;
			if ((style == HT_RECT || style == HT_POINT) && _thd.pos.x >= 0 && _thd.pos.y >= 0) {
				x0 = _thd.pos.x / TILE_SIZE;
				y0 = _thd.pos.y / TILE_SIZE;
				x1 = std::min<uint>(x0 + std::max(1, _thd.size.x / static_cast<int>(TILE_SIZE)), Map::MaxX());
				y1 = std::min<uint>(y0 + std::max(1, _thd.size.y / static_cast<int>(TILE_SIZE)), Map::MaxY());
			}
			/* A very large selection would need very many parts; the selection on the ground still shows it. */
			if ((x1 - x0) + (y1 - y0) <= 512) {
				for (uint x = x0; x < x1; x++) {
					this->AddCursorEdge(*edge, x, y0, x + 1, y0, WHITE);
					this->AddCursorEdge(*edge, x, y1, x + 1, y1, WHITE);
				}
				for (uint y = y0; y < y1; y++) {
					this->AddCursorEdge(*edge, x0, y, x0, y + 1, WHITE);
					this->AddCursorEdge(*edge, x1, y, x1, y + 1, WHITE);
				}
				for (auto [cx, cy] : {std::pair{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}}) {
					this->AddCursorPart(*corner, static_cast<float>(cx * TILE_SIZE), static_cast<float>(cy * TILE_SIZE), GetCornerHeight(cx, cy), 0, WHITE);
				}
			}
		}

		/* The arrow, bobbing and turning slowly over the tile below the mouse. */
		if (arrow != nullptr) {
			float top = std::max({GetCornerHeight(tx, ty), GetCornerHeight(tx + 1, ty), GetCornerHeight(tx, ty + 1), GetCornerHeight(tx + 1, ty + 1)});
			/* Above what stands on the tile, so it is not hidden inside a building. */
			float z = top + 9.0f + 1.2f * std::sin(seconds * 3.0f);
			this->AddCursorPart(*arrow, (tx + 0.5f) * TILE_SIZE, (ty + 0.5f) * TILE_SIZE, z, seconds * 1.2f, YELLOW);
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
				if (this->model_pack->HasGround()) {
					this->terrain_textures = this->renderer->SetTerrainTextures(this->model_pack->GetGroundImage("grass")->size, this->model_pack->GetGroundTiles(), this->ground_textures->MakeTerrainTextures(*this->model_pack));
					if (this->terrain_textures) this->ground_textures->UseTerrain();
				}
				if (this->model_pack->GetModelTextureSize() != 0) this->renderer->SetModelTextures(this->model_pack->GetModelTextureSize(), this->model_pack->GetModelTextures());
				uint id = 0;
				for (const Model3D &model : this->model_pack->GetModels()) {
					this->renderer->SetFarModel(id, this->model_pack->GetFarModel(id));
					this->renderer->SetModel(id++, model);
				}
			}
			this->SetDirty();
		}
		if (this->renderer == nullptr) {
			if (this->main_view) {
				/* Fall back to the 2D view, and say why. */
				ShowErrorMessage(GetEncodedString(STR_VIEW3D_ERROR_NO_RENDERER, this->error), {}, WarningLevel::Error);
				this->Close();
			}
			return;
		}

		if (this->main_view) this->FollowMainView();
		this->MoveCamera(delta_ms);
		if (this->main_view) this->MoveMainView();
		this->PollRenderer();
		if (this->renderer->IsBusy()) return;

		/* Our own view and the viewports of other windows take turns. */
		auto now = std::chrono::steady_clock::now();
		if (this->main_view) {
			this->ForgetClosedSmallViews();
			this->small_turn = !this->small_turn;
			if (this->small_turn && this->RenderSmallView(now)) return;
		}

		const NWidgetBase *view = this->GetWidget<NWidgetBase>(WID_V3D_VIEW);
		if (view->current_x == 0 || view->current_y == 0) return;

		/* Selections and new buildings keep adding ground textures. When there is no room left, start afresh
		 * with only the ones in use now; not too often, in case a single view needs more than there is room for. */
		if (this->ground_textures->IsFull() && now - this->textures_reset > std::chrono::seconds(10)) {
			this->textures_reset = now;
			this->ground_textures = std::make_unique<GroundTextures>(this->renderer->GetMaxGroundTextures());
			if (this->terrain_textures) this->ground_textures->UseTerrain();
			for (ChunkState &chunk : this->chunks) chunk.dirty = true;
			this->terrain_changed = true;
		}

		bool geometry_changed = (this->view_changed || this->terrain_changed) && this->UpdateChunks(this->camera, view->current_x, view->current_y, this->visible_chunks, true);

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
		this->AddLabels(view_proj, view->current_x, view->current_y);
		this->AddCursorModels(seconds);
		uint64_t hash = HashMoving(this->vehicle_sprites, this->vehicle_models);
		bool vehicles_changed = hash != this->vehicle_hash;

		this->renderer->UpdateGroundTextures(*this->ground_textures);
		this->renderer->UpdateSpriteAtlas(*this->atlas);
		if (!this->view_changed && !geometry_changed && !vehicles_changed) return;

		this->renderer->Render(0, view->current_x, view->current_y, this->camera, this->visible_chunks, this->vehicle_sprites, this->vehicle_models);
		this->view_changed = false;
		this->vehicle_hash = hash;
		this->PollRenderer();
	}

	/**
	 * Draw a viewport of another window from its latest 3D image, and ask for a new image when it moved.
	 * @param w The window.
	 * @param left Left edge of the area to draw, in pixels of the screen.
	 * @param top Top edge of the area to draw.
	 * @param right Right edge of the area to draw, exclusive.
	 * @param bottom Bottom edge of the area to draw, exclusive.
	 * @return Whether it was drawn; if not, there is no image yet and the 2D view has to do.
	 */
	bool DrawSmallView(const Window &w, int left, int top, int right, int bottom)
	{
		if (this->renderer == nullptr || !this->main_view) return false;
		const Viewport &vp = *w.viewport;
		if (vp.width <= 0 || vp.height <= 0) return false;

		auto it = std::ranges::find_if(this->small_views, [&w](const SmallView &view) { return view.window_class == w.window_class && view.window_number == w.window_number; });
		if (it == this->small_views.end()) {
			it = this->small_views.emplace(this->small_views.end());
			it->window_class = w.window_class;
			it->window_number = w.window_number;
			it->target = this->next_small_target++;
		}

		/* Look at the spot the 2D view would show in its middle, from as far as makes one pixel of a sprite as large. */
		Camera3D camera;
		Point centre = InverseRemapCoords2(vp.virtual_left + vp.virtual_width / 2, vp.virtual_top + vp.virtual_height / 2, true);
		camera.target.x = Clamp(static_cast<float>(centre.x), 0.0f, static_cast<float>(Map::MaxX() * TILE_SIZE));
		camera.target.y = Clamp(static_cast<float>(centre.y), 0.0f, static_cast<float>(Map::MaxY() * TILE_SIZE));
		camera.target.z = std::max(0.0f, GetGroundHeight(camera.target.x, camera.target.y));
		float distance = UnScaleByZoom(vp.virtual_height, ZoomLevel::Normal) * SPRITE_PIXEL_SIZE / (2.0f * std::tan(Camera3D::FOV_Y / 2));
		camera.distance = Clamp(distance, Camera3D::MIN_DISTANCE, Camera3D::MAX_DISTANCE);
		if (camera.target.x != it->camera.target.x || camera.target.y != it->camera.target.y || camera.target.z != it->camera.target.z ||
				camera.distance != it->camera.distance || it->width != static_cast<uint>(vp.width) || it->height != static_cast<uint>(vp.height)) {
			it->camera = camera;
			it->width = vp.width;
			it->height = vp.height;
			it->changed = true;
		}

		std::span<const uint32_t> image = this->renderer->GetImage(it->target);
		int image_width = this->renderer->GetImageWidth(it->target);
		int image_height = this->renderer->GetImageHeight(it->target);
		if (image.empty()) return false;

		/* Fill with palette index 0 first, for blitters with an animation buffer; see DrawView(). */
		GfxFillRect(left, top, right - 1, bottom - 1, PixelColour{0});
		const DrawPixelInfo *dpi = _cur_dpi;
		Blitter *blitter = BlitterFactory::GetCurrentBlitter();
		int copy_right = std::min({right, vp.left + image_width, dpi->left + dpi->width});
		int copy_bottom = std::min({bottom, vp.top + image_height, dpi->top + dpi->height});
		left = std::max(left, dpi->left);
		top = std::max(top, dpi->top);
		for (int y = top; y < copy_bottom; y++) {
			if (left >= copy_right) break;
			const uint32_t *src = image.data() + static_cast<size_t>(y - vp.top) * image_width + (left - vp.left);
			uint32_t *dst = static_cast<uint32_t *>(blitter->MoveTo(dpi->dst_ptr, left - dpi->left, y - dpi->top));
			std::copy_n(src, copy_right - left, dst);
		}
		return true;
	}

private:
	/**
	 * Hash what moves in an image, to see whether anything moved since the last one.
	 * @param sprites The vehicles drawn with sprites.
	 * @param models Everything drawn with models that is looked up anew for every image.
	 * @return The hash.
	 */
	static uint64_t HashMoving(std::span<const SpriteVertex> sprites, std::span<const ModelInstance> models)
	{
		uint64_t hash = 0xcbf29ce484222325ULL;
		const uint8_t *data = reinterpret_cast<const uint8_t *>(sprites.data());
		for (size_t i = 0; i < sprites.size() * sizeof(SpriteVertex); i++) {
			hash = (hash ^ data[i]) * 0x100000001b3ULL;
		}
		for (const ModelInstance &instance : models) {
			hash = (hash ^ instance.model) * 0x100000001b3ULL;
			for (float f : instance.transform.m) hash = (hash ^ std::bit_cast<uint32_t>(f)) * 0x100000001b3ULL;
		}
		return hash;
	}

	/** Take a finished image, and have whichever view it is for drawn again. */
	void PollRenderer()
	{
		uint target;
		if (!this->renderer->Poll(&target)) return;
		if (target == 0) {
			this->SetWidgetDirty(WID_V3D_VIEW);
			return;
		}
		for (const SmallView &view : this->small_views) {
			if (view.target != target) continue;
			const Window *w = FindWindowById(view.window_class, view.window_number);
			if (w != nullptr && w->viewport != nullptr) AddDirtyBlock(w->viewport->left, w->viewport->top, w->viewport->left + w->viewport->width, w->viewport->top + w->viewport->height);
			return;
		}
	}

	/** Forget the viewports of windows that closed. */
	void ForgetClosedSmallViews()
	{
		for (auto it = this->small_views.begin(); it != this->small_views.end();) {
			const Window *w = FindWindowById(it->window_class, it->window_number);
			if (w != nullptr && w->viewport != nullptr) {
				++it;
				continue;
			}
			this->renderer->ReleaseTarget(it->target);
			it = this->small_views.erase(it);
		}
	}

	/**
	 * Make a new image for one of the viewports of other windows, when one moved or something in it might have.
	 * @param now The time now.
	 * @return Whether an image was asked for.
	 */
	bool RenderSmallView(std::chrono::steady_clock::time_point now)
	{
		/* Vehicles and animations in a viewport that did not move are looked at a few times a second. */
		static constexpr std::chrono::milliseconds RECHECK{200};
		size_t count = this->small_views.size();
		for (size_t i = 0; i < count; i++) {
			size_t index = (this->next_small_view + i) % count;
			SmallView &view = this->small_views[index];
			if (!view.changed && now - view.rendered < RECHECK) continue;
			view.rendered = now;

			/* Chunks only seen here are built here; then our own view may have to be drawn again too. */
			if (this->UpdateChunks(view.camera, view.width, view.height, this->small_chunks, false)) this->view_changed = true;

			Matrix4 view_proj = view.camera.GetProjectionMatrix(static_cast<float>(view.width) / view.height, this->renderer->HomogeneousDepth()) * view.camera.GetViewMatrix();
			BuildVehicleSprites(view.camera, Frustum3D(view_proj), *this->atlas, this->model_pack.get(), this->small_sprites, this->small_places, this->small_models);
			float seconds = std::chrono::duration<float>(now - this->opened).count();
			for (Renderer3D::ChunkID id : this->small_chunks) {
				for (const AnimatedPart &part : this->chunks[id].parts) this->small_models.push_back(part.Place(seconds));
				for (const auto &[tile, models] : this->chunks[id].live) this->small_models.insert(this->small_models.end(), models.begin(), models.end());
			}
			uint64_t hash = HashMoving(this->small_sprites, this->small_models);
			if (!view.changed && hash == view.hash) continue;

			this->renderer->UpdateGroundTextures(*this->ground_textures);
			this->renderer->UpdateSpriteAtlas(*this->atlas);
			this->renderer->Render(view.target, view.width, view.height, view.camera, this->small_chunks, this->small_sprites, this->small_models);
			view.changed = false;
			view.hash = hash;
			this->next_small_view = index + 1;
			return true;
		}
		return false;
	}

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
	 * Find the label at a place in the image.
	 * @param pt The place in pixels.
	 * @return The nearest label there, or \c nullptr when there is none.
	 */
	const View3DLabel *GetLabelAt(Point pt) const
	{
		const LabelPlace *found = nullptr;
		for (const LabelPlace &place : this->label_places) {
			if (pt.x < place.left || pt.x > place.right || pt.y < place.top || pt.y > place.bottom) continue;
			if (found == nullptr || place.depth < found->depth) found = &place;
		}
		return found != nullptr ? &found->label : nullptr;
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
	 * @param camera Where the view looks from.
	 * @param width Width of the image in pixels.
	 * @param height Height of the image in pixels.
	 * @param[out] visible The chunks to draw.
	 * @param own Whether it is our own view, rather than a viewport of another window.
	 * @return Whether the geometry of any visible chunk changed.
	 */
	bool UpdateChunks(const Camera3D &camera, uint width, uint height, std::vector<Renderer3D::ChunkID> &visible, bool own)
	{
		static constexpr std::chrono::milliseconds BUILD_BUDGET{6};
		auto start = std::chrono::steady_clock::now();

		Matrix4 view_proj = camera.GetProjectionMatrix(static_cast<float>(width) / height, this->renderer->HomogeneousDepth()) * camera.GetViewMatrix();
		Frustum3D frustum(view_proj);
		/* Room above the ground for what stands on it. */
		static constexpr float HEADROOM = 48.0f;
		float max_height = _settings_game.construction.map_height_limit * HEIGHT_LEVEL_SIZE + HEADROOM;

		visible.clear();
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
						if (chunk.built) visible.push_back(id);
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
				visible.push_back(id);
			}
		}

		/* Chunks that are dirty but out of view stay dirty, and are rebuilt when they come into view. */
		if (own) {
			this->terrain_changed = out_of_time || slow_waiting || slow_built;
		} else if (out_of_time || slow_waiting || slow_built) {
			/* Not all of it is ready yet; look again soon. */
			changed = true;
		}
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

static constexpr std::initializer_list<NWidgetPart> _nested_view3d_main_widgets = {
	NWidget(WWT_EMPTY, Colours::Invalid, WID_V3D_VIEW), SetResize(1, 1), SetFill(1, 1),
};

/** Window definition for the 3D view in place of the main view. */
static WindowDesc _view3d_main_desc(
	WindowPosition::Manual, {}, 0, 0,
	WindowClass::View3DMain, WindowClass::None,
	{WindowDefaultFlag::NoClose},
	_nested_view3d_main_widgets
);

/**
 * Show the map in 3D in place of the main view, or stop doing so, as the setting says.
 * Called when the main window is made and when the setting changes.
 */
void UpdateView3DMainView()
{
	/* Without a 32bpp blitter there is no 3D image to show, so the 2D view stays. */
	bool wanted = _settings_client.gui.view3d_main && _game_mode != GameMode::Menu && GetMainWindow() != nullptr &&
			BlitterFactory::GetCurrentBlitter()->GetScreenDepth() == 32;
	Window *w = FindWindowById(WindowClass::View3DMain, 0);
	if (!wanted) {
		if (w != nullptr) w->Close();
		return;
	}
	if (w != nullptr) return;
	/* There is one renderer, so the 3D window makes way. */
	CloseWindowById(WindowClass::View3D, 0);
	_view3d_backend.clear();
	new View3DWindow(_view3d_main_desc, 0);
}

/**
 * Open the 3D view, or bring it to the front.
 * @param backend Graphics API to use ("vulkan", "opengl" or "metal"); empty to pick automatically.
 */
void ShowView3DWindow(std::string_view backend)
{
	/* The main view is in 3D already. */
	if (FindWindowById(WindowClass::View3DMain, 0) != nullptr) return;
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
 * Draw the viewport of a window, such as a news message, a town or vehicle window or an extra viewport, in 3D when the map is shown in 3D.
 * @param w The window; not the main window.
 * @param left Left edge of the area to draw, in pixels of the screen.
 * @param top Top edge of the area to draw.
 * @param right Right edge of the area to draw, exclusive.
 * @param bottom Bottom edge of the area to draw, exclusive.
 * @return Whether it was drawn; if not, it is drawn in 2D.
 */
bool DrawViewportIn3D(const Window &w, int left, int top, int right, int bottom)
{
	return _view3d_window != nullptr && _view3d_window->DrawSmallView(w, left, top, right, bottom);
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
