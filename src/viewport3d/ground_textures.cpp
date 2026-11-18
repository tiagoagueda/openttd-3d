/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file ground_textures.cpp Ground textures for the 3D viewport, made from the ground sprites of the 2D game. */

#include "../stdafx.h"
#include "ground_textures.h"
#include "../core/backup_type.hpp"
#include "../core/math_func.hpp"
#include "../gfx_func.h"
#include "../landscape_type.h"
#include "../settings_type.h"
#include "../slope_func.h"
#include "../zoom_func.h"
#include "../table/sprites.h"

#include "../safeguards.h"

/**
 * Let the blitter draw sprites over each other into an image that covers exactly the diamond of a tile.
 * @param sprites The ground sprites, in the order the 2D game draws them, relative to where it draws the northern corner of the tile.
 * @param[out] width Width of the image.
 * @param[out] height Height of the image.
 * @return The pixels, in the format of 32bpp blitters; transparent where nothing was drawn.
 */
std::vector<uint32_t> GroundTextures::DrawDiamond(std::span<const SpritePart> sprites, int &width, int &height) const
{
	const ZoomLevel zoom = this->zoom;
	/* Size of the sprites compared to normal zoom, at which the diamond is 64 pixels wide and 31 high. */
	const int scale = UnScaleByZoom(4, zoom);
	width = 64 * scale;
	/* Raised corners are drawn up to two height levels of 8 pixels above the tile's origin. */
	const int margin = 16 * scale;
	height = 31 * scale + margin + scale;

	std::vector<uint32_t> diamond(static_cast<size_t>(width) * height, 0);
	DrawPixelInfo dpi;
	dpi.dst_ptr = diamond.data();
	dpi.pitch = width;
	dpi.left = 0;
	dpi.top = 0;
	dpi.width = width;
	dpi.height = height;
	dpi.zoom = zoom;

	AutoRestoreBackup dpi_backup(_cur_dpi, &dpi);
	/* Keep the blitters with palette animation from writing to the animation buffer of the screen. */
	AutoRestoreBackup anim_backup(_screen_disable_anim, true);
	/* The tile's origin, its northern corner, is one (unzoomed) pixel left of the diamond's centre line. */
	for (const SpritePart &part : sprites) {
		/* Sprites that make what is below them see-through cannot be drawn onto nothing. */
		if (HasBit(part.sprite, PALETTE_MODIFIER_TRANSPARENT)) continue;
		/* Clipping is relative to the position of the sprite, here in pixels at the most detailed zoom level. */
		SubSprite sub{part.clip_left * static_cast<int>(ZOOM_BASE), part.clip_top * static_cast<int>(ZOOM_BASE), (part.clip_right + 1) * static_cast<int>(ZOOM_BASE) - 1, (part.clip_bottom + 1) * static_cast<int>(ZOOM_BASE) - 1};
		DrawSprite(part.sprite, part.pal, width / 2 - scale + UnScaleByZoom(part.x, zoom), margin + UnScaleByZoom(part.y, zoom), part.clipped ? &sub : nullptr, zoom);
	}
	return diamond;
}

/**
 * Turn the sprites of a ground tile into a square texture.
 *
 * In the 2D game a flat tile is a diamond of 64 by 31 pixels (at normal zoom). The texture is that
 * diamond pulled straight: the texture's u axis runs along the tile's x axis and v along its y axis.
 * On a slope the diamond is bent; the heights of the corners tell how.
 *
 * @param sprites The ground sprites, in the order the 2D game draws them, relative to where it draws the northern corner of the tile.
 * @param heights Heights of the northern, western, eastern and southern corner, in height levels above the lowest.
 * @param[out] texture The pixels of the texture.
 * @param[out] mix If not \c nullptr and mixing: the mix of the texture, see #GroundMix; the texture then has its colours
 *                 multiplied by how much of the pixel they cover.
 * @param[out] under If not \c nullptr and mixing: the texture of the package for the landscape in the sprites.
 * @return Whether the sprites could be used.
 */
bool GroundTextures::Render(std::span<const SpritePart> sprites, const std::array<uint8_t, 4> &heights, std::vector<uint32_t> &texture, std::vector<uint32_t> *mix, uint8_t *under)
{
	const ZoomLevel zoom = this->zoom;
	const uint size = this->size;

	/* Size of the sprites compared to normal zoom, at which the diamond is 64 pixels wide and 31 high. */
	const int scale = UnScaleByZoom(4, zoom);
	/* Raised corners are drawn up to two height levels of 8 pixels above the tile's origin. */
	const int margin = 16 * scale;
	int diamond_width, diamond_height;
	std::vector<uint32_t> diamond = this->DrawDiamond(sprites, diamond_width, diamond_height);

	const float hn = heights[0], hw = heights[1], he = heights[2], hs = heights[3];
	/* Height of the surface in height levels; the tile is split in two triangles the same way as in the terrain mesh. */
	auto surface = [&](float u, float v) {
		if (std::abs(hn - hs) <= std::abs(hw - he)) {
			return u >= v ? hn + (hw - hn) * (u - v) + (hs - hn) * v : hn + (he - hn) * (v - u) + (hs - hn) * u;
		}
		return u + v <= 1.0f ? hn + (hw - hn) * u + (he - hn) * v : hs + (hw - hs) * (1.0f - v) + (he - hs) * (1.0f - u);
	};
	/* Where a point of the tile is in the image. */
	auto project = [&](float u, float v, float &fx, float &fy) {
		fx = diamond_width * 0.5f + (v - u) * diamond_width * 0.5f;
		fy = margin + (u + v) * 15.5f * scale - surface(u, v) * 8 * scale;
	};
	float centre_x, centre_y;
	project(0.5f, 0.5f, centre_x, centre_y);

	/* Bare landscape in the sprites makes way for the textures of the package. */
	const bool classify = this->mixing && mix != nullptr;
	std::array<uint, 4> landscape_votes{};

	/* Look up a point of the tile in the image. Returns the colour, or nothing when there is no pixel; landscape is the texture of the package it stands for, or 0. */
	auto sample = [&](float u, float v, uint8_t &landscape) -> std::optional<uint32_t> {
		float fx, fy;
		project(u, v, fx, fy);
		/* The very corners of the diamond are not in the sprite; move towards the centre until there is a pixel. */
		for (int step = 0; step <= 16; step++) {
			float t = step / 16.0f;
			int x = Clamp(static_cast<int>(fx + (centre_x - fx) * t), 0, diamond_width - 1);
			int y = Clamp(static_cast<int>(fy + (centre_y - fy) * t), 0, diamond_height - 1);
			uint32_t pixel = diamond[static_cast<size_t>(y) * diamond_width + x];
			if ((pixel >> 24) == 0) continue;
			landscape = 0;
			if (classify) {
				auto it = this->landscape_colours.find(pixel | 0xFF000000);
				if (it != this->landscape_colours.end()) landscape = it->second;
			}
			return pixel;
		}
		return std::nullopt;
	};

	texture.assign(static_cast<size_t>(size) * size, 0);
	if (classify) mix->assign(static_cast<size_t>(size) * size, 0);
	for (uint ty = 0; ty < size; ty++) {
		for (uint tx = 0; tx < size; tx++) {
			/* Average four samples per pixel; the diamond has fewer pixels than the texture along one diagonal and more along the other. */
			uint b = 0, g = 0, r = 0, count = 0, land = 0;
			for (float dy : {0.25f, 0.75f}) {
				for (float dx : {0.25f, 0.75f}) {
					uint8_t landscape;
					std::optional<uint32_t> pixel = sample((tx + dx) / size, (ty + dy) / size, landscape);
					if (!pixel.has_value()) continue;
					count++;
					if (landscape != 0) {
						land++;
						landscape_votes[landscape == GROUND_TEXTURE_GRASS + 3 ? 1 : (landscape == GROUND_TEXTURE_GRASS ? 2 : 3)]++;
						continue;
					}
					b += *pixel & 0xFF;
					g += (*pixel >> 8) & 0xFF;
					r += (*pixel >> 16) & 0xFF;
				}
			}
			if (count == 0) return false;
			size_t i = static_cast<size_t>(ty) * size + tx;
			if (classify) {
				/* Colours are multiplied by how much of the pixel they cover, so that filtering blends them right with the landscape. */
				uint alpha = (255 * (count - land) + count / 2) / count;
				texture[i] = (alpha << 24) | ((r / count) << 16) | ((g / count) << 8) | (b / count);
				(*mix)[i] = ((255 - alpha) << GroundMix::LAND);
			} else {
				texture[i] = 0xFF000000 | ((r / count) << 16) | ((g / count) << 8) | (b / count);
			}
		}
	}
	if (classify && under != nullptr) {
		/* The landscape that most of the tile shows. */
		static constexpr uint8_t LAYERS[4] = {GROUND_TEXTURE_GRASS + 3, GROUND_TEXTURE_GRASS + 3, GROUND_TEXTURE_GRASS, GROUND_TEXTURE_SNOW_DESERT + 3};
		*under = LAYERS[std::max_element(landscape_votes.begin() + 1, landscape_votes.end()) - landscape_votes.begin()];
	}
	return true;
}

/**
 * Make a ground texture from sprites.
 * @param layer The layer of the texture array to make.
 * @param sprites The ground sprites, in the order the 2D game draws them, relative to where it draws the northern corner of the tile.
 * @param heights Heights of the northern, western, eastern and southern corner, in height levels above the lowest.
 * @return Whether the sprites could be used.
 */
bool GroundTextures::Build(uint layer, std::span<const SpritePart> sprites, const std::array<uint8_t, 4> &heights)
{
	std::vector<uint32_t> texture, mix;
	uint8_t under = GROUND_TEXTURE_GRASS + 3;
	if (!this->Render(sprites, heights, texture, &mix, &under)) return false;
	if (layer >= this->under.size()) this->under.resize(layer + 1, GROUND_TEXTURE_GRASS + 3);
	this->under[layer] = under;
	this->pending.push_back({layer, std::move(texture), std::move(mix)});
	return true;
}

/** A colour with fractions, for working on textures. */
struct TextureColour {
	float r, g, b;
};

/** The average colour of an image. */
static TextureColour GetAverageColour(std::span<const uint32_t> pixels)
{
	uint64_t r = 0, g = 0, b = 0;
	for (uint32_t pixel : pixels) {
		r += (pixel >> 16) & 0xFF;
		g += (pixel >> 8) & 0xFF;
		b += pixel & 0xFF;
	}
	float count = std::max<size_t>(1, pixels.size());
	return {r / count, g / count, b / count};
}

/** Lighten or darken the colour channels of an image so that its average colour becomes \a target. */
static void TintTo(std::vector<uint32_t> &pixels, const TextureColour &target)
{
	TextureColour average = GetAverageColour(pixels);
	float fr = target.r / std::max(1.0f, average.r), fg = target.g / std::max(1.0f, average.g), fb = target.b / std::max(1.0f, average.b);
	for (uint32_t &pixel : pixels) {
		uint r = std::min(255u, static_cast<uint>(((pixel >> 16) & 0xFF) * fr + 0.5f));
		uint g = std::min(255u, static_cast<uint>(((pixel >> 8) & 0xFF) * fg + 0.5f));
		uint b = std::min(255u, static_cast<uint>((pixel & 0xFF) * fb + 0.5f));
		pixel = 0xFF000000 | (r << 16) | (g << 8) | b;
	}
}

/**
 * Let one kind of ground take over from another, in patches.
 * @param from The ground that is there to start with.
 * @param to The ground that takes over.
 * @param mask Where \a to comes first (dark) and last (light).
 * @param amount How far the change has gone, 0 to 1.
 * @return The mix.
 */
static std::vector<uint32_t> BlendGround(std::span<const uint32_t> from, std::span<const uint32_t> to, std::span<const uint32_t> mask, float amount)
{
	/* The patches have slightly soft edges. */
	static constexpr float EDGE = 0.06f;

	std::vector<uint32_t> result(from.size());
	for (size_t i = 0; i < from.size(); i++) {
		float m = (mask[i] & 0xFF) / 255.0f;
		float t = std::clamp((amount * (1.0f + 2 * EDGE) - EDGE - m) / EDGE * 0.5f + 0.5f, 0.0f, 1.0f);
		uint32_t pixel = 0xFF000000;
		for (uint shift = 0; shift < 24; shift += 8) {
			float a = (from[i] >> shift) & 0xFF, b = (to[i] >> shift) & 0xFF;
			pixel |= static_cast<uint>(a + (b - a) * t + 0.5f) << shift;
		}
		result[i] = pixel;
	}
	return result;
}

/**
 * Make the textures of the landscape from the ground textures of the model package.
 * Each is tinted to the average colour of the ground sprite that the 2D game draws for that kind of ground,
 * so the 3D view keeps the colours of the climate and of the graphics in use.
 * @param pack The model package; it must have ground textures.
 * @return The textures, for the layers up to #GROUND_TEXTURE_TERRAIN_END.
 */
std::vector<GroundTextures::Pending> GroundTextures::MakeTerrainTextures(const ModelPack &pack)
{
	assert(pack.HasGround());

	auto image = [&](std::string_view name, std::string_view otherwise) -> const std::vector<uint32_t> & {
		const ModelPack::GroundImage *found = pack.GetGroundImage(name);
		if (found == nullptr) found = pack.GetGroundImage(otherwise);
		return found->pixels;
	};
	const std::vector<uint32_t> &grass = image("grass", "grass");
	const std::vector<uint32_t> &soil = image("soil", "soil");
	const std::vector<uint32_t> &mask = image("mask", "mask");
	const std::vector<uint32_t> &rough = image("rough", "grass");
	const std::vector<uint32_t> &rocks = image("rocks", "grass");

	/* The average colour of ground sprites; nothing when the sprites cannot be used. */
	std::vector<uint32_t> rendered;
	auto colour_of = [&](SpriteID sprite) -> std::optional<TextureColour> {
		SpritePart part{.sprite = sprite, .pal = PAL_NONE};
		if (!this->Render(std::span<const SpritePart>(&part, 1), std::array<uint8_t, 4>{}, rendered)) return std::nullopt;
		return GetAverageColour(rendered);
	};
	auto tinted = [&](const std::vector<uint32_t> &pixels, SpriteID sprite) {
		std::vector<uint32_t> result = pixels;
		if (auto colour = colour_of(sprite); colour.has_value()) TintTo(result, *colour);
		return result;
	};

	std::vector<Pending> layers;
	layers.push_back({GROUND_TEXTURE_NONE, std::vector<uint32_t>(grass.size(), 0xFFFFFFFF)});

	/* Bare land that grass grows over. */
	static constexpr SpriteID GRASS[] = {SPR_FLAT_BARE_LAND, SPR_FLAT_1_THIRD_GRASS_TILE, SPR_FLAT_2_THIRD_GRASS_TILE, SPR_FLAT_GRASS_TILE};
	std::vector<uint32_t> bare = tinted(soil, SPR_FLAT_BARE_LAND);
	std::vector<uint32_t> full = tinted(grass, SPR_FLAT_GRASS_TILE);
	for (uint i = 0; i < std::size(GRASS); i++) {
		layers.push_back({GROUND_TEXTURE_GRASS + i, BlendGround(bare, full, mask, i / 3.0f)});
	}

	static constexpr SpriteID ROUGH[] = {SPR_FLAT_ROUGH_LAND, SPR_FLAT_ROUGH_LAND_1, SPR_FLAT_ROUGH_LAND_2, SPR_FLAT_ROUGH_LAND_3, SPR_FLAT_ROUGH_LAND_4};
	for (uint i = 0; i < std::size(ROUGH); i++) layers.push_back({GROUND_TEXTURE_ROUGH + i, tinted(rough, ROUGH[i])});
	layers.push_back({GROUND_TEXTURE_ROCKS, tinted(rocks, SPR_FLAT_ROCKY_LAND_1)});

	/* Farm fields are ploughed earth in the colour of the crop. */
	for (uint i = 0; i < 9; i++) layers.push_back({GROUND_TEXTURE_FIELDS + i, tinted(soil, SPR_FARMLAND_BARE + i * 19)});

	/* Snow in the sub-arctic climate and desert in the sub-tropical one cover the grass bit by bit. */
	const std::vector<uint32_t> &cover_image = _settings_game.game_creation.landscape == LandscapeType::Tropic ? image("desert", "sand") : image("snow", "sand");
	std::vector<uint32_t> cover = tinted(cover_image, SPR_FLAT_SNOW_DESERT_TILE);
	std::vector<uint32_t> stony = tinted(rocks, SPR_FLAT_ROCKY_LAND_1);
	for (uint i = 0; i < 4; i++) {
		float amount = (i + 1) / 4.0f;
		layers.push_back({GROUND_TEXTURE_SNOW_DESERT + i, BlendGround(full, cover, mask, amount)});
		layers.push_back({GROUND_TEXTURE_SNOWY_ROCKS + i, BlendGround(stony, cover, mask, amount)});
		layers.push_back({GROUND_TEXTURE_DESERT_ROCKS + i, BlendGround(stony, cover, mask, amount)});
	}

	layers.push_back({GROUND_TEXTURE_WATER, tinted(image("water", "water"), SPR_FLAT_WATER_TILE)});
	/* The beach has its own colour; the 2D game has no sprite of only sand to take it from. */
	layers.push_back({GROUND_TEXTURE_BEACH, image("sand", "sand")});
	/* Roads, tracks and pavements have their own colours too. */
	layers.push_back({GROUND_TEXTURE_ASPHALT, image("asphalt", "soil")});
	layers.push_back({GROUND_TEXTURE_GRAVEL, image("gravel", "soil")});
	layers.push_back({GROUND_TEXTURE_PAVING, image("paving", "soil")});
	return layers;
}

/**
 * Mix the ground textures of the model package into the textures made from sprites from now on:
 * where those sprites show bare landscape, the landscape of the package shows instead.
 * Must be called before the first texture is made from sprites.
 */
void GroundTextures::UseTerrain()
{
	assert(this->layers.empty() && this->patterns.empty());
	this->mixing = true;
	this->landscape_colours.clear();

	/* The colours of the landscape sprites, on all slopes; each slope has its own shades. Later kinds do not take colours that are taken. */
	auto collect = [&](SpriteID first, uint8_t layer) {
		for (uint slope = 0; slope < 19; slope++) {
			SpritePart part{.sprite = first + slope, .pal = PAL_NONE};
			int width, height;
			for (uint32_t pixel : this->DrawDiamond(std::span<const SpritePart>(&part, 1), width, height)) {
				if ((pixel >> 24) != 0) this->landscape_colours.try_emplace(pixel | 0xFF000000, layer);
			}
		}
	};
	collect(SPR_FLAT_GRASS_TILE, GROUND_TEXTURE_GRASS + 3);
	collect(SPR_FLAT_BARE_LAND, GROUND_TEXTURE_GRASS);
	collect(SPR_FLAT_1_THIRD_GRASS_TILE, GROUND_TEXTURE_GRASS + 3);
	collect(SPR_FLAT_2_THIRD_GRASS_TILE, GROUND_TEXTURE_GRASS + 3);
	/* Only the sub-arctic and sub-tropical climates have snow or desert. */
	if (_settings_game.game_creation.landscape == LandscapeType::Arctic || _settings_game.game_creation.landscape == LandscapeType::Tropic) {
		collect(SPR_FLAT_SNOW_DESERT_TILE, GROUND_TEXTURE_SNOW_DESERT + 3);
		collect(SPR_FLAT_3_QUART_SNOW_DESERT_TILE, GROUND_TEXTURE_SNOW_DESERT + 3);
	}
}

/**
 * Get the layer of a pattern: the ground of a road or of tracks, drawn with the textures of the package alone.
 * Patterns are flat shapes on the square of the tile, so one serves all slopes.
 * @param kind What the pattern is of.
 * @param bits Its road or track bits.
 * @param reserved For tracks: those that are reserved for a train, which get a pale edge along the track bed.
 * @return The layer, or #GROUND_TEXTURE_NONE when there is no room.
 */
uint GroundTextures::GetPattern(PatternKind kind, uint8_t bits, uint8_t reserved)
{
	assert(this->mixing);
	auto [it, inserted] = this->patterns.try_emplace({kind, bits, reserved}, GROUND_TEXTURE_NONE);
	if (!inserted || this->next_layer >= this->max_layers) return it->second;

	/* Distance from a point to the segment from a to b. */
	auto segment = [](float u, float v, float au, float av, float bu, float bv) {
		float du = bu - au, dv = bv - av;
		float t = std::clamp(((u - au) * du + (v - av) * dv) / (du * du + dv * dv), 0.0f, 1.0f);
		float eu = u - au - du * t, ev = v - av - dv * t;
		return std::sqrt(eu * eu + ev * ev);
	};
	/* How far a point is from the nearest piece of track or road, in tiles. */
	auto distance = [&](float u, float v, uint8_t bits) {
		float nearest = 2.0f;
		if (kind == PatternKind::Rail || kind == PatternKind::Gravel || kind == PatternKind::Paving) {
			/* The middle lines of the six tracks, from edge to edge; the same as the pieces of 3D track. */
			static constexpr float TRACKS[6][4] = {
				{0, 0.5f, 1, 0.5f}, {0.5f, 0, 0.5f, 1}, {0, 0.5f, 0.5f, 0}, {0.5f, 1, 1, 0.5f}, {0.5f, 0, 1, 0.5f}, {0, 0.5f, 0.5f, 1},
			};
			for (uint track = 0; track < 6; track++) {
				if (!HasBit(bits, track)) continue;
				const float *t = TRACKS[track];
				/* The track goes on in the next tile, so the edge of its bed goes on straight up to the edge of this tile.
				 * That matters for the pieces across a corner: near their ends, points of the tile lie beyond the end of the piece. */
				float du = (t[2] - t[0]) * 0.5f, dv = (t[3] - t[1]) * 0.5f;
				nearest = std::min(nearest, segment(u, v, t[0] - du, t[1] - dv, t[2] + du, t[3] + dv));
			}
		} else {
			/* From the middle of the tile to the middle of each edge with a road bit: NW, SW, SE, NE. */
			static constexpr float ENDS[4][2] = {{0.5f, 0}, {1, 0.5f}, {0.5f, 1}, {0, 0.5f}};
			for (uint bit = 0; bit < 4; bit++) {
				if (!HasBit(bits, bit)) continue;
				nearest = std::min(nearest, segment(u, v, 0.5f, 0.5f, ENDS[bit][0], ENDS[bit][1]));
			}
		}
		return nearest;
	};

	/* Shares of the textures at a point, from 0 to 1. */
	auto shares = [&](float u, float v, float &land, float &asphalt, float &gravel, float &paving) {
		float d = distance(u, v, bits);
		land = asphalt = gravel = paving = 0;
		switch (kind) {
			case PatternKind::Rail:
				/* A little wider than the bed of the 3D track, so it does not end sharply against the grass. */
				gravel = d < 0.27f ? 1.0f : (d < 0.36f ? 0.5f : 0.0f);
				land = 1 - gravel;
				break;
			case PatternKind::Road:
				asphalt = d < 0.29f ? 1.0f : 0.0f;
				/* A narrow strip of trodden earth along the edge of the road. */
				gravel = asphalt == 0 && d < 0.33f ? 0.6f : 0.0f;
				land = 1 - asphalt - gravel;
				break;
			case PatternKind::PavedRoad:
				/* As wide as the road between the 3D kerbs. */
				asphalt = d < 0.42f ? 1.0f : 0.0f;
				paving = 1 - asphalt;
				break;
			case PatternKind::Crossing: {
				/* The road goes across the crossing as wide as a plain road; the track bed runs across the other way. */
				asphalt = d < 0.29f ? 1.0f : 0.0f;
				float across = (bits & 0b1010) != 0 ? std::abs(u - 0.5f) : std::abs(v - 0.5f);
				gravel = asphalt == 0 && across < 0.27f ? 1.0f : 0.0f;
				land = 1 - asphalt - gravel;
				break;
			}
			case PatternKind::Gravel:
				gravel = 1;
				break;
			case PatternKind::Paving:
				paving = 1;
				break;
		}
	};

	/* Straight roads have a dashed line down the middle, two dashes per tile so they line up with the next tile. */
	static constexpr uint8_t ROAD_X = 0b1010, ROAD_Y = 0b0101; // NE and SW, NW and SE
	const bool lined = (kind == PatternKind::Road || kind == PatternKind::PavedRoad) && (bits == ROAD_X || bits == ROAD_Y);
	auto marking = [&](float u, float v) {
		/* Reserved tracks: a pale edge either side of the 3D track bed, like the 2D game lights up the ballast. */
		if (reserved != 0) {
			float d = distance(u, v, reserved);
			return d > 0.24f && d < 0.31f ? 0.7f : 0.0f;
		}
		if (!lined) return 0.0f;
		float across = bits == ROAD_X ? v : u;
		float along = bits == ROAD_X ? u : v;
		float dash = std::abs(along - 0.5f);
		return std::abs(across - 0.5f) < 0.018f && dash > 0.12f && dash < 0.38f ? 1.0f : 0.0f;
	};

	/* Sixteen samples per pixel smooth the edges. */
	static constexpr uint SAMPLES = 4;
	const uint size = this->size;
	std::vector<uint32_t> texture(static_cast<size_t>(size) * size, 0);
	std::vector<uint32_t> mix(static_cast<size_t>(size) * size);
	for (uint ty = 0; ty < size; ty++) {
		for (uint tx = 0; tx < size; tx++) {
			float sum[4] = {};
			float paint = 0;
			for (uint sy = 0; sy < SAMPLES; sy++) {
				for (uint sx = 0; sx < SAMPLES; sx++) {
					float u = (tx + (sx + 0.5f) / SAMPLES) / size, v = (ty + (sy + 0.5f) / SAMPLES) / size;
					float share[4];
					shares(u, v, share[0], share[1], share[2], share[3]);
					float line = marking(u, v);
					paint += line;
					for (uint i = 0; i < 4; i++) sum[i] += share[i] * (1 - line);
				}
			}
			auto byte = [&](float value) { return static_cast<uint32_t>(std::clamp(value * 255.0f / (SAMPLES * SAMPLES) + 0.5f, 0.0f, 255.0f)); };
			size_t i = static_cast<size_t>(ty) * size + tx;
			mix[i] = (byte(sum[0]) << GroundMix::LAND) | (byte(sum[1]) << GroundMix::ASPHALT) | (byte(sum[2]) << GroundMix::GRAVEL) | (byte(sum[3]) << GroundMix::PAVING);
			/* The paint, off-white, multiplied by how much of the pixel it covers. */
			uint32_t grey = byte(paint * 0.86f);
			texture[i] = (byte(paint) << 24) | (grey << 16) | (grey << 8) | grey;
		}
	}

	it->second = this->next_layer++;
	this->pending.push_back({it->second, std::move(texture), std::move(mix)});
	return it->second;
}

/**
 * Get the texture for a tile with certain sprites on its ground, making it when it does not exist yet.
 * @param sprites The ground sprites, in the order the 2D game draws them, relative to where it draws the northern corner of the tile.
 * @param heights Heights of the northern, western, eastern and southern corner, in height levels above the lowest.
 * @return The layer of the texture, or GROUND_TEXTURE_NONE when there is no room or nothing to see.
 */
uint GroundTextures::Get(std::span<const SpritePart> sprites, const std::array<uint8_t, 4> &heights)
{
	auto [it, inserted] = this->layers.try_emplace(Key{heights, {sprites.begin(), sprites.end()}}, GROUND_TEXTURE_NONE);
	if (inserted && this->next_layer < this->max_layers && this->Build(this->next_layer, sprites, heights)) {
		it->second = this->next_layer++;
	}
	return it->second;
}

/**
 * Make the fixed ground textures from the sprites that are loaded right now.
 * @param max_layers Number of layers the texture array has room for.
 */
GroundTextures::GroundTextures(uint max_layers) : max_layers(max_layers)
{
	/* Use the most detailed sprites the game is allowed to use; base sets without them get scaled up by the sprite loader. */
	this->zoom = std::min(_settings_client.gui.zoom_min, ZoomLevel::Normal);
	/* At the most detailed zoom level a tile is 256 pixels wide, but textures of that size for every kind of tile take too much memory. */
	this->size = std::min(UnScaleByZoom(256, this->zoom), 128);

	this->pending.push_back({GROUND_TEXTURE_NONE, std::vector<uint32_t>(static_cast<size_t>(this->size) * this->size, 0xFFFFFFFF)});
	auto build = [&](uint layer, std::initializer_list<SpriteID> sprites, const std::array<uint8_t, 4> &heights = {}) {
		std::vector<SpritePart> parts;
		for (SpriteID sprite : sprites) parts.push_back({.sprite = sprite, .pal = PAL_NONE});
		/* Textures whose sprites cannot be used are plain white. */
		if (!this->Build(layer, parts, heights)) this->pending.push_back({layer, this->pending.front().pixels});
	};

	static constexpr SpriteID GRASS[] = {SPR_FLAT_BARE_LAND, SPR_FLAT_1_THIRD_GRASS_TILE, SPR_FLAT_2_THIRD_GRASS_TILE, SPR_FLAT_GRASS_TILE};
	static constexpr SpriteID ROUGH[] = {SPR_FLAT_ROUGH_LAND, SPR_FLAT_ROUGH_LAND_1, SPR_FLAT_ROUGH_LAND_2, SPR_FLAT_ROUGH_LAND_3, SPR_FLAT_ROUGH_LAND_4};
	static constexpr SpriteID SNOW_DESERT[] = {SPR_FLAT_1_QUART_SNOW_DESERT_TILE, SPR_FLAT_2_QUART_SNOW_DESERT_TILE, SPR_FLAT_3_QUART_SNOW_DESERT_TILE, SPR_FLAT_SNOW_DESERT_TILE};
	static constexpr SpriteID SNOWY_ROCKS[] = {SPR_OVERLAY_ROCKS_1_QUART_SNOW, SPR_OVERLAY_ROCKS_2_QUART_SNOW, SPR_OVERLAY_ROCKS_3_QUART_SNOW, SPR_OVERLAY_ROCKS_FULL_SNOW};

	for (uint i = 0; i < std::size(GRASS); i++) build(GROUND_TEXTURE_GRASS + i, {GRASS[i]});
	for (uint i = 0; i < std::size(ROUGH); i++) build(GROUND_TEXTURE_ROUGH + i, {ROUGH[i]});
	build(GROUND_TEXTURE_ROCKS, {SPR_FLAT_ROCKY_LAND_1});
	/* The sprites of the farm field kinds follow each other, one per slope. */
	for (uint i = 0; i < 9; i++) build(GROUND_TEXTURE_FIELDS + i, {SPR_FARMLAND_BARE + i * 19});
	for (uint i = 0; i < std::size(SNOW_DESERT); i++) {
		build(GROUND_TEXTURE_SNOW_DESERT + i, {SNOW_DESERT[i]});
		build(GROUND_TEXTURE_SNOWY_ROCKS + i, {SNOW_DESERT[i], SNOWY_ROCKS[i]});
		build(GROUND_TEXTURE_DESERT_ROCKS + i, {SNOW_DESERT[i], SPR_OVERLAY_ROCKS_BASE});
	}
	build(GROUND_TEXTURE_WATER, {SPR_FLAT_WATER_TILE});

	/* The slope that each shore sprite is drawn for; the reverse of the table in DrawShoreTile(). */
	static constexpr Slope SHORE_SLOPES[] = {
		SLOPE_STEEP_S, SLOPE_W, SLOPE_S, SLOPE_SW, SLOPE_E, SLOPE_STEEP_W, SLOPE_SE, SLOPE_WSE, SLOPE_N,
		SLOPE_NW, SLOPE_STEEP_N, SLOPE_NWS, SLOPE_NE, SLOPE_ENW, SLOPE_SEN, SLOPE_STEEP_E, SLOPE_EW, SLOPE_NS,
	};
	for (uint i = 0; i < std::size(SHORE_SLOPES); i++) {
		Slope slope = SHORE_SLOPES[i];
		auto height = [&](Slope bit, Corner corner) -> uint8_t {
			if (IsSteepSlope(slope) && GetHighestSlopeCorner(slope) == corner) return 2;
			return (slope & bit) != 0 ? 1 : 0;
		};
		build(GROUND_TEXTURE_SHORE + i, {SPR_SHORE_BASE + i}, {height(SLOPE_N, Corner::N), height(SLOPE_W, Corner::W), height(SLOPE_E, Corner::E), height(SLOPE_S, Corner::S)});
	}

}
