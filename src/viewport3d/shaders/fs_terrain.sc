$input v_color0, v_texcoord0, v_texcoord1, v_haze, v_tile

/*
 * Terrain fragment shader for the OpenTTD 3D viewport.
 *
 * The ground comes from one of two texture arrays. s_terrain has the textures of the model package for the
 * landscape, each spread over several tiles. s_ground has textures made from the sprites of the 2D game, one tile each:
 * everything that is built on the ground and, without textures in the model package, the landscape too.
 * With textures in the model package, the textures made from sprites are mixed with those: s_mix says how much of the
 * landscape, asphalt, gravel and paving to add to the colour of the sprites, which is multiplied by how much it covers.
 * The alpha of the vertex colour then names the landscape.
 * Water moves with small waves, and foam washes along the edge of the water on coasts.
 */

#include <bgfx_shader.sh>

SAMPLER2DARRAY(s_ground, 0);
SAMPLER2DARRAY(s_terrain, 2);
SAMPLER2DARRAY(s_mix, 3);
#include "shadow.sh"
#include "haze.sh"

/* x: layer of the water texture. y: first layer of the coasts made from sprites. z: one past their last layer. w: seconds, for the movement. */
uniform vec4 u_water;
/* x: number of layers that are in s_terrain, 0 without textures in the model package. y: one divided by the tiles per texture.
 * z: layer of the beach. w: layer of the water over coasts. */
uniform vec4 u_terrain;
/* x: first layer of s_ground that is mixed with the textures of s_terrain through s_mix. y: layer of asphalt in s_terrain, followed by gravel and paving.
 * z: the light of flat ground at half brightness, which the colours of sprites already have. */
uniform vec4 u_layers;

/* Layer of full grass, which the beach turns into higher up. */
#define LAYER_GRASS 4.0

/* Whether a colour of a ground texture is water. */
float IsWaterColour(vec3 c)
{
	return step(c.r + 0.06, c.b) * step(c.g, c.b + 0.02);
}

/* A texture of the model package at a place on the map, in tiles. */
vec3 Terrain(vec2 at, float layer)
{
	return texture2DArray(s_terrain, vec3(at * u_terrain.y, layer)).rgb;
}

/* The average colour of a texture of the model package: its smallest version. */
vec3 TerrainAverage(float layer)
{
	return texture2DArrayLod(s_terrain, vec3(0.5, 0.5, layer), 16.0).rgb;
}

#include "weather_terrain.sh"

/* Water of the model package: two copies of the texture drift across each other. */
vec3 Water(vec2 at, float t)
{
	float layer = u_water.x;
	vec3 a = Terrain(at + t * vec2(0.045, 0.018), layer);
	vec3 b = Terrain(vec2(at.y, -at.x) * 1.37 + vec2(1.7, 0.4) - t * vec2(0.022, 0.050), layer);
	return max(a + b - TerrainAverage(layer), vec3_splat(0.0));
}

void main()
{
	float layer = floor(v_texcoord0.z + 0.5);
	vec2 at = v_texcoord0.xy;
	float t = u_water.w;
	float alpha = 1.0;
	vec3 ground;
	/* How much snow can lie here. */
	float land = 1.0;

	/* The same texture, looked at from much further away, makes some regions lighter and others darker,
	 * so the eye does not find the texture repeating. */
	float region = 1.0;
	if (u_terrain.x > 0.5) {
		vec2 far = vec2(at.x * 0.8 + at.y * 0.6, at.y * 0.8 - at.x * 0.6) * 0.113;
		region = dot(Terrain(far, LAYER_GRASS), vec3_splat(1.0)) / max(dot(TerrainAverage(LAYER_GRASS), vec3_splat(1.0)), 0.01);
		region = mix(1.0, region, 0.55);
	}

	/* Textures of s_ground cover one tile each and start over at its edge, so filtering would mix in the far side of the tile
	 * and draw its outline. Stay away from the edge, further when the texture is drawn smaller.
	 * With anti-aliasing, pixels on the edge get a position just outside the tile; the position within the tile shows that, where fract() would start over. */
	float margin = min(max(0.008, 1.5 * max(fwidth(at.x), fwidth(at.y))), 0.5);
	vec2 inside = at - v_tile.xy + clamp(v_tile.xy, vec2_splat(margin), vec2_splat(1.0 - margin));

	bool shallows = u_terrain.x > 0.5 && abs(layer - u_terrain.w) < 0.5;
	if (layer > u_layers.x - 0.5) {
		vec4 sprites = texture2DArray(s_ground, vec3(inside, layer));
		vec4 share = texture2DArray(s_mix, vec3(inside, layer));
		float under = floor(v_color0.a * 255.0 + 0.5);
		ground = share.r * Terrain(at, under) * region
			+ share.g * Terrain(at, u_layers.y) * mix(1.0, region, 0.4)
			+ share.b * Terrain(at, u_layers.y + 1.0) * mix(1.0, region, 0.4)
			+ share.a * Terrain(at, u_layers.y + 2.0);
		/* The sprites are shaded for their slope already; the vertex colour lights the tile for its slope as well. */
		ground += sprites.rgb * (u_layers.z / max(v_color0.g, 0.05));
		land = 0.6 * (1.0 - IsWaterColour(ground));
	} else if (shallows || layer < u_terrain.x - 0.5) {

		if (shallows || abs(layer - u_water.x) < 0.5) {
			/* How deep the water is: the vertices hold the height of the ground under it.
			 * The edge of the water wanders a little from the straight line across the tile, and the surface goes up and down. */
			float depth = 1.0 - v_color0.a * 255.0 / 64.0;
			land = 0.0;
			float wander = Terrain(at * 2.7, LAYER_GRASS).g / max(TerrainAverage(LAYER_GRASS).g, 0.01) - 1.0;
			float surge = sin(t * 1.3 + (at.x + at.y) * 2.1) * sin(t * 0.7 + (at.x - at.y) * 1.3);
			float near_land = 1.0 - smoothstep(0.5, 0.9, depth);
			depth += (0.45 * wander + 0.03 * surge) * near_land;
			if (shallows) {
				if (depth <= 0.0) discard;
			} else {
				depth = max(depth, 0.03);
			}
			float waves = 0.5 * sin(dot(at, vec2(5.3, 2.1)) + t * 1.1) + 0.3 * sin(dot(at, vec2(-3.7, 7.9)) + t * 1.6);
			ground = Water(at, t) * (1.0 + 0.05 * waves);
			/* Clear and greenish at the edge, where the sand shows through, and solid further out. */
			float deep = smoothstep(0.0, 0.6, depth);
			ground = mix(ground * vec3(0.9, 1.25, 1.1) + vec3(0.05, 0.12, 0.10), ground, deep);
			alpha = mix(0.4, 1.0, deep);
			/* Foam where the water meets the land, in bits and pieces that come and go. */
			float bits = Terrain(at * 5.3 + vec2(t * 0.07, -t * 0.04), u_water.x).b / max(TerrainAverage(u_water.x).b, 0.01);
			float foam = (1.0 - smoothstep(0.0, 0.10 + 0.04 * surge, depth)) * smoothstep(0.75, 1.1, bits + 0.35 * (1.0 - smoothstep(0.0, 0.05, depth)));
			ground = mix(ground, vec3(0.95, 0.97, 0.98), foam);
			alpha = max(alpha, foam);
			if (!shallows) {
				/* Open water has no ground below it to show through; make do with the colour of sand. */
				ground = mix(TerrainAverage(u_terrain.z) * 0.8, ground, alpha);
				alpha = 1.0;
			}
		} else if (abs(layer - u_terrain.z) < 0.5) {
			/* A beach: sand near the water, darker where it is wet, and grass from some way up. */
			float height = v_tile.z;
			vec3 sand = Terrain(at, layer) * mix(1.0, region, 0.5);
			sand *= mix(0.8, 1.0, smoothstep(-0.2, 0.14, height));
			vec3 grass = Terrain(at, LAYER_GRASS) * region;
			float patchy = Terrain(at * 2.3, LAYER_GRASS).g / max(TerrainAverage(LAYER_GRASS).g, 0.01) - 1.0;
			ground = mix(sand, grass, smoothstep(0.42, 0.62, height + 0.5 * patchy));
		} else {
			ground = Terrain(at, layer) * region;
		}
	} else {
		bool water = abs(layer - u_water.x) < 0.5;
		bool coast = layer > u_water.y - 0.5 && layer < u_water.z - 0.5;
		if (coast) at = inside;
		ground = texture2DArray(s_ground, vec3(inside, layer)).rgb;

		if (water || coast) {
			float wet = water ? 1.0 : IsWaterColour(ground);
			land = 1.0 - wet;
			/* Waves of a few sizes and directions, moving at their own speeds, lighten and darken the water. */
			float waves = 0.5 * sin(dot(at, vec2(5.3, 2.1)) + t * 1.1) + 0.3 * sin(dot(at, vec2(-3.7, 7.9)) + t * 1.6)
				+ 0.2 * sin(dot(at, vec2(17.3, -11.1)) + t * 2.3) * sin(dot(at, vec2(9.1, 14.7)) - t * 1.9);
			vec3 moving = ground * (1.0 + 0.12 * waves);
			ground = mix(ground, moving, wet);
			if (coast && wet > 0.5) {
				/* Foam where the water meets the land, coming and going. */
				/* Look a little to each side, but not past the edge of the tile, where the texture starts over. */
				float d = 0.05;
				vec2 f = fract(at);
				vec2 lo = at - min(vec2_splat(d), f - 0.002);
				vec2 hi = at + min(vec2_splat(d), 0.998 - f);
				float land = 4.0 - IsWaterColour(texture2DArray(s_ground, vec3(hi.x, at.y, layer)).rgb)
					- IsWaterColour(texture2DArray(s_ground, vec3(lo.x, at.y, layer)).rgb)
					- IsWaterColour(texture2DArray(s_ground, vec3(at.x, hi.y, layer)).rgb)
					- IsWaterColour(texture2DArray(s_ground, vec3(at.x, lo.y, layer)).rgb);
				float surge = 0.55 + 0.45 * sin(t * 1.8 + (at.x + at.y) * 3.0);
				ground = mix(ground, vec3(0.92, 0.95, 0.97), clamp(land * 0.5, 0.0, 1.0) * surge);
			}
		}
	}

	ground = GroundWeather(ground, v_texcoord0.xy, land, v_color0.g * 2.0);

	/* In shadow only the light from all around is left, about half of the light on flat ground. */
	float sun = ShadowVisibility(v_texcoord1);
	gl_FragColor = vec4(ApplyHaze(ground * v_color0.rgb * 2.0 * mix(0.5, 1.0, sun), v_haze.x), alpha);
}
