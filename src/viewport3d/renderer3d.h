/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file renderer3d.h GPU side of the 3D viewport: draws terrain chunks offscreen and hands back the pixels. */

#ifndef VIEWPORT3D_RENDERER3D_H
#define VIEWPORT3D_RENDERER3D_H

#include "camera3d.h"
#include "ground_textures.h"
#include "sprite_atlas.h"
#include "terrain_mesh.h"

/**
 * Offscreen renderer for the 3D viewport.
 *
 * The renderer owns its own GPU device, separate from the video driver, and renders
 * into an offscreen image that is read back into normal memory. The window then
 * draws those pixels like any other part of the GUI. This keeps the 2D game, its
 * video drivers and blitters completely unaware of the 3D view.
 *
 * All methods must be called from the thread that draws the windows.
 */
class Renderer3D {
public:
	/** Identifies a terrain chunk on the GPU. */
	using ChunkID = uint32_t;

	static std::unique_ptr<Renderer3D> Create(std::string_view backend, std::string &error);
	~Renderer3D();

	/** Whether a renderer currently exists; there can only be one at a time. */
	static bool Exists() { return Renderer3D::exists; }

	std::string_view GetBackendName() const;

	uint GetMaxGroundTextures() const;
	void UpdateGroundTextures(GroundTextures &textures);
	void UpdateSpriteAtlas(SpriteAtlas &atlas);

	void SetChunk(ChunkID id, const TerrainChunkMesh &mesh);
	void ClearChunks();

	bool IsBusy() const { return this->pending; }
	void Render(uint width, uint height, const Camera3D &camera, std::span<const ChunkID> chunks);
	bool Poll();

	bool HomogeneousDepth() const;

	/** Width of the last finished image in pixels. */
	uint GetImageWidth() const { return this->image_width; }
	/** Height of the last finished image in pixels. */
	uint GetImageHeight() const { return this->image_height; }
	/** Pixels of the last finished image, top row first, in the format of 32bpp blitters. */
	std::span<const uint32_t> GetImage() const { return this->image; }

private:
	Renderer3D() = default;

	static inline bool exists = false; ///< Whether a renderer currently exists.

	struct Impl;
	std::unique_ptr<Impl> impl; ///< bgfx state; kept out of the header so only the renderer includes bgfx.

	bool pending = false; ///< Whether an image has been requested but not yet received.
	uint32_t pending_frame = 0; ///< Frame number at which the requested image is complete.
	uint pending_width = 0; ///< Width of the requested image.
	uint pending_height = 0; ///< Height of the requested image.
	std::vector<uint32_t> pending_image; ///< Destination of the requested image; must stay put while pending.

	uint image_width = 0; ///< Width of the last finished image.
	uint image_height = 0; ///< Height of the last finished image.
	std::vector<uint32_t> image; ///< The last finished image.
};

#endif /* VIEWPORT3D_RENDERER3D_H */
