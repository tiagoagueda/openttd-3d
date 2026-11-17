/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file renderer3d.cpp GPU side of the 3D viewport, implemented with bgfx. */

#include "../stdafx.h"
#include "renderer3d.h"
#include "../core/math_func.hpp"
#include "../debug.h"
#include "../string_func.h"
#include "../tile_type.h"

#include <bgfx/bgfx.h>

#include "shaders/vs_terrain.bin.h"
#include "shaders/fs_terrain.bin.h"
#include "shaders/vs_sprite.bin.h"
#include "shaders/fs_sprite.bin.h"

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
	bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE; ///< The terrain.
	uint32_t num_quads = 0; ///< Number of tiles of terrain.
	bgfx::VertexBufferHandle sprites = BGFX_INVALID_HANDLE; ///< The sprites standing on the terrain.
	uint32_t num_sprites = 0; ///< Number of sprites.
};

struct Renderer3D::Impl {
	Renderer3DCallback callback;
	bool initialised = false;
	bgfx::VertexLayout layout;
	bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
	bgfx::VertexLayout sprite_layout;
	bgfx::ProgramHandle sprite_program = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle sprite_sampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle sprite_axes = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle sprite_nearer = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle sprite_atlas = BGFX_INVALID_HANDLE; ///< The sprites of the 2D game that are in use.
	bgfx::UniformHandle ground_sampler = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle ground_textures = BGFX_INVALID_HANDLE; ///< Texture array with one layer per #GroundTexture.
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

/** The precompiled variants of a shader. */
struct ShaderVariants {
	ShaderBlob glsl, essl, spv, mtl;
};
#define SHADER_VARIANTS(name) ShaderVariants{{name##_glsl, sizeof(name##_glsl)}, {name##_essl, sizeof(name##_essl)}, {name##_spv, sizeof(name##_spv)}, {name##_mtl, sizeof(name##_mtl)}}

/**
 * Create a shader from the precompiled variant that fits the backend in use.
 * @param variants The variants of the shader.
 * @return The shader.
 */
static bgfx::ShaderHandle CreateShader(const ShaderVariants &variants)
{
	const ShaderBlob *blob;
	switch (bgfx::getRendererType()) {
		case bgfx::RendererType::OpenGL: blob = &variants.glsl; break;
		case bgfx::RendererType::OpenGLES: blob = &variants.essl; break;
		case bgfx::RendererType::Vulkan: blob = &variants.spv; break;
		case bgfx::RendererType::Metal: blob = &variants.mtl; break;
		default: NOT_REACHED();
	}
	return bgfx::createShader(bgfx::makeRef(blob->data, blob->size));
}

/**
 * Start the renderer on the first backend that works.
 * @param backend Name of the only backend to try ("vulkan", "opengl" or "metal"); empty to try all.
 * @param[out] error Explanation for the user when no backend works.
 * @return The renderer, or \c nullptr on failure.
 */
/* static */ std::unique_ptr<Renderer3D> Renderer3D::Create(std::string_view backend, std::string &error)
{
	/** A backend the renderer can run on. */
	struct Backend {
		bgfx::RendererType::Enum type;
		std::string_view name;
	};
	/* Backends in order of preference. Direct3D is missing because its shaders can only be compiled on Windows. */
	static constexpr Backend BACKENDS[] = {
#if defined(__APPLE__)
		{bgfx::RendererType::Metal, "metal"},
#else
		{bgfx::RendererType::Vulkan, "vulkan"},
		{bgfx::RendererType::OpenGL, "opengl"},
#endif
	};

	std::unique_ptr<Renderer3D> renderer(new Renderer3D());
	renderer->impl = std::make_unique<Impl>();
	Impl &impl = *renderer->impl;

	for (const Backend &candidate : BACKENDS) {
		if (!backend.empty() && !StrEqualsIgnoreCase(backend, candidate.name)) continue;
		bgfx::RendererType::Enum type = candidate.type;

		bgfx::Init init;
		init.type = type;
		init.fallback = false;
		/* No window: bgfx creates a device that can only render offscreen. */
		init.swapChain.width = 0;
		init.swapChain.height = 0;
		init.callback = &impl.callback;
		impl.callback.failed = false;

		if (!bgfx::init(init)) {
			Debug(Facility::Driver, Severity::Error, "[viewport3d] backend {} is not available", candidate.name);
			continue;
		}
		if (impl.callback.failed || bgfx::getCaps()->limits.maxTextureLayers < GROUND_TEXTURE_END * 2) {
			bgfx::shutdown();
			continue;
		}
		impl.initialised = true;
		break;
	}

	if (!impl.initialised) {
		error = backend.empty() ? "No usable Vulkan or OpenGL graphics driver was found." : fmt::format("The graphics API '{}' is not available.", backend);
		return nullptr;
	}
	Debug(Facility::Driver, Severity::Error, "[viewport3d] using bgfx backend {}", bgfx::getRendererName(bgfx::getRendererType()));

	impl.layout.begin()
		.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
		.add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
		.add(bgfx::Attrib::TexCoord0, 1, bgfx::AttribType::Float)
		.end();

	impl.program = bgfx::createProgram(CreateShader(SHADER_VARIANTS(vs_terrain)), CreateShader(SHADER_VARIANTS(fs_terrain)), true);

	impl.sprite_layout.begin()
		.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
		.add(bgfx::Attrib::TexCoord0, 4, bgfx::AttribType::Float)
		.add(bgfx::Attrib::TexCoord1, 1, bgfx::AttribType::Float)
		.end();
	impl.sprite_program = bgfx::createProgram(CreateShader(SHADER_VARIANTS(vs_sprite)), CreateShader(SHADER_VARIANTS(fs_sprite)), true);
	impl.sprite_sampler = bgfx::createUniform("s_sprites", bgfx::UniformType::Sampler);
	impl.sprite_axes = bgfx::createUniform("u_spriteAxes", bgfx::UniformType::Vec4);
	impl.sprite_nearer = bgfx::createUniform("u_spriteNearer", bgfx::UniformType::Vec4);
	/* Without smaller versions of the atlas: sprites next to each other would blend into one another. */
	impl.sprite_atlas = bgfx::createTexture2D(SpriteAtlas::WIDTH, SpriteAtlas::HEIGHT, false, 1, bgfx::TextureFormat::BGRA8, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);

	impl.ground_sampler = bgfx::createUniform("s_ground", bgfx::UniformType::Sampler);

	/* Two triangles per quad, split along the diagonal between vertex 1 and 2. */
	static constexpr uint MAX_QUADS = MAX_CHUNK_QUADS;
	const bgfx::Memory *mem = bgfx::alloc(MAX_QUADS * 6 * sizeof(uint16_t));
	uint16_t *index = reinterpret_cast<uint16_t *>(mem->data);
	for (uint i = 0; i < MAX_QUADS; i++) {
		uint16_t base = static_cast<uint16_t>(i * 4);
		for (uint16_t offset : {0, 1, 2, 2, 1, 3}) *index++ = base + offset;
	}
	impl.quad_indices = bgfx::createIndexBuffer(mem);

	Renderer3D::exists = true;

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
	if (bgfx::isValid(impl.sprite_atlas)) bgfx::destroy(impl.sprite_atlas);
	if (bgfx::isValid(impl.sprite_axes)) bgfx::destroy(impl.sprite_axes);
	if (bgfx::isValid(impl.sprite_nearer)) bgfx::destroy(impl.sprite_nearer);
	if (bgfx::isValid(impl.sprite_sampler)) bgfx::destroy(impl.sprite_sampler);
	if (bgfx::isValid(impl.sprite_program)) bgfx::destroy(impl.sprite_program);
	if (bgfx::isValid(impl.ground_textures)) bgfx::destroy(impl.ground_textures);
	if (bgfx::isValid(impl.ground_sampler)) bgfx::destroy(impl.ground_sampler);
	if (bgfx::isValid(impl.program)) bgfx::destroy(impl.program);
	bgfx::shutdown();
	Renderer3D::exists = false;
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

/** The number of ground textures there is room for. */
uint Renderer3D::GetMaxGroundTextures() const
{
	static constexpr uint MAX_GROUND_TEXTURES = 1024;
	return std::min<uint>(MAX_GROUND_TEXTURES, bgfx::getCaps()->limits.maxTextureLayers);
}

/**
 * Send the ground textures that were made since the last call to the GPU.
 * @param textures The ground textures.
 */
void Renderer3D::UpdateGroundTextures(GroundTextures &textures)
{
	Impl &impl = *this->impl;
	const uint texture_size = textures.GetSize();
	assert(HasExactlyOneBit(texture_size));

	if (!bgfx::isValid(impl.ground_textures)) {
		impl.ground_textures = bgfx::createTexture2D(static_cast<uint16_t>(texture_size), static_cast<uint16_t>(texture_size), true, static_cast<uint16_t>(this->GetMaxGroundTextures()),
			bgfx::TextureFormat::BGRA8, BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC);
	}

	std::vector<uint32_t> smaller;
	for (GroundTextures::Pending &pending : textures.TakePending()) {
		std::vector<uint32_t> &level = pending.pixels;
		assert(level.size() == static_cast<size_t>(texture_size) * texture_size);

		uint8_t mip = 0;
		for (uint size = texture_size; ; size /= 2, mip++) {
			bgfx::updateTexture2D(impl.ground_textures, static_cast<uint16_t>(pending.layer), mip, 0, 0, static_cast<uint16_t>(size), static_cast<uint16_t>(size),
				bgfx::copy(level.data(), static_cast<uint32_t>(level.size() * sizeof(uint32_t))));
			if (size == 1) break;

			/* The next level is half the size, each pixel the average of four. */
			uint half = size / 2;
			smaller.resize(static_cast<size_t>(half) * half);
			for (uint y = 0; y < half; y++) {
				for (uint x = 0; x < half; x++) {
					const uint32_t *src = &level[static_cast<size_t>(y) * 2 * size + x * 2];
					uint32_t result = 0;
					for (uint shift = 0; shift < 32; shift += 8) {
						uint sum = ((src[0] >> shift) & 0xFF) + ((src[1] >> shift) & 0xFF) + ((src[size] >> shift) & 0xFF) + ((src[size + 1] >> shift) & 0xFF);
						result |= ((sum + 2) / 4) << shift;
					}
					smaller[static_cast<size_t>(y) * half + x] = result;
				}
			}
			std::swap(level, smaller);
		}
	}
}

/**
 * Send the parts of the sprite atlas that changed to the GPU.
 * @param atlas The atlas.
 */
void Renderer3D::UpdateSpriteAtlas(SpriteAtlas &atlas)
{
	uint top, bottom;
	if (!atlas.TakeDirtyRows(top, bottom)) return;

	std::span<const uint32_t> rows = atlas.GetPixels().subspan(static_cast<size_t>(top) * SpriteAtlas::WIDTH, static_cast<size_t>(bottom - top) * SpriteAtlas::WIDTH);
	bgfx::updateTexture2D(this->impl->sprite_atlas, 0, 0, 0, static_cast<uint16_t>(top), SpriteAtlas::WIDTH, static_cast<uint16_t>(bottom - top),
		bgfx::copy(rows.data(), static_cast<uint32_t>(rows.size_bytes())));
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
	if (bgfx::isValid(chunk.sprites)) bgfx::destroy(chunk.sprites);
	chunk.sprites = BGFX_INVALID_HANDLE;

	chunk.num_quads = static_cast<uint32_t>(mesh.vertices.size() / 4);
	if (chunk.num_quads != 0) {
		chunk.vertices = bgfx::createVertexBuffer(
			bgfx::copy(mesh.vertices.data(), static_cast<uint32_t>(mesh.vertices.size() * sizeof(TerrainVertex))),
			this->impl->layout);
	}

	chunk.num_sprites = static_cast<uint32_t>(mesh.sprites.size() / 4);
	if (chunk.num_sprites != 0) {
		chunk.sprites = bgfx::createVertexBuffer(
			bgfx::copy(mesh.sprites.data(), static_cast<uint32_t>(mesh.sprites.size() * sizeof(SpriteVertex))),
			this->impl->sprite_layout);
	}
}

/** Remove all terrain chunks from the GPU. */
void Renderer3D::ClearChunks()
{
	for (auto &[id, chunk] : this->impl->chunks) {
		if (bgfx::isValid(chunk.vertices)) bgfx::destroy(chunk.vertices);
		if (bgfx::isValid(chunk.sprites)) bgfx::destroy(chunk.sprites);
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
 * @param sprites Sprites to draw that are in no chunk, such as vehicles; four vertices per sprite.
 */
void Renderer3D::Render(uint width, uint height, const Camera3D &camera, std::span<const ChunkID> chunks, std::span<const SpriteVertex> sprites)
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
	model.m[10] = HEIGHT_LEVEL_SIZE;
	model.m[15] = 1;

	Vec3 right = camera.GetRight();
	const float sprite_axes[4] = {right.x * SPRITE_PIXEL_SIZE, right.y * SPRITE_PIXEL_SIZE, right.z * SPRITE_PIXEL_SIZE, WORLD_Z_SCALE};

	/* A point this many pixels of height below a sprite's foot is hidden by ground that is that height divided by
	 * the sine of the camera's pitch nearer to the camera. Limited, or sprites would show through hills when looking along the ground. */
	Vec3 eye = camera.GetEye();
	float eye_distance = std::max(1.0f, std::sqrt((eye.x - camera.target.x) * (eye.x - camera.target.x) + (eye.y - camera.target.y) * (eye.y - camera.target.y) + (eye.z - camera.target.z) * (eye.z - camera.target.z)));
	const float sprite_nearer[4] = {(eye.x - camera.target.x) / eye_distance, (eye.y - camera.target.y) / eye_distance, (eye.z - camera.target.z) / eye_distance,
		WORLD_Z_SCALE / std::max(0.35f, std::sin(camera.pitch))};

	for (ChunkID id : chunks) {
		auto it = impl.chunks.find(id);
		if (it == impl.chunks.end()) continue;
		const GpuChunk &chunk = it->second;

		if (bgfx::isValid(chunk.vertices)) {
			bgfx::setTransform(model.m.data());
			bgfx::setVertexBuffer(0, chunk.vertices);
			bgfx::setTexture(0, impl.ground_sampler, impl.ground_textures);
			bgfx::setIndexBuffer(impl.quad_indices, 0, chunk.num_quads * 6);
			bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
			bgfx::submit(VIEW_SCENE, impl.program);
		}

		if (bgfx::isValid(chunk.sprites)) {
			bgfx::setTransform(model.m.data());
			bgfx::setVertexBuffer(0, chunk.sprites);
			bgfx::setTexture(0, impl.sprite_sampler, impl.sprite_atlas);
			bgfx::setUniform(impl.sprite_axes, sprite_axes);
			bgfx::setUniform(impl.sprite_nearer, sprite_nearer);
			bgfx::setIndexBuffer(impl.quad_indices, 0, chunk.num_sprites * 6);
			bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
			bgfx::submit(VIEW_SCENE, impl.sprite_program);
		}
	}

	uint32_t num_sprites = static_cast<uint32_t>(std::min<size_t>(sprites.size() / 4, MAX_CHUNK_QUADS));
	if (num_sprites != 0 && bgfx::getAvailTransientVertexBuffer(num_sprites * 4, impl.sprite_layout) >= num_sprites * 4) {
		/* These change all the time, so they go in a buffer that only lives for this image. */
		bgfx::TransientVertexBuffer buffer;
		bgfx::allocTransientVertexBuffer(&buffer, num_sprites * 4, impl.sprite_layout);
		std::copy_n(reinterpret_cast<const uint8_t *>(sprites.data()), num_sprites * 4 * sizeof(SpriteVertex), buffer.data);

		bgfx::setTransform(model.m.data());
		bgfx::setVertexBuffer(0, &buffer);
		bgfx::setTexture(0, impl.sprite_sampler, impl.sprite_atlas);
		bgfx::setUniform(impl.sprite_axes, sprite_axes);
		bgfx::setUniform(impl.sprite_nearer, sprite_nearer);
		bgfx::setIndexBuffer(impl.quad_indices, 0, num_sprites * 6);
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
		bgfx::submit(VIEW_SCENE, impl.sprite_program);
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
