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
		uint64_t hash = 0; ///< Hash of the geometry on the GPU.
		uint min_height = 0; ///< Lowest height level in the chunk; only valid when built.
		uint max_height = 0; ///< Highest height level in the chunk; only valid when built.
		std::vector<AnimatedPart> parts; ///< Parts of models in the chunk that move.
	};

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
		if (this->renderer == nullptr) return true;
		const NWidgetBase *view = this->GetWidget<NWidgetBase>(WID_V3D_VIEW);
		int x = _cursor.pos.x - this->left - view->pos_x;
		int y = _cursor.pos.y - this->top - view->pos_y;
		if (x < 0 || y < 0 || x >= static_cast<int>(view->current_x) || y >= static_cast<int>(view->current_y)) return true;

		/* The line of sight through the cursor. */
		Vec3 eye = this->camera.GetEye();
		Vec3 forward = Normalise(this->camera.target - eye);
		Vec3 side = Normalise(Cross(forward, Vec3{0, 0, 1}));
		Vec3 up = Cross(side, forward);
		float half_height = std::tan(Camera3D::FOV_Y / 2);
		float half_width = half_height * view->current_x / view->current_y;
		Vec3 direction = Normalise(forward
				+ side * (((x + 0.5f) / view->current_x * 2 - 1) * half_width)
				+ up * ((1 - (y + 0.5f) / view->current_y * 2) * half_height));
		if (direction.z > -0.001f) return true; // Looking at the sky.

		/* Walk along the line where it is low enough to meet the ground, until it goes below the ground. */
		static constexpr float STEP = 2.0f;
		float max_height = _settings_game.construction.map_height_limit * HEIGHT_LEVEL_SIZE;
		float start = std::max(0.0f, (eye.z - max_height) / -direction.z);
		float end = eye.z / -direction.z;
		auto above_ground = [&](float distance) {
			Vec3 p = eye + direction * distance;
			return p.z - GetGroundHeight(p.x, p.y);
		};
		if (above_ground(start) < 0) return true;

		float low = start;
		float high = -1;
		for (float distance = start + STEP; distance < end + STEP; distance += STEP) {
			if (above_ground(distance) < 0) {
				high = distance;
				break;
			}
			low = distance;
		}
		if (high < 0) return true;

		/* Narrow it down to where the line meets the ground. */
		for (int i = 0; i < 10; i++) {
			float middle = (low + high) / 2;
			(above_ground(middle) < 0 ? high : low) = middle;
		}

		Vec3 hit = eye + direction * high;
		if (hit.x < 0 || hit.y < 0 || hit.x >= Map::MaxX() * TILE_SIZE || hit.y >= Map::MaxY() * TILE_SIZE) return true;
		pt = {static_cast<int>(hit.x), static_cast<int>(hit.y)};
		return true;
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
		} else {
			/* Move over the map: one pixel is this many world units at the target. */
			const NWidgetBase *view = this->GetWidget<NWidgetBase>(WID_V3D_VIEW);
			float scale = this->camera.distance * 2.0f * std::tan(Camera3D::FOV_Y / 2) / std::max<uint>(view->current_y, 1);
			/* The ground is seen at an angle, so vertical movement covers more of it. */
			float forward_scale = scale / std::max(0.2f, std::sin(this->camera.pitch));
			this->camera.target = this->camera.target + this->camera.GetRight() * (delta.x * scale) - this->camera.GetForward() * (delta.y * forward_scale);
		}
		this->ClampCamera();
	}

	void OnMouseWheel(int wheel, WidgetID widget) override
	{
		if (widget != WID_V3D_VIEW) return;
		this->camera.distance *= std::pow(1.15f, static_cast<float>(wheel));
		this->ClampCamera();
	}

	void OnRealtimeTick([[maybe_unused]] uint delta_ms) override
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
				for (const Model3D &model : this->model_pack->GetModels()) this->renderer->SetModel(id++, model);
			}
			this->SetDirty();
		}
		if (this->renderer == nullptr) return;

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
		}
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
	}

	/** Keep the camera within sensible limits and on the ground. */
	void ClampCamera()
	{
		this->camera.pitch = Clamp(this->camera.pitch, Camera3D::MIN_PITCH, Camera3D::MAX_PITCH);
		this->camera.distance = Clamp(this->camera.distance, Camera3D::MIN_DISTANCE, Camera3D::MAX_DISTANCE);
		this->camera.target.x = Clamp(this->camera.target.x, 0.0f, static_cast<float>(Map::MaxX() * TILE_SIZE));
		this->camera.target.y = Clamp(this->camera.target.y, 0.0f, static_cast<float>(Map::MaxY() * TILE_SIZE));

		TileIndex tile = TileXY(std::min(static_cast<uint>(this->camera.target.x) / TILE_SIZE, Map::MaxX()), std::min(static_cast<uint>(this->camera.target.y) / TILE_SIZE, Map::MaxY()));
		this->camera.target.z = TileHeight(tile) * HEIGHT_LEVEL_SIZE;
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

				if (!chunk.built || chunk.dirty) {
					if (out_of_time) {
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
					chunk.dirty = false;
					chunk.hash = this->scratch_mesh.hash;
					chunk.min_height = this->scratch_mesh.min_height;
					chunk.max_height = this->scratch_mesh.max_height;
					chunk.parts = this->scratch_mesh.parts;

					if (std::chrono::steady_clock::now() - start > BUILD_BUDGET) out_of_time = true;
				}
				this->visible_chunks.push_back(id);
			}
		}

		/* Chunks that are dirty but out of view stay dirty, and are rebuilt when they come into view. */
		this->terrain_changed = out_of_time;
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
