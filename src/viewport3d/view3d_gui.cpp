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
#include "terrain_mesh.h"
#include "../blitter/factory.hpp"
#include "../gfx_func.h"
#include "../landscape.h"
#include "../map_func.h"
#include "../palette_func.h"
#include "../settings_type.h"
#include "../strings_func.h"
#include "../tile_map.h"
#include "../viewport_type.h"
#include "../window_func.h"
#include "../window_gui.h"

#include "table/strings.h"

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
	};

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

	bool panning = false; ///< Whether the view is being dragged with the left mouse button.
	Point pan_last{}; ///< Cursor position at the previous step of the drag.

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
		if (widget != WID_V3D_VIEW) return;
		this->panning = true;
		this->pan_last = _cursor.pos;
	}

	void OnMouseLoop() override
	{
		if (!this->panning) return;
		if (!IsLeftButtonDown()) {
			this->panning = false;
			return;
		}

		Point delta{_cursor.pos.x - this->pan_last.x, _cursor.pos.y - this->pan_last.y};
		if (delta.x == 0 && delta.y == 0) return;
		this->pan_last = _cursor.pos;

		/* Move the ground along with the cursor: one pixel is this many world units at the target. */
		const NWidgetBase *view = this->GetWidget<NWidgetBase>(WID_V3D_VIEW);
		float scale = this->camera.distance * 2.0f * std::tan(Camera3D::FOV_Y / 2) / std::max<uint>(view->current_y, 1);
		/* The ground is seen at an angle, so vertical cursor movement covers more of it. */
		float forward_scale = scale / std::max(0.2f, std::sin(this->camera.pitch));
		this->camera.target = this->camera.target - this->camera.GetRight() * (delta.x * scale) + this->camera.GetForward() * (delta.y * forward_scale);
		this->ClampCamera();
	}

	bool OnRightClick(Point, WidgetID widget) override
	{
		if (widget != WID_V3D_VIEW || _scrolling_viewport) return false;
		_scrolling_viewport = true;
		return true;
	}

	void OnScroll(Point delta) override
	{
		static constexpr float RADIANS_PER_PIXEL = 0.006f;
		this->camera.yaw -= delta.x * RADIANS_PER_PIXEL;
		this->camera.pitch += delta.y * RADIANS_PER_PIXEL;
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
			this->SetDirty();
		}
		if (this->renderer == nullptr) return;

		if (this->renderer->Poll()) this->SetWidgetDirty(WID_V3D_VIEW);
		if (this->renderer->IsBusy() || (!this->view_changed && !this->terrain_changed)) return;

		const NWidgetBase *view = this->GetWidget<NWidgetBase>(WID_V3D_VIEW);
		if (view->current_x == 0 || view->current_y == 0) return;

		bool geometry_changed = this->UpdateChunks(view->current_x, view->current_y);
		if (!this->view_changed && !geometry_changed) return;

		this->renderer->Render(view->current_x, view->current_y, this->camera, this->visible_chunks);
		this->view_changed = false;
		this->renderer->Poll();
	}

private:
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
		this->camera.target.z = static_cast<float>(TileHeight(tile) * TILE_HEIGHT);
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
		float max_height = static_cast<float>(_settings_game.construction.map_height_limit * TILE_HEIGHT);

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
					min.z = static_cast<float>(chunk.min_height * TILE_HEIGHT);
					max.z = static_cast<float>(chunk.max_height * TILE_HEIGHT);
				}
				if (!frustum.IntersectsBox(min, max)) continue;

				if (!chunk.built || chunk.dirty) {
					if (out_of_time) {
						/* Draw what we have for now; the rest follows in the next frames. */
						if (chunk.built) this->visible_chunks.push_back(id);
						continue;
					}

					BuildTerrainChunkMesh(cx, cy, this->scratch_mesh);
					if (!chunk.built || chunk.hash != this->scratch_mesh.hash) {
						this->renderer->SetChunk(id, this->scratch_mesh);
						changed = true;
					}
					chunk.built = true;
					chunk.dirty = false;
					chunk.hash = this->scratch_mesh.hash;
					chunk.min_height = this->scratch_mesh.min_height;
					chunk.max_height = this->scratch_mesh.max_height;

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
		GfxFillRect(r, PC_BLACK);

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
