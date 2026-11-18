/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file renderer3d.cpp GPU side of the 3D viewport, implemented with bgfx. */

#include "../stdafx.h"
#include "renderer3d.h"
#include "lighting3d.h"
#include "model_pack.h"
#include "../core/math_func.hpp"
#include "../debug.h"
#include "../string_func.h"
#include "../tile_type.h"

#include <bgfx/bgfx.h>
#include <bx/math.h>

#include <chrono>

#include "shaders/vs_terrain.bin.h"
#include "shaders/fs_terrain.bin.h"
#include "shaders/vs_sprite.bin.h"
#include "shaders/fs_sprite.bin.h"
#include "shaders/vs_model.bin.h"
#include "shaders/fs_model.bin.h"
#include "shaders/vs_shadow.bin.h"
#include "shaders/fs_shadow.bin.h"
#include "shaders/fs_sprite_shadow.bin.h"
#include "shaders/vs_model_instanced.bin.h"
#include "shaders/vs_shadow_instanced.bin.h"
#include "shaders/vs_sky.bin.h"
#include "shaders/fs_sky.bin.h"
#include "shaders/vs_weather.bin.h"
#include "shaders/fs_weather.bin.h"

#include "../safeguards.h"

/** Texels along each side of the shadow map. */
static constexpr uint16_t SHADOW_MAP_SIZE = 2048;

/** Number of drops of rain or flakes of snow in the cube that is repeated all over the world. */
static constexpr uint NUM_DROPS = 16000;

/** A value between two others. */
static inline float Lerp(float a, float b, float t)
{
	return a + (b - a) * t;
}

/** One corner of a drop of rain or a flake of snow. */
struct DropVertex {
	float x, y, z; ///< Place of the drop in the cube, each from 0 to 1.
	float u, v; ///< Which corner, each -1 or 1.
	float r1, r2; ///< Two numbers from 0 to 1 that differ per drop.
};

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
	uint32_t num_water = 0; ///< Number of quads of see-through water, which follow the terrain in #vertices.
	bgfx::VertexBufferHandle sprites = BGFX_INVALID_HANDLE; ///< The sprites standing on the terrain.
	uint32_t num_sprites = 0; ///< Number of sprites.
	std::vector<ModelInstance> models; ///< 3D models standing on the terrain.
};

/** GPU buffers of one 3D model. */
struct GpuModel {
	bgfx::VertexBufferHandle vertices = BGFX_INVALID_HANDLE; ///< The corners.
	bgfx::IndexBufferHandle indices = BGFX_INVALID_HANDLE; ///< The triangles.
	uint32_t num_indices = 0; ///< Number of indices.
	uint32_t num_fixed_indices = 0; ///< Number of indices before the triangles of the load.
	float load_depth = 0; ///< How far the load can sink into the vehicle.
	bgfx::VertexBufferHandle pool_vertices = BGFX_INVALID_HANDLE; ///< The light that the lamps of the model cast on the ground at night.
	bgfx::IndexBufferHandle pool_indices = BGFX_INVALID_HANDLE; ///< The triangles of that.
	uint32_t num_pool_indices = 0; ///< Number of indices of that.
};

struct Renderer3D::Impl {
	bgfx::VertexLayout model_layout;
	bgfx::ProgramHandle model_program = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle model_tint = BGFX_INVALID_HANDLE;
	Lighting3D lighting; ///< The light of the moment.
	bgfx::UniformHandle light_ambient = BGFX_INVALID_HANDLE; ///< Light from all around, and how much lamps glow.
	bgfx::UniformHandle light_direct = BGFX_INVALID_HANDLE; ///< Light from the sun or the moon, and the share of windows that is lit.
	bgfx::UniformHandle light_direction = BGFX_INVALID_HANDLE; ///< Direction towards the sun or the moon.
	bgfx::UniformHandle sky_sun = BGFX_INVALID_HANDLE; ///< Where the sun is in the sky.
	bgfx::UniformHandle sky_moon = BGFX_INVALID_HANDLE; ///< Where the moon is in the sky.
	std::vector<GpuModel> models;
	bgfx::UniformHandle model_sampler = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle model_textures = BGFX_INVALID_HANDLE; ///< Texture array with the textures of the models; ModelVertex::texture is the layer.
	uint num_model_textures = 0; ///< Number of layers of #model_textures.
	bgfx::ProgramHandle shadow_program = BGFX_INVALID_HANDLE; ///< Draws depth into the shadow map.
	bgfx::ProgramHandle sprite_shadow_program = BGFX_INVALID_HANDLE; ///< Draws sprites, turned towards the sun, into the shadow map.
	bgfx::ProgramHandle model_instanced_program = BGFX_INVALID_HANDLE; ///< Draws many copies of one model at once.
	bgfx::ProgramHandle shadow_instanced_program = BGFX_INVALID_HANDLE; ///< Draws many copies of one model into the shadow map.
	std::vector<uint> far; ///< For each model, what to draw when it is far away; see ModelPack::GetFarModel().
	std::vector<std::vector<std::array<float, 20>>> batches; ///< For each model, the copies to draw this frame: model matrix and tint.
	bgfx::FrameBufferHandle shadow_map = BGFX_INVALID_HANDLE; ///< Depth of what casts shadows, seen from the sun.
	bgfx::UniformHandle shadow_sampler = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle shadow_matrix = BGFX_INVALID_HANDLE;
	bgfx::UniformHandle shadow_params = BGFX_INVALID_HANDLE;
	bool shadows = false; ///< Whether the GPU can compare depths when sampling, which shadows need.
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
	bgfx::UniformHandle mix_sampler = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle ground_mix = BGFX_INVALID_HANDLE; ///< For each layer of #ground_textures, how much of the textures of the model package to mix in; see #GroundMix.
	bgfx::UniformHandle layers = BGFX_INVALID_HANDLE; ///< Which layers are mixed, and the layers of road and track.
	bgfx::UniformHandle terrain_sampler = BGFX_INVALID_HANDLE;
	bgfx::TextureHandle terrain_textures = BGFX_INVALID_HANDLE; ///< Texture array with the ground textures of the model package, if it has them.
	bgfx::UniformHandle terrain = BGFX_INVALID_HANDLE; ///< How the ground textures of the model package are used.
	float terrain_tiles = 0; ///< Along how many tiles each of those is spread; 0 when there are none.
	bgfx::UniformHandle haze_colour = BGFX_INVALID_HANDLE; ///< Colour of the haze in the distance, and of the sky at the horizon.
	bgfx::UniformHandle haze_params = BGFX_INVALID_HANDLE; ///< Where the haze starts and how thick it gets.
	bgfx::UniformHandle water = BGFX_INVALID_HANDLE; ///< Which ground textures are water, and the time for the waves.
	bgfx::UniformHandle sky_matrix = BGFX_INVALID_HANDLE; ///< From the screen back to the world, for the sky.
	bgfx::UniformHandle sky_colour = BGFX_INVALID_HANDLE; ///< Colour of the sky high up.
	bgfx::ProgramHandle sky_program = BGFX_INVALID_HANDLE; ///< Draws the sky behind everything.
	bgfx::VertexLayout sky_layout;
	bgfx::VertexBufferHandle sky_vertices = BGFX_INVALID_HANDLE; ///< One triangle that covers the screen.
	std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now(); ///< When the renderer started; things move with the time since.
	Weather3D weather; ///< The weather and season to draw.
	bgfx::UniformHandle weather_params = BGFX_INVALID_HANDLE; ///< Snow on the ground, wet ground, clouds and lightning.
	bgfx::UniformHandle season_params = BGFX_INVALID_HANDLE; ///< How far the year is, for leaves and grass.
	bgfx::ProgramHandle drop_program = BGFX_INVALID_HANDLE; ///< Draws rain and snow.
	bgfx::VertexLayout drop_layout;
	bgfx::VertexBufferHandle drop_vertices = BGFX_INVALID_HANDLE; ///< The drops in their cube.
	bgfx::UniformHandle drop_box = BGFX_INVALID_HANDLE; ///< Where the cube of drops is.
	bgfx::UniformHandle drop_move = BGFX_INVALID_HANDLE; ///< How far the drops have fallen.
	bgfx::UniformHandle drop_shape = BGFX_INVALID_HANDLE; ///< Size of the drops, and whether they are snow.
	bgfx::UniformHandle drop_fall = BGFX_INVALID_HANDLE; ///< Which way the drops fall.
	bgfx::UniformHandle drop_look = BGFX_INVALID_HANDLE; ///< Which way the camera looks.
	bgfx::UniformHandle drop_colour = BGFX_INVALID_HANDLE; ///< Colour of the drops.
	bgfx::IndexBufferHandle quad_indices = BGFX_INVALID_HANDLE; ///< Indices for a full chunk of quads, shared by all chunks.
	/** Where the images of one view are drawn and read back. */
	struct Target {
		bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE; ///< Colour and depth to draw into.
		bgfx::TextureHandle readback = BGFX_INVALID_HANDLE; ///< Copy of the colour the CPU can read.
		uint width = 0; ///< Width of both in pixels.
		uint height = 0; ///< Height of both in pixels.

		/** Free the GPU memory. */
		void Destroy()
		{
			if (bgfx::isValid(this->framebuffer)) bgfx::destroy(this->framebuffer);
			if (bgfx::isValid(this->readback)) bgfx::destroy(this->readback);
			this->framebuffer = BGFX_INVALID_HANDLE;
			this->readback = BGFX_INVALID_HANDLE;
		}
	};
	std::unordered_map<uint, Target> targets; ///< Render targets by the id of their view.
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
		.add(bgfx::Attrib::TexCoord0, 3, bgfx::AttribType::Float)
		.end();

	impl.program = bgfx::createProgram(CreateShader(SHADER_VARIANTS(vs_terrain)), CreateShader(SHADER_VARIANTS(fs_terrain)), true);

	impl.sprite_layout.begin()
		.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
		.add(bgfx::Attrib::TexCoord0, 4, bgfx::AttribType::Float)
		.add(bgfx::Attrib::TexCoord1, 1, bgfx::AttribType::Float)
		.end();
	impl.sprite_program = bgfx::createProgram(CreateShader(SHADER_VARIANTS(vs_sprite)), CreateShader(SHADER_VARIANTS(fs_sprite)), true);
	impl.sprite_sampler = bgfx::createUniform("s_sprites", bgfx::UniformType::Sampler);

	impl.model_layout.begin()
		.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
		.add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
		.add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
		.add(bgfx::Attrib::TexCoord0, 3, bgfx::AttribType::Float)
		.end();
	impl.model_sampler = bgfx::createUniform("s_modelTextures", bgfx::UniformType::Sampler);
	/* Until the model package brings textures: one white pixel, as the shader always looks at a texture. */
	{
		std::vector<uint32_t> white{0xFFFFFFFF};
		renderer->SetModelTextures(1, {&white, 1});
	}
	impl.model_program = bgfx::createProgram(CreateShader(SHADER_VARIANTS(vs_model)), CreateShader(SHADER_VARIANTS(fs_model)), true);
	impl.model_tint = bgfx::createUniform("u_modelTint", bgfx::UniformType::Vec4);
	impl.light_ambient = bgfx::createUniform("u_lightAmbient", bgfx::UniformType::Vec4);
	impl.light_direct = bgfx::createUniform("u_lightDirect", bgfx::UniformType::Vec4);
	impl.light_direction = bgfx::createUniform("u_lightDirection", bgfx::UniformType::Vec4);
	impl.sky_sun = bgfx::createUniform("u_skySun", bgfx::UniformType::Vec4);
	impl.sky_moon = bgfx::createUniform("u_skyMoon", bgfx::UniformType::Vec4);
	impl.sprite_axes = bgfx::createUniform("u_spriteAxes", bgfx::UniformType::Vec4);
	impl.sprite_nearer = bgfx::createUniform("u_spriteNearer", bgfx::UniformType::Vec4);
	/* Without smaller versions of the atlas: sprites next to each other would blend into one another. */
	impl.sprite_atlas = bgfx::createTexture2D(SpriteAtlas::WIDTH, SpriteAtlas::HEIGHT, false, 1, bgfx::TextureFormat::BGRA8, BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);

	impl.ground_sampler = bgfx::createUniform("s_ground", bgfx::UniformType::Sampler);
	impl.haze_colour = bgfx::createUniform("u_hazeColour", bgfx::UniformType::Vec4);
	impl.haze_params = bgfx::createUniform("u_hazeParams", bgfx::UniformType::Vec4);
	impl.water = bgfx::createUniform("u_water", bgfx::UniformType::Vec4);
	impl.terrain_sampler = bgfx::createUniform("s_terrain", bgfx::UniformType::Sampler);
	impl.mix_sampler = bgfx::createUniform("s_mix", bgfx::UniformType::Sampler);
	impl.layers = bgfx::createUniform("u_layers", bgfx::UniformType::Vec4);
	impl.terrain = bgfx::createUniform("u_terrain", bgfx::UniformType::Vec4);
	impl.sky_matrix = bgfx::createUniform("u_skyInvViewProj", bgfx::UniformType::Mat4);
	impl.sky_colour = bgfx::createUniform("u_skyColour", bgfx::UniformType::Vec4);
	impl.sky_program = bgfx::createProgram(CreateShader(SHADER_VARIANTS(vs_sky)), CreateShader(SHADER_VARIANTS(fs_sky)), true);
	impl.sky_layout.begin().add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float).end();
	static const float SKY_TRIANGLE[] = {-1, -1, 3, -1, -1, 3};
	impl.sky_vertices = bgfx::createVertexBuffer(bgfx::makeRef(SKY_TRIANGLE, sizeof(SKY_TRIANGLE)), impl.sky_layout);

	/* Rain and snow: drops at made-up places in a cube, four corners each. */
	impl.weather_params = bgfx::createUniform("u_weather", bgfx::UniformType::Vec4);
	impl.season_params = bgfx::createUniform("u_season", bgfx::UniformType::Vec4);
	impl.drop_box = bgfx::createUniform("u_dropBox", bgfx::UniformType::Vec4);
	impl.drop_move = bgfx::createUniform("u_dropMove", bgfx::UniformType::Vec4);
	impl.drop_shape = bgfx::createUniform("u_dropShape", bgfx::UniformType::Vec4);
	impl.drop_fall = bgfx::createUniform("u_dropFall", bgfx::UniformType::Vec4);
	impl.drop_look = bgfx::createUniform("u_dropLook", bgfx::UniformType::Vec4);
	impl.drop_colour = bgfx::createUniform("u_dropColour", bgfx::UniformType::Vec4);
	impl.drop_program = bgfx::createProgram(CreateShader(SHADER_VARIANTS(vs_weather)), CreateShader(SHADER_VARIANTS(fs_weather)), true);
	impl.drop_layout.begin()
		.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
		.add(bgfx::Attrib::TexCoord0, 4, bgfx::AttribType::Float)
		.end();
	{
		const bgfx::Memory *drops = bgfx::alloc(NUM_DROPS * 4 * sizeof(DropVertex));
		DropVertex *drop = reinterpret_cast<DropVertex *>(drops->data);
		/* The same made-up places every time; they only have to look scattered. */
		uint32_t state = 0x2F6E2B1;
		auto random = [&state]() {
			state = state * 1664525 + 1013904223;
			return static_cast<float>(state >> 8) / static_cast<float>(1 << 24);
		};
		for (uint i = 0; i < NUM_DROPS; i++) {
			float x = random(), y = random(), z = random(), r1 = random(), r2 = random();
			/* In the order of the shared quad indices. */
			for (auto [u, v] : {std::pair{-1.0f, -1.0f}, std::pair{1.0f, -1.0f}, std::pair{-1.0f, 1.0f}, std::pair{1.0f, 1.0f}}) *drop++ = {x, y, z, u, v, r1, r2};
		}
		impl.drop_vertices = bgfx::createVertexBuffer(drops, impl.drop_layout);
	}

	/* The shadow map; without a depth format that can be compared when sampling, there are no shadows, but the shaders still need a map. */
	impl.shadow_program = bgfx::createProgram(CreateShader(SHADER_VARIANTS(vs_shadow)), CreateShader(SHADER_VARIANTS(fs_shadow)), true);
	impl.sprite_shadow_program = bgfx::createProgram(CreateShader(SHADER_VARIANTS(vs_sprite)), CreateShader(SHADER_VARIANTS(fs_sprite_shadow)), true);
	if (bgfx::getCaps()->limits.maxInstanceData >= 5) {
		impl.model_instanced_program = bgfx::createProgram(CreateShader(SHADER_VARIANTS(vs_model_instanced)), CreateShader(SHADER_VARIANTS(fs_model)), true);
		impl.shadow_instanced_program = bgfx::createProgram(CreateShader(SHADER_VARIANTS(vs_shadow_instanced)), CreateShader(SHADER_VARIANTS(fs_shadow)), true);
	}
	impl.shadow_sampler = bgfx::createUniform("s_shadow", bgfx::UniformType::Sampler);
	impl.shadow_matrix = bgfx::createUniform("u_shadowMtx", bgfx::UniformType::Mat4);
	impl.shadow_params = bgfx::createUniform("u_shadowParams", bgfx::UniformType::Vec4);
	bgfx::TextureFormat::Enum shadow_format = bgfx::TextureFormat::D16;
	for (bgfx::TextureFormat::Enum candidate : {bgfx::TextureFormat::D32F, bgfx::TextureFormat::D24, bgfx::TextureFormat::D16}) {
		if (bgfx::isTextureValid(0, false, 1, candidate, BGFX_TEXTURE_RT | BGFX_SAMPLER_COMPARE_LEQUAL)) {
			shadow_format = candidate;
			impl.shadows = true;
			break;
		}
	}
	bgfx::TextureHandle shadow_texture = bgfx::createTexture2D(SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, false, 1, shadow_format, BGFX_TEXTURE_RT | BGFX_SAMPLER_COMPARE_LEQUAL | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP);
	impl.shadow_map = bgfx::createFrameBuffer(1, &shadow_texture, true);

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
	for (GpuModel &model : impl.models) {
		if (bgfx::isValid(model.vertices)) bgfx::destroy(model.vertices);
		if (bgfx::isValid(model.indices)) bgfx::destroy(model.indices);
		if (bgfx::isValid(model.pool_vertices)) bgfx::destroy(model.pool_vertices);
		if (bgfx::isValid(model.pool_indices)) bgfx::destroy(model.pool_indices);
	}
	if (bgfx::isValid(impl.model_program)) bgfx::destroy(impl.model_program);
	if (bgfx::isValid(impl.model_sampler)) bgfx::destroy(impl.model_sampler);
	if (bgfx::isValid(impl.model_textures)) bgfx::destroy(impl.model_textures);
	if (bgfx::isValid(impl.model_tint)) bgfx::destroy(impl.model_tint);
	for (bgfx::UniformHandle handle : {impl.light_ambient, impl.light_direct, impl.light_direction, impl.sky_sun, impl.sky_moon}) {
		if (bgfx::isValid(handle)) bgfx::destroy(handle);
	}
	if (bgfx::isValid(impl.shadow_program)) bgfx::destroy(impl.shadow_program);
	if (bgfx::isValid(impl.sprite_shadow_program)) bgfx::destroy(impl.sprite_shadow_program);
	if (bgfx::isValid(impl.model_instanced_program)) bgfx::destroy(impl.model_instanced_program);
	if (bgfx::isValid(impl.shadow_instanced_program)) bgfx::destroy(impl.shadow_instanced_program);
	if (bgfx::isValid(impl.shadow_map)) bgfx::destroy(impl.shadow_map);
	if (bgfx::isValid(impl.shadow_sampler)) bgfx::destroy(impl.shadow_sampler);
	if (bgfx::isValid(impl.shadow_matrix)) bgfx::destroy(impl.shadow_matrix);
	if (bgfx::isValid(impl.shadow_params)) bgfx::destroy(impl.shadow_params);
	for (auto &[id, target] : impl.targets) target.Destroy();
	if (bgfx::isValid(impl.quad_indices)) bgfx::destroy(impl.quad_indices);
	if (bgfx::isValid(impl.sprite_atlas)) bgfx::destroy(impl.sprite_atlas);
	if (bgfx::isValid(impl.sprite_axes)) bgfx::destroy(impl.sprite_axes);
	if (bgfx::isValid(impl.sprite_nearer)) bgfx::destroy(impl.sprite_nearer);
	if (bgfx::isValid(impl.sprite_sampler)) bgfx::destroy(impl.sprite_sampler);
	if (bgfx::isValid(impl.sprite_program)) bgfx::destroy(impl.sprite_program);
	if (bgfx::isValid(impl.ground_textures)) bgfx::destroy(impl.ground_textures);
	if (bgfx::isValid(impl.terrain_textures)) bgfx::destroy(impl.terrain_textures);
	if (bgfx::isValid(impl.ground_mix)) bgfx::destroy(impl.ground_mix);
	for (bgfx::UniformHandle handle : {impl.haze_colour, impl.haze_params, impl.water, impl.sky_matrix, impl.sky_colour, impl.terrain, impl.terrain_sampler, impl.mix_sampler, impl.layers}) {
		if (bgfx::isValid(handle)) bgfx::destroy(handle);
	}
	for (bgfx::UniformHandle handle : {impl.weather_params, impl.season_params, impl.drop_box, impl.drop_move, impl.drop_shape, impl.drop_fall, impl.drop_look, impl.drop_colour}) {
		if (bgfx::isValid(handle)) bgfx::destroy(handle);
	}
	if (bgfx::isValid(impl.drop_program)) bgfx::destroy(impl.drop_program);
	if (bgfx::isValid(impl.drop_vertices)) bgfx::destroy(impl.drop_vertices);
	if (bgfx::isValid(impl.sky_program)) bgfx::destroy(impl.sky_program);
	if (bgfx::isValid(impl.sky_vertices)) bgfx::destroy(impl.sky_vertices);
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
 * Send one layer of a texture array to the GPU, with all its smaller versions.
 * @param texture The texture array.
 * @param texture_size Width and height of the layer.
 * @param layer Which layer.
 * @param level The pixels; changed while making the smaller versions.
 */
/** Bytes of texture data sent to the GPU since the last frame of bgfx. */
static size_t _uploaded_bytes = 0;

/**
 * Make room for texture data to send to the GPU.
 * bgfx stages all texture updates of a frame in one buffer of 32 MiB (with Vulkan); what does not fit gets a staging buffer
 * of its own, whose memory bgfx hands out again before the GPU has copied from it. The AMD Vulkan driver on Windows crashes
 * on that while loading the textures of the model package. So when a frame has had a fair amount, it is finished first.
 * @param bytes How much is about to be sent.
 */
static void BeforeUpload(size_t bytes)
{
	static constexpr size_t BUDGET = 12 << 20;
	if (_uploaded_bytes != 0 && _uploaded_bytes + bytes > BUDGET) {
		bgfx::frame();
		_uploaded_bytes = 0;
	}
	_uploaded_bytes += bytes;
}

static void UploadTextureLayer(bgfx::TextureHandle texture, uint texture_size, uint layer, std::vector<uint32_t> &level)
{
	std::vector<uint32_t> smaller;
	uint8_t mip = 0;
	for (uint size = texture_size; ; size /= 2, mip++) {
		BeforeUpload(level.size() * sizeof(uint32_t));
		bgfx::updateTexture2D(texture, static_cast<uint16_t>(layer), mip, 0, 0, static_cast<uint16_t>(size), static_cast<uint16_t>(size),
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

	for (GroundTextures::Pending &pending : textures.TakePending()) {
		assert(pending.pixels.size() == static_cast<size_t>(texture_size) * texture_size);
		UploadTextureLayer(impl.ground_textures, texture_size, pending.layer, pending.pixels);
		if (pending.mix.empty()) continue;
		if (!bgfx::isValid(impl.ground_mix)) {
			impl.ground_mix = bgfx::createTexture2D(static_cast<uint16_t>(texture_size), static_cast<uint16_t>(texture_size), true, static_cast<uint16_t>(this->GetMaxGroundTextures()),
				bgfx::TextureFormat::BGRA8, BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC);
		}
		UploadTextureLayer(impl.ground_mix, texture_size, pending.layer, pending.mix);
	}
}

/**
 * Use the ground textures of the model package for the landscape.
 * @param size Width and height of each texture; a power of two.
 * @param tiles Along how many tiles each texture is spread.
 * @param textures The textures, for the layers below #GROUND_TEXTURE_TERRAIN_END.
 * @return Whether the GPU can use them.
 */
bool Renderer3D::SetTerrainTextures(uint size, uint tiles, std::vector<GroundTextures::Pending> &&textures)
{
	Impl &impl = *this->impl;
	assert(HasExactlyOneBit(size));
	if (bgfx::getCaps()->limits.maxTextureLayers < GROUND_TEXTURE_TERRAIN_END) return false;

	if (bgfx::isValid(impl.terrain_textures)) bgfx::destroy(impl.terrain_textures);
	impl.terrain_textures = bgfx::createTexture2D(static_cast<uint16_t>(size), static_cast<uint16_t>(size), true, GROUND_TEXTURE_TERRAIN_END,
		bgfx::TextureFormat::BGRA8, BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC);
	for (GroundTextures::Pending &pending : textures) {
		assert(pending.layer < GROUND_TEXTURE_TERRAIN_END && pending.pixels.size() == static_cast<size_t>(size) * size);
		UploadTextureLayer(impl.terrain_textures, size, pending.layer, pending.pixels);
	}
	impl.terrain_tiles = static_cast<float>(tiles);
	return true;
}

/**
 * Send the parts of the sprite atlas that changed to the GPU.
 * @param atlas The atlas.
 */
void Renderer3D::UpdateSpriteAtlas(SpriteAtlas &atlas)
{
	uint top, bottom;
	if (!atlas.TakeDirtyRows(top, bottom)) return;

	/* In bands of rows, so that no single update is too big for the staging buffer of a frame; see BeforeUpload(). */
	static constexpr uint BAND = 256;
	for (uint first = top; first < bottom; first += BAND) {
		uint last = std::min(bottom, first + BAND);
		std::span<const uint32_t> rows = atlas.GetPixels().subspan(static_cast<size_t>(first) * SpriteAtlas::WIDTH, static_cast<size_t>(last - first) * SpriteAtlas::WIDTH);
		BeforeUpload(rows.size_bytes());
		bgfx::updateTexture2D(this->impl->sprite_atlas, 0, 0, 0, static_cast<uint16_t>(first), SpriteAtlas::WIDTH, static_cast<uint16_t>(last - first),
			bgfx::copy(rows.data(), static_cast<uint32_t>(rows.size_bytes())));
	}
}

/**
 * Set the textures of the 3D models.
 * @param size Width and height of each texture; a power of two.
 * @param textures The textures; ModelVertex::texture is an index into these. Those that the GPU has no room for are left white.
 */
void Renderer3D::SetModelTextures(uint size, std::span<const std::vector<uint32_t>> textures)
{
	Impl &impl = *this->impl;
	assert(HasExactlyOneBit(size) && !textures.empty());

	if (bgfx::isValid(impl.model_textures)) bgfx::destroy(impl.model_textures);
	const uint layers = std::min<uint>(static_cast<uint>(textures.size()), bgfx::getCaps()->limits.maxTextureLayers);
	impl.model_textures = bgfx::createTexture2D(static_cast<uint16_t>(size), static_cast<uint16_t>(size), true, static_cast<uint16_t>(layers),
		bgfx::TextureFormat::BGRA8, BGFX_SAMPLER_MIN_ANISOTROPIC | BGFX_SAMPLER_MAG_ANISOTROPIC);
	impl.num_model_textures = layers;
	for (uint layer = 0; layer < layers; layer++) {
		assert(textures[layer].size() == static_cast<size_t>(size) * size);
		std::vector<uint32_t> pixels = textures[layer];
		UploadTextureLayer(impl.model_textures, size, layer, pixels);
	}
}

/**
 * Upload or replace a 3D model.
 * @param id Number of the model, chosen by the caller; ModelInstance::model refers to it.
 * @param model The model.
 */
/**
 * Tell what to draw in place of a model when it is far from the camera.
 * @param id Number of the model.
 * @param far Number of a simpler model, ModelPack::FAR_SAME to draw the model itself, or ModelPack::FAR_HIDDEN to leave it out.
 */
void Renderer3D::SetFarModel(uint id, uint far)
{
	Impl &impl = *this->impl;
	if (id >= impl.far.size()) impl.far.resize(id + 1, ModelPack::FAR_SAME);
	impl.far[id] = far;
}

/**
 * Make the light that the lamps of a model cast on the ground at night: flat patches just above the ground the model
 * stands on, bright in the middle and black at the rim, to be added to what is below them.
 * A lamp high up lights a circle below it. A lamp low down is a headlight and lights a stretch in front of the vehicle;
 * lamps at its back are tail lights and light nothing. Lamps on the ground, like those along runways, light a small circle.
 * @param model The model.
 * @return The patches; empty when the model has no lamps.
 */
static Model3D MakeLightPools(const Model3D &model)
{
	static constexpr int SIDES = 14;
	static constexpr float HEIGHT = 0.14f;
	/* Brightness at the middle, half way and at the rim. */
	static constexpr float RINGS[3] = {1.0f, 0.42f, 0.0f};

	Model3D pools;
	bool headlights_in_front = std::ranges::any_of(model.lamps, [](const Vec3 &lamp) { return lamp.z >= 0.6f && lamp.z <= 3.0f && lamp.x > 0; });
	for (const Vec3 &lamp : model.lamps) {
		float cx = lamp.x, cy = lamp.y, along = 0, across = 0, bright = 1.0f;
		/* Unit vector along the patch. */
		float dx = 1, dy = 0;
		if (lamp.z > 3.0f) {
			along = across = std::clamp(lamp.z * 1.3f, 4.5f, 9.0f);
			bright = 0.85f;
		} else if (lamp.z < 0.6f) {
			along = across = 2.2f;
			bright = 0.6f;
		} else {
			if (headlights_in_front && lamp.x <= 0) continue;
			if (std::abs(lamp.x) < std::abs(lamp.y)) {
				dx = 0;
				dy = lamp.y < 0 ? -1.0f : 1.0f;
			} else {
				dx = lamp.x < 0 ? -1.0f : 1.0f;
			}
			along = 6.0f;
			across = 2.4f;
			cx += dx * (along - 0.5f);
			cy += dy * (along - 0.5f);
			bright = 0.7f;
		}

		uint32_t first = static_cast<uint32_t>(pools.vertices.size());
		auto add = [&](float x, float y, float level) {
			uint8_t r = static_cast<uint8_t>(255 * level * bright), g = static_cast<uint8_t>(214 * level * bright), b = static_cast<uint8_t>(140 * level * bright);
			pools.vertices.push_back({x, y, HEIGHT, 0, 0, 1, r, g, b, ModelVertex::LAMP, 0, 0, -1});
		};
		add(cx, cy, RINGS[0]);
		for (int ring = 1; ring <= 2; ring++) {
			for (int i = 0; i < SIDES; i++) {
				float angle = 6.2831853f * i / SIDES;
				float u = std::cos(angle) * along * ring * 0.5f, v = std::sin(angle) * across * ring * 0.5f;
				add(cx + dx * u - dy * v, cy + dy * u + dx * v, RINGS[ring]);
			}
		}
		for (uint32_t i = 0; i < SIDES; i++) {
			uint32_t next = (i + 1) % SIDES;
			pools.indices.insert(pools.indices.end(), {first, first + 1 + i, first + 1 + next});
			pools.indices.insert(pools.indices.end(), {first + 1 + i, first + 1 + SIDES + i, first + 1 + SIDES + next, first + 1 + i, first + 1 + SIDES + next, first + 1 + next});
		}
	}
	return pools;
}

/**
 * Set the light to draw the next images with.
 * @param lighting The light.
 */
void Renderer3D::SetLighting(const Lighting3D &lighting)
{
	this->impl->lighting = lighting;
}

void Renderer3D::SetModel(uint id, const Model3D &model)
{
	Impl &impl = *this->impl;
	if (id >= impl.models.size()) impl.models.resize(id + 1);
	GpuModel &gpu = impl.models[id];
	if (bgfx::isValid(gpu.vertices)) bgfx::destroy(gpu.vertices);
	if (bgfx::isValid(gpu.indices)) bgfx::destroy(gpu.indices);
	if (bgfx::isValid(gpu.pool_vertices)) bgfx::destroy(gpu.pool_vertices);
	if (bgfx::isValid(gpu.pool_indices)) bgfx::destroy(gpu.pool_indices);
	gpu = {};
	if (model.vertices.empty() || model.indices.empty()) return;

	Model3D pools = MakeLightPools(model);
	if (!pools.indices.empty()) {
		gpu.pool_vertices = bgfx::createVertexBuffer(bgfx::copy(pools.vertices.data(), static_cast<uint32_t>(pools.vertices.size() * sizeof(ModelVertex))), impl.model_layout);
		gpu.pool_indices = bgfx::createIndexBuffer(bgfx::copy(pools.indices.data(), static_cast<uint32_t>(pools.indices.size() * sizeof(uint32_t))), BGFX_BUFFER_INDEX32);
		gpu.num_pool_indices = static_cast<uint32_t>(pools.indices.size());
	}

	gpu.vertices = bgfx::createVertexBuffer(bgfx::copy(model.vertices.data(), static_cast<uint32_t>(model.vertices.size() * sizeof(ModelVertex))), impl.model_layout);
	gpu.indices = bgfx::createIndexBuffer(bgfx::copy(model.indices.data(), static_cast<uint32_t>(model.indices.size() * sizeof(uint32_t))), BGFX_BUFFER_INDEX32);
	gpu.num_indices = static_cast<uint32_t>(model.indices.size());
	gpu.num_fixed_indices = static_cast<uint32_t>(model.num_fixed_indices);
	gpu.load_depth = model.load_depth;
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
	chunk.num_water = static_cast<uint32_t>(mesh.water.size() / 4);
	if (chunk.num_quads + chunk.num_water != 0) {
		/* One buffer, with the water after the terrain. */
		const bgfx::Memory *memory = bgfx::alloc(static_cast<uint32_t>((mesh.vertices.size() + mesh.water.size()) * sizeof(TerrainVertex)));
		std::copy_n(reinterpret_cast<const uint8_t *>(mesh.vertices.data()), mesh.vertices.size() * sizeof(TerrainVertex), memory->data);
		std::copy_n(reinterpret_cast<const uint8_t *>(mesh.water.data()), mesh.water.size() * sizeof(TerrainVertex), memory->data + mesh.vertices.size() * sizeof(TerrainVertex));
		chunk.vertices = bgfx::createVertexBuffer(memory, this->impl->layout);
	}

	chunk.models = mesh.models;

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
 * Set the weather and season of the images that are made from now on.
 * @param weather The weather.
 */
void Renderer3D::SetWeather(const Weather3D &weather)
{
	this->impl->weather = weather;
}

/** How the sun looks at the map, for the shadow map. */
struct ShadowView {
	Matrix4 view; ///< World to the sun's view.
	Matrix4 proj; ///< The sun's view to clip space.
	Matrix4 sample; ///< World to coordinates in the shadow map.
	float bias; ///< How much nearer a point must be than what the map holds, to not shadow itself.
	Vec3 side; ///< Unit vector to the right as seen from the sun, parallel to the ground.
};

/**
 * Find how the sun looks at the area around what the camera looks at.
 * @param camera The camera.
 * @param sun Direction towards the sun.
 * @param homogeneous_depth Whether clip space depth runs from -1 to 1.
 * @param origin_bottom_left Whether textures start at the bottom.
 * @return The view of the sun.
 */
static ShadowView GetShadowView(const Camera3D &camera, Vec3 sun, bool homogeneous_depth, bool origin_bottom_left)
{
	/* Covers what is near enough to see shadows; when looking from far away, shadows are too small to matter. */
	float radius = std::clamp(camera.distance * 1.1f, 96.0f, 1200.0f);
	float depth = radius * 2 + 1024;

	sun = Normalise(sun);
	Vec3 side = Normalise(Cross(Vec3{0, 0, 1}, sun));
	Vec3 up = Cross(sun, side);

	/* Move the view by whole texels only, so shadows do not shimmer when the camera moves. */
	float texel = radius * 2 / SHADOW_MAP_SIZE;
	Vec3 centre = camera.target;
	float cs = std::floor(Dot(centre, side) / texel) * texel;
	float cu = std::floor(Dot(centre, up) / texel) * texel;
	float cd = Dot(centre, sun);
	Vec3 eye = side * cs + up * cu + sun * (cd + depth / 2);

	ShadowView result;
	/* Looks along -sun; x to the side, y up. */
	result.view.m = {
		side.x, up.x, sun.x, 0,
		side.y, up.y, sun.y, 0,
		side.z, up.z, sun.z, 0,
		-Dot(side, eye), -Dot(up, eye), -Dot(sun, eye), 1,
	};
	/* Orthographic; view space z runs from 0 at the eye to -depth. */
	result.proj.m = {};
	result.proj.m[0] = 1 / radius;
	result.proj.m[5] = 1 / radius;
	if (homogeneous_depth) {
		result.proj.m[10] = -2 / depth;
		result.proj.m[14] = -1;
	} else {
		result.proj.m[10] = -1 / depth;
		result.proj.m[14] = 0;
	}
	result.proj.m[15] = 1;

	/* From clip space to texture coordinates and depth from 0 to 1. */
	Matrix4 crop;
	float sy = origin_bottom_left ? 0.5f : -0.5f;
	float sz = homogeneous_depth ? 0.5f : 1.0f;
	float tz = homogeneous_depth ? 0.5f : 0.0f;
	crop.m = {
		0.5f, 0, 0, 0,
		0, sy, 0, 0,
		0, 0, sz, 0,
		0.5f, 0.5f, tz, 1,
	};
	result.sample = crop * result.proj * result.view;
	/* About a texel and a half of depth. */
	result.bias = 1.5f * texel / depth + 0.0005f;
	result.side = side;
	return result;
}

/**
 * Start drawing a new image. The result arrives a few calls to Poll() later.
 * Must not be called while IsBusy().
 * @param width Width of the image in pixels.
 * @param height Height of the image in pixels.
 * @param camera Where to look from.
 * @param chunks The chunks to draw.
 * @param sprites Sprites to draw that are in no chunk, such as vehicles; four vertices per sprite.
 * @param models 3D models to draw.
 */
void Renderer3D::Render(uint target_id, uint width, uint height, const Camera3D &camera, std::span<const ChunkID> chunks, std::span<const SpriteVertex> sprites, std::span<const ModelInstance> models, std::span<const TerrainVertex> moving_water)
{
	assert(!this->pending);
	Impl &impl = *this->impl;
	if (impl.callback.failed) return;

	width = Clamp(width, 1, 4096);
	height = Clamp(height, 1, 4096);

	Impl::Target &target = impl.targets[target_id];
	if (width != target.width || height != target.height) {
		target.Destroy();

		bgfx::TextureFormat::Enum depth_format = bgfx::TextureFormat::D24S8;
		for (bgfx::TextureFormat::Enum candidate : {bgfx::TextureFormat::D32F, bgfx::TextureFormat::D24S8, bgfx::TextureFormat::D24, bgfx::TextureFormat::D16}) {
			if (bgfx::isTextureValid(0, false, 1, candidate, BGFX_TEXTURE_RT_WRITE_ONLY)) {
				depth_format = candidate;
				break;
			}
		}

		/* Smooth the edges of hills, rails and models with 4x multisampling where the GPU can do it; the image is
		 * resolved to a single sample before it is copied for reading back. */
		uint64_t msaa = BGFX_TEXTURE_RT_MSAA_X4;
		if (!bgfx::isTextureValid(0, false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT_MSAA_X4) ||
				!bgfx::isTextureValid(0, false, 1, depth_format, BGFX_TEXTURE_RT_MSAA_X4 | BGFX_TEXTURE_RT_WRITE_ONLY)) {
			msaa = BGFX_TEXTURE_RT;
		}

		bgfx::TextureHandle attachments[] = {
			bgfx::createTexture2D(static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1, bgfx::TextureFormat::BGRA8, msaa | BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP),
			bgfx::createTexture2D(static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1, depth_format, (msaa & BGFX_TEXTURE_RT_MSAA_MASK) | BGFX_TEXTURE_RT_WRITE_ONLY),
		};
		target.framebuffer = bgfx::createFrameBuffer(static_cast<uint8_t>(std::size(attachments)), attachments, true);
		target.readback = bgfx::createTexture2D(static_cast<uint16_t>(width), static_cast<uint16_t>(height), false, 1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
		target.width = width;
		target.height = height;
	}

	static constexpr bgfx::ViewId VIEW_SHADOW = 0;
	static constexpr bgfx::ViewId VIEW_SCENE = 1;
	static constexpr bgfx::ViewId VIEW_READBACK = 2;

	Matrix4 view = camera.GetViewMatrix();
	Matrix4 proj = camera.GetProjectionMatrix(static_cast<float>(width) / height, bgfx::getCaps()->homogeneousDepth);

	bgfx::setViewFrameBuffer(VIEW_SCENE, target.framebuffer);
	bgfx::setViewRect(VIEW_SCENE, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height));
	bgfx::setViewClear(VIEW_SCENE, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, this->background != 0 ? this->background : 0xccdbebff, 1.0f, 0);
	bgfx::setViewTransform(VIEW_SCENE, view.m.data(), proj.m.data());
	bgfx::touch(VIEW_SCENE);

	/* The shadow map looks from the sun at the area around the point the camera looks at; further away there are no shadows. */
	/* Pictures for windows, which have a background of their own, are always taken by day. */
	const Lighting3D lighting = this->background != 0 ? Lighting3D{} : impl.lighting;
	const float MODEL_SUN[4] = {lighting.direction.x, lighting.direction.y, lighting.direction.z, 0};
	const float light_ambient[4] = {lighting.ambient[0], lighting.ambient[1], lighting.ambient[2], lighting.lamps};
	const float light_direct[4] = {lighting.direct[0], lighting.direct[1], lighting.direct[2], lighting.windows};
	ShadowView shadow = GetShadowView(camera, Vec3{MODEL_SUN[0], MODEL_SUN[1], MODEL_SUN[2]}, bgfx::getCaps()->homogeneousDepth, bgfx::getCaps()->originBottomLeft);
	bgfx::setViewFrameBuffer(VIEW_SHADOW, impl.shadow_map);
	bgfx::setViewRect(VIEW_SHADOW, 0, 0, SHADOW_MAP_SIZE, SHADOW_MAP_SIZE);
	bgfx::setViewClear(VIEW_SHADOW, BGFX_CLEAR_DEPTH, 0, 1.0f, 0);
	bgfx::setViewTransform(VIEW_SHADOW, shadow.view.m.data(), shadow.proj.m.data());
	bgfx::touch(VIEW_SHADOW);
	/* Clouds and fog take most of the shadows away. */
	const Weather3D &weather = impl.weather;
	const float shadow_params[4] = {impl.shadows ? 1.0f : 0.0f, shadow.bias, 1.0f / SHADOW_MAP_SIZE, std::min(0.85f, std::max(weather.overcast, weather.fog))};
	auto set_shadow = [&]() {
		bgfx::setTexture(1, impl.shadow_sampler, bgfx::getTexture(impl.shadow_map));
		bgfx::setUniform(impl.shadow_matrix, shadow.sample.m.data());
		bgfx::setUniform(impl.shadow_params, shadow_params);
	};

	/* Vertices are in tiles and height levels; the model matrix turns them into world units. */
	Matrix4 model;
	model.m[0] = TILE_SIZE;
	model.m[5] = TILE_SIZE;
	model.m[10] = HEIGHT_LEVEL_SIZE;
	model.m[15] = 1;

	/* What does not have a closed shape, like terrain and sprites, casts its shadow from a little further away from the sun,
	 * or it would shadow itself where its depth in the map and its own depth are nearly the same. */
	auto away_from_sun = [&](float distance) {
		Matrix4 moved = model;
		moved.m[12] -= MODEL_SUN[0] * distance;
		moved.m[13] -= MODEL_SUN[1] * distance;
		moved.m[14] -= MODEL_SUN[2] * distance;
		return moved;
	};
	const Matrix4 terrain_caster = away_from_sun(2.0f);
	const Matrix4 sprite_caster = away_from_sun(3.0f);
	/* Sprites turn towards the sun to cast their shadow, and keep their own depth. */
	const float shadow_sprite_axes[4] = {shadow.side.x * SPRITE_PIXEL_SIZE, shadow.side.y * SPRITE_PIXEL_SIZE, shadow.side.z * SPRITE_PIXEL_SIZE, WORLD_Z_SCALE};
	const float no_nearer[4] = {0, 0, 0, 0};
	auto cast_sprites = [&](auto set_buffer, uint32_t count) {
		if (!impl.shadows) return;
		bgfx::setTransform(sprite_caster.m.data());
		set_buffer();
		bgfx::setTexture(0, impl.sprite_sampler, impl.sprite_atlas);
		bgfx::setUniform(impl.sprite_axes, shadow_sprite_axes);
		bgfx::setUniform(impl.sprite_nearer, no_nearer);
		bgfx::setIndexBuffer(impl.quad_indices, 0, count * 6);
		bgfx::setState(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
		bgfx::submit(VIEW_SHADOW, impl.sprite_shadow_program);
	};

	Vec3 right = camera.GetRight();
	const float sprite_axes[4] = {right.x * SPRITE_PIXEL_SIZE, right.y * SPRITE_PIXEL_SIZE, right.z * SPRITE_PIXEL_SIZE, WORLD_Z_SCALE};

	/* A point this many pixels of height below a sprite's foot is hidden by ground that is that height divided by
	 * the sine of the camera's pitch nearer to the camera. Limited, or sprites would show through hills when looking along the ground. */
	Vec3 eye = camera.GetEye();
	float eye_distance = std::max(1.0f, std::sqrt((eye.x - camera.target.x) * (eye.x - camera.target.x) + (eye.y - camera.target.y) * (eye.y - camera.target.y) + (eye.z - camera.target.z) * (eye.z - camera.target.z)));
	const float sprite_nearer[4] = {(eye.x - camera.target.x) / eye_distance, (eye.y - camera.target.y) / eye_distance, (eye.z - camera.target.z) / eye_distance,
		WORLD_Z_SCALE / std::max(0.35f, std::sin(camera.pitch))};

	/* Far away things fade into the colour of the sky at the horizon; the sky gets deeper blue above it. */
	const float HAZE_COLOUR[4] = {lighting.haze[0], lighting.haze[1], lighting.haze[2], 1};
	const float SKY_COLOUR[4] = {lighting.sky[0], lighting.sky[1], lighting.sky[2], lighting.stars};
	const float sky_sun[4] = {lighting.sun.x, lighting.sun.y, lighting.sun.z, 0};
	const float sky_moon[4] = {lighting.moon.x, lighting.moon.y, lighting.moon.z, 0};
	/* Under clouds the sky and the haze turn grey; fog, and rain and snow a little, bring the haze nearer and make it thicker;
	 * lightning lights it all up. */
	const float mist = std::max({weather.fog, 0.3f * weather.rain, 0.4f * weather.snow});
	const float haze_grey = 0.30f * HAZE_COLOUR[0] + 0.59f * HAZE_COLOUR[1] + 0.11f * HAZE_COLOUR[2];
	float haze_colour[4], sky_colour[4];
	for (int i = 0; i < 3; i++) {
		float cloud = haze_grey * (0.86f - 0.14f * weather.rain);
		haze_colour[i] = Lerp(HAZE_COLOUR[i], cloud * 1.04f, 0.85f * weather.overcast);
		sky_colour[i] = Lerp(SKY_COLOUR[i], cloud * (i == 2 ? 0.98f : 0.93f), 0.92f * weather.overcast);
		sky_colour[i] = Lerp(sky_colour[i], haze_colour[i], weather.fog);
		haze_colour[i] = std::min(1.0f, haze_colour[i] + 0.5f * weather.flash);
		sky_colour[i] = std::min(1.0f, sky_colour[i] + 0.6f * weather.flash);
	}
	haze_colour[3] = HAZE_COLOUR[3];
	sky_colour[3] = SKY_COLOUR[3] * (1 - weather.overcast);
	const float haze_params[4] = {
		Lerp(std::max(250.0f, eye_distance * 1.4f), eye_distance * 0.55f, mist),
		1.0f / Lerp(std::max(800.0f, eye_distance * 5.0f), eye_distance * 1.5f, mist),
		Lerp(0.85f, 0.97f, mist), 0};
	const float weather_params[4] = {weather.cover, weather.wet, weather.overcast, weather.flash};
	const float season_params[4] = {weather.autumn, weather.bare, weather.spring, weather.dry};
	bgfx::setUniform(impl.haze_colour, haze_colour);
	bgfx::setUniform(impl.haze_params, haze_params);
	const float water[4] = {static_cast<float>(GROUND_TEXTURE_WATER), static_cast<float>(GROUND_TEXTURE_SHORE), static_cast<float>(GROUND_TEXTURE_SHORE + 18),
		std::fmod(std::chrono::duration<float>(std::chrono::steady_clock::now() - impl.start).count(), 3600.0f)};
	bgfx::setUniform(impl.water, water);
	/* x: number of layers that come from the model package. y: tiles per texture. z: layer of the beach. w: layer of the water over coasts. */
	const bool terrain_textures = bgfx::isValid(impl.terrain_textures);
	const float terrain[4] = {terrain_textures ? static_cast<float>(GROUND_TEXTURE_TERRAIN_END) : 0.0f, terrain_textures ? 1.0f / impl.terrain_tiles : 1.0f,
		static_cast<float>(GROUND_TEXTURE_BEACH), static_cast<float>(GROUND_TEXTURE_SHALLOWS)};
	bgfx::setUniform(impl.terrain, terrain);
	/* x: first layer that is mixed. y: layer of asphalt, followed by gravel and paving. z: the light of flat ground, as in BuildTerrainChunkMesh(), at half brightness. w: world units per height level. */
	const float layers[4] = {terrain_textures && bgfx::isValid(impl.ground_mix) ? static_cast<float>(GROUND_TEXTURE_END) : 4096.0f, static_cast<float>(GROUND_TEXTURE_ASPHALT), 0.5f * (Lighting3D::REFERENCE_AMBIENT + Lighting3D::REFERENCE_DIRECT * Lighting3D::REFERENCE_SUN.z), HEIGHT_LEVEL_SIZE};
	bgfx::setUniform(impl.layers, layers);
	/* Draws are sorted before they are made, so a draw may come before the one these were set for; and they would
	 * then keep the values of the previous image, which can be of another view. So every draw sets them. */
	auto set_scene = [&]() {
		bgfx::setUniform(impl.haze_colour, haze_colour);
		bgfx::setUniform(impl.haze_params, haze_params);
		bgfx::setUniform(impl.weather_params, weather_params);
		bgfx::setUniform(impl.season_params, season_params);
		bgfx::setUniform(impl.water, water);
		bgfx::setUniform(impl.terrain, terrain);
		bgfx::setUniform(impl.layers, layers);
		bgfx::setUniform(impl.light_ambient, light_ambient);
		bgfx::setUniform(impl.light_direct, light_direct);
		bgfx::setUniform(impl.light_direction, MODEL_SUN);
		set_shadow();
	};

	Matrix4 view_proj = proj * view;
	Matrix4 inverse;
	bx::mtxInverse(inverse.m.data(), view_proj.m.data());
	if (this->background == 0) {
		bgfx::setUniform(impl.sky_matrix, inverse.m.data());
		bgfx::setUniform(impl.haze_colour, haze_colour);
		bgfx::setUniform(impl.sky_colour, sky_colour);
		bgfx::setUniform(impl.sky_sun, sky_sun);
		bgfx::setUniform(impl.sky_moon, sky_moon);
		bgfx::setVertexBuffer(0, impl.sky_vertices);
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL);
		bgfx::submit(VIEW_SCENE, impl.sky_program);
	}

	/* At night the light of lamps is added to the ground below them, more as it gets darker. */
	const bool light_pools = lighting.lamps > 0.02f;
	const uint32_t pool_strength = static_cast<uint32_t>(std::clamp(lighting.lamps * 0.62f, 0.0f, 1.0f) * 255.0f) * 0x01010101U;
	static constexpr uint64_t POOL_STATE = BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LEQUAL | BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_FACTOR, BGFX_STATE_BLEND_ONE);
	/* Copies of the same model are drawn together when the GPU can, which matters for the thousands of trees in a forest. */
	const bool instancing = bgfx::isValid(impl.model_instanced_program) && bgfx::isValid(impl.shadow_instanced_program);
	if (impl.batches.size() < impl.models.size()) impl.batches.resize(impl.models.size());
	/* Beyond this distance models may be drawn simpler, or not at all when they are small details. */
	const float far_distance = std::max(320.0f, eye_distance * 1.6f);
	const float far_squared = far_distance * far_distance;
	auto draw_model = [&](const ModelInstance &original) {
		ModelInstance instance = original;
		if (instance.model < impl.far.size() && impl.far[instance.model] != ModelPack::FAR_SAME) {
			float dx = instance.transform.m[12] - eye.x;
			float dy = instance.transform.m[13] - eye.y;
			float dz = instance.transform.m[14] - eye.z;
			if (dx * dx + dy * dy + dz * dz > far_squared) {
				if (impl.far[instance.model] == ModelPack::FAR_HIDDEN) return;
				instance.model = impl.far[instance.model];
			}
		}
		if (instance.model >= impl.models.size()) return;
		const GpuModel &gpu = impl.models[instance.model];
		if (!bgfx::isValid(gpu.vertices)) return;
		/* See-through models, such as puffs of smoke, are blended over what is behind them and cast no shadow. */
		const bool see_through = instance.tint[3] < 1.0f;
		const bool batched = instancing && gpu.num_fixed_indices != 0 && !see_through;
		if (batched) {
			auto &copy = impl.batches[instance.model].emplace_back();
			std::copy_n(instance.transform.m.data(), 16, copy.data());
			std::copy_n(instance.tint.data(), 4, copy.data() + 16);
		}

		auto submit = [&](const Matrix4 &transform, uint32_t first, uint32_t count) {
			if (count == 0) return;
			bgfx::setTransform(transform.m.data());
			bgfx::setVertexBuffer(0, gpu.vertices);
			bgfx::setIndexBuffer(gpu.indices, first, count);
			bgfx::setUniform(impl.model_tint, instance.tint.data());
			bgfx::setTexture(0, impl.model_sampler, impl.model_textures);
			set_scene();
			if (see_through) {
				bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_BLEND_ALPHA);
				bgfx::submit(VIEW_SCENE, impl.model_program);
				return;
			}
			bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
			bgfx::submit(VIEW_SCENE, impl.model_program);

			if (light_pools && first == 0 && gpu.num_pool_indices != 0) {
				bgfx::setTransform(transform.m.data());
				bgfx::setVertexBuffer(0, gpu.pool_vertices);
				bgfx::setIndexBuffer(gpu.pool_indices, 0, gpu.num_pool_indices);
				bgfx::setUniform(impl.model_tint, instance.tint.data());
				bgfx::setTexture(0, impl.model_sampler, impl.model_textures);
				set_scene();
				bgfx::setState(POOL_STATE, pool_strength);
				bgfx::submit(VIEW_SCENE, impl.model_program);
			}

			if (!impl.shadows) return;
			bgfx::setTransform(transform.m.data());
			bgfx::setVertexBuffer(0, gpu.vertices);
			bgfx::setIndexBuffer(gpu.indices, first, count);
			bgfx::setState(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
			bgfx::submit(VIEW_SHADOW, impl.shadow_program);
		};
		if (!batched) submit(instance.transform, 0, gpu.num_fixed_indices);
		if (instance.load <= 0) return;

		/* The load sinks along the up axis of the model as the vehicle gets emptier. */
		Matrix4 load = instance.transform;
		float sink = (1 - std::min(instance.load, 1.0f)) * gpu.load_depth;
		for (int i = 0; i < 3; i++) load.m[12 + i] -= load.m[8 + i] * sink;
		submit(load, gpu.num_fixed_indices, gpu.num_indices - gpu.num_fixed_indices);
	};

	for (ChunkID id : chunks) {
		auto it = impl.chunks.find(id);
		if (it == impl.chunks.end()) continue;
		const GpuChunk &chunk = it->second;

		if (bgfx::isValid(chunk.vertices) && chunk.num_water != 0) {
			/* See-through water over the land of coasts; bgfx draws what blends after everything else of the view. */
			bgfx::setTransform(model.m.data());
			bgfx::setVertexBuffer(0, chunk.vertices, chunk.num_quads * 4, chunk.num_water * 4);
			bgfx::setTexture(0, impl.ground_sampler, impl.ground_textures);
			bgfx::setTexture(2, impl.terrain_sampler, terrain_textures ? impl.terrain_textures : impl.ground_textures);
			bgfx::setTexture(3, impl.mix_sampler, bgfx::isValid(impl.ground_mix) ? impl.ground_mix : impl.ground_textures);
			bgfx::setIndexBuffer(impl.quad_indices, 0, chunk.num_water * 6);
			set_scene();
			bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA, BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_ONE));
			bgfx::submit(VIEW_SCENE, impl.program);
		}

		if (bgfx::isValid(chunk.vertices) && chunk.num_quads != 0) {
			bgfx::setTransform(model.m.data());
			bgfx::setVertexBuffer(0, chunk.vertices, 0, chunk.num_quads * 4);
			bgfx::setTexture(0, impl.ground_sampler, impl.ground_textures);
			bgfx::setTexture(2, impl.terrain_sampler, terrain_textures ? impl.terrain_textures : impl.ground_textures);
			bgfx::setTexture(3, impl.mix_sampler, bgfx::isValid(impl.ground_mix) ? impl.ground_mix : impl.ground_textures);
			bgfx::setIndexBuffer(impl.quad_indices, 0, chunk.num_quads * 6);
			set_scene();
			bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
			bgfx::submit(VIEW_SCENE, impl.program);

			if (impl.shadows) {
				bgfx::setTransform(terrain_caster.m.data());
				bgfx::setVertexBuffer(0, chunk.vertices, 0, chunk.num_quads * 4);
				bgfx::setIndexBuffer(impl.quad_indices, 0, chunk.num_quads * 6);
				bgfx::setState(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
				bgfx::submit(VIEW_SHADOW, impl.shadow_program);
			}
		}

		for (const ModelInstance &instance : chunk.models) draw_model(instance);

		if (bgfx::isValid(chunk.sprites)) {
			bgfx::setTransform(model.m.data());
			bgfx::setVertexBuffer(0, chunk.sprites);
			bgfx::setTexture(0, impl.sprite_sampler, impl.sprite_atlas);
			bgfx::setUniform(impl.sprite_axes, sprite_axes);
			bgfx::setUniform(impl.sprite_nearer, sprite_nearer);
			bgfx::setIndexBuffer(impl.quad_indices, 0, chunk.num_sprites * 6);
			set_scene();
			bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
			bgfx::submit(VIEW_SCENE, impl.sprite_program);
			cast_sprites([&]() { bgfx::setVertexBuffer(0, chunk.sprites); }, chunk.num_sprites);
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
		set_scene();
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
		bgfx::submit(VIEW_SCENE, impl.sprite_program);
		cast_sprites([&]() { bgfx::setVertexBuffer(0, &buffer); }, num_sprites);
	}

	uint32_t num_water = static_cast<uint32_t>(std::min<size_t>(moving_water.size() / 4, MAX_CHUNK_QUADS));
	if (num_water != 0 && bgfx::getAvailTransientVertexBuffer(num_water * 4, impl.layout) >= num_water * 4) {
		/* Water that moves, such as in the chambers of locks; see-through like the water over coasts. */
		bgfx::TransientVertexBuffer buffer;
		bgfx::allocTransientVertexBuffer(&buffer, num_water * 4, impl.layout);
		std::copy_n(reinterpret_cast<const uint8_t *>(moving_water.data()), num_water * 4 * sizeof(TerrainVertex), buffer.data);
		bgfx::setTransform(model.m.data());
		bgfx::setVertexBuffer(0, &buffer);
		bgfx::setTexture(0, impl.ground_sampler, impl.ground_textures);
		bgfx::setTexture(2, impl.terrain_sampler, terrain_textures ? impl.terrain_textures : impl.ground_textures);
		bgfx::setTexture(3, impl.mix_sampler, bgfx::isValid(impl.ground_mix) ? impl.ground_mix : impl.ground_textures);
		bgfx::setIndexBuffer(impl.quad_indices, 0, num_water * 6);
		set_scene();
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_BLEND_FUNC_SEPARATE(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA, BGFX_STATE_BLEND_ZERO, BGFX_STATE_BLEND_ONE));
		bgfx::submit(VIEW_SCENE, impl.program);
	}

	for (const ModelInstance &instance : models) draw_model(instance);

	static constexpr uint16_t INSTANCE_STRIDE = sizeof(std::array<float, 20>);
	for (uint id = 0; id < impl.batches.size(); id++) {
		auto &batch = impl.batches[id];
		if (batch.empty()) continue;
		const GpuModel &gpu = impl.models[id];
		for (size_t first = 0; first < batch.size();) {
			uint32_t count = bgfx::getAvailInstanceDataBuffer(static_cast<uint32_t>(std::min<size_t>(batch.size() - first, UINT32_MAX)), INSTANCE_STRIDE);
			if (count == 0) break;
			bgfx::InstanceDataBuffer copies;
			bgfx::allocInstanceDataBuffer(&copies, count, INSTANCE_STRIDE);
			std::copy_n(reinterpret_cast<const uint8_t *>(batch.data() + first), static_cast<size_t>(count) * INSTANCE_STRIDE, copies.data);
			first += count;

			bgfx::setVertexBuffer(0, gpu.vertices);
			bgfx::setIndexBuffer(gpu.indices, 0, gpu.num_fixed_indices);
			bgfx::setInstanceDataBuffer(&copies);
			bgfx::setTexture(0, impl.model_sampler, impl.model_textures);
			set_scene();
			bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
			bgfx::submit(VIEW_SCENE, impl.model_instanced_program);

			if (light_pools && gpu.num_pool_indices != 0) {
				bgfx::setVertexBuffer(0, gpu.pool_vertices);
				bgfx::setIndexBuffer(gpu.pool_indices, 0, gpu.num_pool_indices);
				bgfx::setInstanceDataBuffer(&copies);
				bgfx::setTexture(0, impl.model_sampler, impl.model_textures);
				set_scene();
				bgfx::setState(POOL_STATE, pool_strength);
				bgfx::submit(VIEW_SCENE, impl.model_instanced_program);
			}

			if (!impl.shadows) continue;
			bgfx::setVertexBuffer(0, gpu.vertices);
			bgfx::setIndexBuffer(gpu.indices, 0, gpu.num_fixed_indices);
			bgfx::setInstanceDataBuffer(&copies);
			bgfx::setState(BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS);
			bgfx::submit(VIEW_SHADOW, impl.shadow_instanced_program);
		}
		batch.clear();
	}

	/* Rain and snow: the drops fill a cube around the spot the camera looks at. Two cubes, one twice the size of the other,
	 * take over from each other as the camera zooms, so drops keep about their size and number on the screen. */
	if (weather.rain > 0.005f || weather.snow > 0.005f) {
		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - impl.start).count();
		const float zoom = std::log2(eye_distance);
		const float between = zoom - std::floor(zoom);
		Vec3 look = Normalise(camera.target - eye);
		const float drop_look[4] = {look.x, look.y, look.z, 0};
		Vec3 fall = Normalise(Vec3{weather.wind_x, weather.wind_y, -1});

		auto draw_drops = [&](float size, float show, bool snow, float amount) {
			uint32_t count = std::min<uint32_t>({static_cast<uint32_t>(NUM_DROPS * amount), NUM_DROPS, MAX_CHUNK_QUADS});
			if (count == 0 || show < 0.01f) return;
			/* Snow falls slowly and straighter down than rain. */
			Vec3 way = snow ? Normalise(Vec3{weather.wind_x * 1.5f, weather.wind_y * 1.5f, -1}) : fall;
			const double speed = snow ? 0.085 : 0.95;
			Vec3 centre = camera.target;
			centre.z += size * 0.12f;
			float box[4] = {centre.x, centre.y, centre.z, size};
			float move[4];
			const float *middle = box;
			const float direction[3] = {way.x, way.y, way.z};
			for (int i = 0; i < 3; i++) {
				/* Minus the middle of the cube, so the drops stay where they are in the world when the camera moves. */
				double moved = direction[i] * speed * seconds - static_cast<double>(middle[i]) / size + 0.5;
				move[i] = static_cast<float>(moved - std::floor(moved));
			}
			move[3] = show;
			const float shape[4] = {size * (snow ? 0.0026f : 0.00055f), size * (snow ? 0.0026f : 0.0045f), snow ? 1.0f : 0.0f, static_cast<float>(std::fmod(seconds, 3600.0))};
			const float drop_fall[4] = {way.x, way.y, way.z, 0};
			float bright = snow ? 1.0f : 1.0f - 0.25f * weather.overcast + 0.5f * weather.flash;
			const float colour[4] = {(snow ? 0.97f : 0.78f) * bright, (snow ? 0.98f : 0.84f) * bright, (snow ? 1.0f : 0.94f) * bright, snow ? 0.95f : 0.6f};
			Matrix4 identity;
			identity.m[0] = identity.m[5] = identity.m[10] = identity.m[15] = 1;
			bgfx::setTransform(identity.m.data());
			bgfx::setVertexBuffer(0, impl.drop_vertices, 0, count * 4);
			bgfx::setIndexBuffer(impl.quad_indices, 0, count * 6);
			bgfx::setUniform(impl.drop_box, box);
			bgfx::setUniform(impl.drop_move, move);
			bgfx::setUniform(impl.drop_shape, shape);
			bgfx::setUniform(impl.drop_fall, drop_fall);
			bgfx::setUniform(impl.drop_look, drop_look);
			bgfx::setUniform(impl.drop_colour, colour);
			bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_BLEND_ALPHA);
			bgfx::submit(VIEW_SCENE, impl.drop_program);
		};
		const float size = std::exp2(std::floor(zoom));
		for (bool snow : {false, true}) {
			float amount = snow ? weather.snow : weather.rain;
			if (amount <= 0.005f) continue;
			draw_drops(size, 1 - between, snow, amount);
			draw_drops(size * 2, between, snow, amount);
		}
	}

	/* Copy the finished image into a texture the CPU can read, and ask for its contents. */
	bgfx::blit(VIEW_READBACK, bgfx::TextureRegion{.handle = target.readback}, bgfx::TextureRegion{.handle = bgfx::getTexture(target.framebuffer, 0)});

	this->pending_image.resize(static_cast<size_t>(width) * height);
	this->pending_width = width;
	this->pending_height = height;
	this->pending_frame = bgfx::read(bgfx::TextureRegion{.handle = target.readback}, this->pending_image.data());
	this->pending_target = target_id;
	this->pending = true;
}

/**
 * Forget a view: free its render target and its image.
 * @param target_id The view, as given to Render().
 */
void Renderer3D::ReleaseTarget(uint target_id)
{
	this->images.erase(target_id);
	/* The GPU may still be copying out of it; then it goes once that is done. */
	if (this->pending && this->pending_target == target_id) {
		this->release_pending = true;
		return;
	}
	auto it = this->impl->targets.find(target_id);
	if (it == this->impl->targets.end()) return;
	it->second.Destroy();
	this->impl->targets.erase(it);
}

/**
 * Let the GPU make progress on a requested image. Call once per display frame while IsBusy().
 * @return Whether a new image became available through GetImage().
 */
bool Renderer3D::Poll(uint *target_id)
{
	if (!this->pending) return false;
	if (this->impl->callback.failed) {
		this->pending = false;
		return false;
	}

	uint32_t frame = bgfx::frame();
	_uploaded_bytes = 0;
	if (frame < this->pending_frame) return false;

	this->pending = false;
	if (this->release_pending) {
		this->release_pending = false;
		this->ReleaseTarget(this->pending_target);
		return false;
	}

	Image &image = this->images[this->pending_target];
	image.width = this->pending_width;
	image.height = this->pending_height;

	if (bgfx::getCaps()->originBottomLeft) {
		/* The image arrives with the bottom row first; flip it. */
		image.pixels.resize(this->pending_image.size());
		for (uint y = 0; y < image.height; y++) {
			std::copy_n(this->pending_image.begin() + static_cast<size_t>(image.height - 1 - y) * image.width, image.width, image.pixels.begin() + static_cast<size_t>(y) * image.width);
		}
	} else {
		std::swap(image.pixels, this->pending_image);
	}
	/* Blitters treat the top byte as alpha; make sure the image is opaque. */
	for (uint32_t &pixel : image.pixels) pixel |= 0xFF000000;
	if (target_id != nullptr) *target_id = this->pending_target;
	return true;
}
