/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file renderer3d.cpp GPU side of the 3D viewport, implemented with bgfx. */

#include "../stdafx.h"
#include "renderer3d.h"
#include "../debug.h"
#include "../tile_type.h"

#include <bgfx/bgfx.h>

#include "shaders/vs_terrain.bin.h"
#include "shaders/fs_terrain.bin.h"

#include "../safeguards.h"

/** Receives messages from bgfx. */
struct Renderer3DCallback : public bgfx::CallbackI {
	bool failed = false; ///< Whether bgfx reported an error it cannot recover from.

	void fatal(const char *, uint16_t, bgfx::Fatal::Enum code, const char *str) override
	{
		Debug(Facility::Driver, Severity::Error, "[viewport3d] bgfx error {}: {}", static_cast<int>(code), str);
		this->failed = true;
	}

	void traceVargs(const char *, uint16_t, const char *, va_list) override {}
	void profilerBegin(const char *, uint32_t, const char *, uint16_t) override {}
	void profilerBeginLiteral(const char *, uint32_t, const char *, uint16_t) override {}
	void profilerEnd() override {}
	uint32_t cacheReadSize(uint64_t) override { return 0; }
	bool cacheRead(uint64_t, void *, uint32_t) override { return false; }
	void cacheWrite(uint64_t, const void *, uint32_t) override {}
	void screenShot(const char *, uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, const void *, uint32_t, bool) override {}
	void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
	void captureEnd() override {}
	void captureFrame(const void *, uint32_t) override {}
};

/** GPU buffers of one terrain chunk. */
struct GpuChunk {
	bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE;
	uint32_t num_quads = 0;
};

struct Renderer3D::Impl {
	Renderer3DCallback callback;
	bool initialised = false;
	bgfx::VertexLayout layout;
	bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
	bgfx::IndexBufferHandle quad_indices = BGFX_INVALID_HANDLE; ///< Indices for a full chunk of quads, shared by all chunks.
	bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle readback = BGFX_INVALID_HANDLE;
	uint fb_width = 0;
	uint fb_height = 0;
	std::unordered_map<ChunkID, GpuChunk> chunks;
};

/** A compiled shader for one kind of backend. */
struct ShaderBlob {
	const uint8_t *data;
	uint32_t size;
};

/**
 * Get the precompiled variant of the terrain shaders that fits a backend.
 * @param type The backend.
 * @param[out] vs The vertex shader.
 * @param[out] fs The fragment shader.
 * @return Whether shaders for this backend are available.
 */
static bool GetShaders(bgfx::RendererType::Enum type, ShaderBlob &vs, ShaderBlob &fs)
{
	switch (type) {
		case bgfx::RendererType::OpenGL:
			vs = {vs_terrain_glsl, sizeof(vs_terrain_glsl)};
			fs = {fs_terrain_glsl, sizeof(fs_terrain_glsl)};
			return true;
		case bgfx::RendererType::OpenGLES:
			vs = {vs_terrain_essl, sizeof(vs_terrain_essl)};
			fs = {fs_terrain_essl, sizeof(fs_terrain_essl)};
			return true;
		case bgfx::RendererType::Vulkan:
			vs = {vs_terrain_spv, sizeof(vs_terrain_spv)};
			fs = {fs_terrain_spv, sizeof(fs_terrain_spv)};
			return true;
		case bgfx::RendererType::Metal:
			vs = {vs_terrain_mtl, sizeof(vs_terrain_mtl)};
			fs = {fs_terrain_mtl, sizeof(fs_terrain_mtl)};
			return true;
		default:
			return false;
	}
}

/**
 * Start the renderer on the first backend that works.
 * @param[out] error Explanation for the user when no backend works.
 * @return The renderer, or \c nullptr on failure.
 */
/* static */ std::unique_ptr<Renderer3D> Renderer3D::Create(std::string &error)
{
	/* Backends in order of preference. Direct3D is missing because its shaders can only be compiled on Windows. */
	static constexpr bgfx::RendererType::Enum BACKENDS[] = {
#if defined(__APPLE__)
		bgfx::RendererType::Metal,
#else
		bgfx::RendererType::Vulkan,
		bgfx::RendererType::OpenGL,
#endif
	};

	std::unique_ptr<Renderer3D> renderer(new Renderer3D());
	renderer->impl = std::make_unique<Impl>();
	Impl &impl = *renderer->impl;

	static constexpr uint64_t NEEDED_CAPS = BGFX_CAPS_TEXTURE_BLIT | BGFX_CAPS_TEXTURE_READ_BACK;

	for (bgfx::RendererType::Enum type : BACKENDS) {
		bgfx::Init init;
		init.type = type;
		init.fallback = false;
		/* No window: bgfx creates a device that can only render offscreen. */
		init.swapChain.width = 0;
		init.swapChain.height = 0;
		init.callback = &impl.callback;
		impl.callback.failed = false;

		if (!bgfx::init(init)) {
			Debug(Facility::Driver, Severity::Error, "[viewport3d] backend {} is not available", bgfx::getRendererName(type));
			continue;
		}
		if (impl.callback.failed || (bgfx::getCaps()->supported & NEEDED_CAPS) != NEEDED_CAPS) {
			Debug(Facility::Driver, Severity::Error, "[viewport3d] backend {} cannot read back images", bgfx::getRendererName(type));
			bgfx::shutdown();
			continue;
		}
		impl.initialised = true;
		break;
	}

	if (!impl.initialised) {
		error = "No usable Vulkan or OpenGL graphics driver was found.";
		return nullptr;
	}
	Debug(Facility::Driver, Severity::Error, "[viewport3d] using bgfx backend {}", bgfx::getRendererName(bgfx::getRendererType()));

	impl.layout.begin()
		.add(bgfx::Attrib::Position, 4, bgfx::AttribType::Int16)
		.add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
		.end();

	ShaderBlob vs, fs;
	[[maybe_unused]] bool has_shaders = GetShaders(bgfx::getRendererType(), vs, fs);
	assert(has_shaders);
	impl.program = bgfx::createProgram(
		bgfx::createShader(bgfx::makeRef(vs.data, vs.size)),
		bgfx::createShader(bgfx::makeRef(fs.data, fs.size)),
		true);

	/* Two triangles per quad, split along the diagonal between vertex 1 and 2. */
	static constexpr uint MAX_QUADS = TERRAIN_CHUNK_SIZE * TERRAIN_CHUNK_SIZE;
	const bgfx::Memory *mem = bgfx::alloc(MAX_QUADS * 6 * sizeof(uint16_t));
	uint16_t *index = reinterpret_cast<uint16_t *>(mem->data);
	for (uint i = 0; i < MAX_QUADS; i++) {
		uint16_t base = static_cast<uint16_t>(i * 4);
		for (uint16_t offset : {0, 1, 2, 2, 1, 3}) *index++ = base + offset;
	}
	impl.quad_indices = bgfx::createIndexBuffer(mem);

	return renderer;
}

Renderer3D::~Renderer3D()
{
	Impl &impl = *this->impl;
	if (!impl.initialised) return;

	/* An image may still be on its way into pending_image; let it arrive before the memory goes away. */
	for (int i = 0; this->pending && i < 8; i++) this->Poll();

	this->ClearChunks();
	if (bgfx::isValid(impl.framebuffer)) bgfx::destroy(impl.framebuffer);
	if (bgfx::isValid(impl.readback)) bgfx::destroy(impl.readback);
	if (bgfx::isValid(impl.quad_indices)) bgfx::destroy(impl.quad_indices);
	if (bgfx::isValid(impl.program)) bgfx::destroy(impl.program);
	bgfx::shutdown();
}

/** Name of the graphics API in use, for display. */
std::string_view Renderer3D::GetBackendName() const
{
	return bgfx::getRendererName(bgfx::getRendererType());
}

/** Whether clip space depth runs from -1 to 1 instead of 0 to 1 on this backend. */
bool Renderer3D::HomogeneousDepth() const
{
	return bgfx::getCaps()->homogeneousDepth;
}

/**
 * Upload or replace the geometry of a terrain chunk.
 * @param id Identifier of the chunk, chosen by the caller.
 * @param mesh The geometry.
 */
void Renderer3D::SetChunk(ChunkID id, const TerrainChunkMesh &mesh)
{
	GpuChunk &chunk = this->impl->chunks[id];
	if (bgfx::isValid(chunk.vertices)) bgfx::destroy(chunk.vertices);
	chunk.vertices = BGFX_INVALID_HANDLE;
	chunk.num_quads = static_cast<uint32_t>(mesh.vertices.size() / 4);
	if (chunk.num_quads == 0) return;

	chunk.vertices = bgfx::createVertexBuffer(
		bgfx::copy(mesh.vertices.data(), static_cast<uint32_t>(mesh.vertices.size() * sizeof(TerrainVertex))),
		this->impl->layout);
}

/** Remove all terrain chunks from the GPU. */
void Renderer3D::ClearChunks()
{
	for (auto &[id, chunk] : this->impl->chunks) {
		if (bgfx::isValid(chunk.vertices)) bgfx::destroy(chunk.vertices);
	}
	this->impl->chunks.clear();
}

/**
 * Start drawing a new image. The result arrives a few calls to Poll() later.
 * Must not be called while IsBusy().
 * @param width Width of the image in pixels.
 * @param height Height of the image in pixels.
 * @param camera Where to look from.
 * @param chunks The chunks to draw.
 */
void Renderer3D::Render(uint width, uint height, const Camera3D &camera, std::span<const ChunkID> chunks)
{
	assert(!this->pending);
	Impl &impl = *this->impl;
	if (impl.callback.failed) return;

	width = Clamp(width, 1, 4096);
	height = Clamp(height, 1, 4096);

	if (width != impl.fb_width || height != impl.fb_height) {
		if (bgfx::isValid(impl.framebuffer)) bgfx::destroy(impl.framebuffer);
		if (bgfx::isValid(impl.readback)) bgfx::destroy(impl.readback);

		bgfx::TextureFormat::Enum depth_format = bgfx::TextureFormat::D24S8;
		for (bgfx::TextureFormat::Enum candidate : {bgfx::TextureFormat::D32F, bgfx::TextureFormat::D24S8, bgfx::TextureFormat::D24, bgfx::TextureFormat::D16}) {
			if (bgfx::isTextureValid(0, false, 1, candidate, BGFX_TEXTURE_RT_WRITE_ONLY)) {
				depth_format = candidate;
				break;
			}
		}

		bgfx::TextureHandle attachments[] = {
			bgfx::createTexture2D(static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP),
			bgfx::createTexture2D(static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1, depth_format, BGFX_TEXTURE_RT_WRITE_ONLY),
		};
		impl.framebuffer = bgfx::createFrameBuffer(static_cast<uint8_t>(std::size(attachments)), attachments, true);
		impl.readback = bgfx::createTexture2D(static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
		impl.fb_width = width;
		impl.fb_height = height;
	}

	static constexpr bgfx::ViewId VIEW_SCENE = 0;
	static constexpr bgfx::ViewId VIEW_READBACK = 1;

	Matrix4 view = camera.GetViewMatrix();
	Matrix4 proj = camera.GetProjectionMatrix(static_cast<float>(width) / height, bgfx::getCaps()->homogeneousDepth);

	bgfx::setViewFrameBuffer(VIEW_SCENE, impl.framebuffer);
	bgfx::setViewRect(VIEW_SCENE, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
	bgfx::setViewClear(VIEW_SCENE, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, 0x8fb8e0ff, 1.0f, 0);
	bgfx::setViewTransform(VIEW_SCENE, view.m.data(), proj.m.data());
	bgfx::touch(VIEW_SCENE);

	/* Vertices are in tiles and height levels; the model matrix turns them into world units. */
	Matrix4 model;
	model.m[0] = TILE_SIZE;
	model.m[5] = TILE_SIZE;
	model.m[10] = TILE_HEIGHT;
	model.m[15] = 1;

	for (ChunkID id : chunks) {
		auto it = impl.chunks.find(id);
		if (it == impl.chunks.end() || !bgfx::isValid(it->second.vertices)) continue;

		bgfx::setTransform(model.m.data());
		bgfx::setVertexBuffer(0, it->second.vertices);
		bgfx::setIndexBuffer(impl.quad_indices, 0, it->second.num_quads * 6);
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
		bgfx::submit(VIEW_SCENE, impl.program);
	}

	/* Copy the finished image into a texture the CPU can read, and ask for its contents. */
	bgfx::blit(VIEW_READBACK, bgfx::TextureRegion{.handle = impl.readback}, bgfx::TextureRegion{.handle = bgfx::getTexture(impl.framebuffer, 0)});

	this->pending_image.resize(static_cast<size_t>(width) * height);
	this->pending_width = width;
	this->pending_height = height;
	this->pending_frame = bgfx::read(bgfx::TextureRegion{.handle = impl.readback}, this->pending_image.data());
	this->pending = true;
}

/**
 * Let the GPU make progress on a requested image. Call once per display frame while IsBusy().
 * @return Whether a new image became available through GetImage().
 */
bool Renderer3D::Poll()
{
	if (!this->pending) return false;
	if (this->impl->callback.failed) {
		this->pending = false;
		return false;
	}

	uint32_t frame = bgfx::frame();
	if (frame < this->pending_frame) return false;

	this->pending = false;
	this->image_width = this->pending_width;
	this->image_height = this->pending_height;

	if (bgfx::getCaps()->originBottomLeft) {
		/* The image arrives with the bottom row first; flip it. */
		this->image.resize(this->pending_image.size());
		for (uint y = 0; y < this->image_height; y++) {
			std::copy_n(this->pending_image.begin() + static_cast<size_t>(this->image_height - 1 - y) * this->image_width, this->image_width, this->image.begin() + static_cast<size_t>(y) * this->image_width);
		}
	} else {
		std::swap(this->image, this->pending_image);
	}
	/* Blitters treat the top byte as alpha; make sure the image is opaque. */
	for (uint32_t &pixel : this->image) pixel |= 0xFF000000;
	return true;
}
