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
 * Rivers that step down a slope are rapids: the water rushes downhill, streaked with foam.
 */

#include <bgfx_shader.sh>

SAMPLER2DARRAY(s_ground, 0);
SAMPLER2DARRAY(s_terrain, 2);
SAMPLER2DARRAY(s_mix, 3);
#include "shadow.sh"
#include "haze.sh"
#include "light.sh"

/* x: layer of the water texture. y: first layer of the coasts made from sprites. z: one past their last layer. w: seconds, for the movement. */
uniform vec4 u_water;
/* x: number of layers that are in s_terrain, 0 without textures in the model package. y: one divided by the tiles per texture.
 * z: layer of the beach. w: layer of the water over coasts. */
uniform vec4 u_terrain;
/* x: first layer of s_ground that is mixed with the textures of s_terrain through s_mix. y: layer of asphalt in s_terrain, followed by gravel and paving.
 * z: the light of flat ground at half brightness, which the colours of sprites already have. w: world units per height level. */
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

/**
 * Rapids: water that rushes down a step of a river, streaked white with foam that races downstream,
 * and churning most at the foot of the step.
 * @param water The colour of the water.
 * @param at The place on the map, in tiles.
 * @param along How far down the step, 0 at the top to 1 at the foot.
 * @param flow Which way the water flows, in tiles.
 * @param t Seconds, for the movement.
 * @param foam How much foam there is.
 * @return The colour of the rapids.
 */
vec3 Rapids(vec3 water, vec2 at, float along, vec2 flow, float t, out float foam)
{
	float a = dot(at, flow);
	float c = dot(at, vec2(-flow.y, flow.x));
	/* Fine detail fades out where it would be smaller than the pixels. */
	float fine = 1.0 - clamp((max(fwidth(a), fwidth(c)) - 0.02) * 25.0, 0.0, 1.0);
	/* Long streaks that race downstream, a coarse and a fine layer, bubbles, and lumps that churn in place. */
	float coarse = GroundNoise(vec2(c * 16.0, (a - t * 1.8) * 1.6));
	float streaks = mix(0.5, GroundNoise(vec2(c * 34.0 + 5.0, (a - t * 2.6) * 3.0 + 1.3)), fine);
	float bubbles = mix(0.5, GroundNoise(vec2(c, a - t * 1.4) * 34.0 + 0.6 * vec2(sin(t * 3.1), cos(t * 2.7))), fine);
	float churn = GroundNoise(vec2(c * 6.0 + 0.7 * sin(t * 1.7), a * 6.0 - t * 3.2 + 0.7 * cos(t * 1.3)));
	float white = 0.45 * coarse + 0.3 * streaks + 0.1 * bubbles + 0.15 * churn;
	/* Smooth at the lip, more foam further down, and a band of churning white where the water crashes into the river at the foot. */
	float drop = smoothstep(0.05, 0.9, along);
	float foot = smoothstep(0.72, 0.97, along);
	float from = 0.62 - 0.12 * drop - 0.22 * foot;
	foam = smoothstep(from, from + 0.14, white) * smoothstep(0.0, 0.15, along);
	/* The water itself is streaked lighter and darker, with small ripples rushing down with it. */
	float ripple = 0.5 + 0.5 * sin(a * 38.0 - t * 14.0 + 6.0 * coarse);
	vec3 rushing = water * (0.82 + 0.3 * streaks + 0.08 * fine * ripple) + vec3(0.04, 0.06, 0.06) * drop;
	return mix(rushing, vec3(0.93, 0.96, 0.98) * (0.9 + 0.1 * bubbles), foam);
}

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

	/* The markers after the one of the water over coasts are rapids, one for each way the water can flow,
	 * and then the still water at the foot of rapids, one for each way they come from. */
	bool rapids = u_terrain.x > 0.5 && layer > u_terrain.w + 0.5 && layer < u_terrain.w + 4.5;
	bool foot = u_terrain.x > 0.5 && layer > u_terrain.w + 4.5 && layer < u_terrain.w + 8.5;
	bool shallows = u_terrain.x > 0.5 && (abs(layer - u_terrain.w) < 0.5 || rapids || foot);
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
			if (rapids || foot) {
				/* Down along x, along y, up along x or up along y. */
				float k = (layer - u_terrain.w - (rapids ? 1.0 : 5.0)) * 1.5707963;
				vec2 flow = vec2(cos(k), sin(k));
				float along = dot(v_tile.xy - 0.5, flow) + 0.5;
				float white;
				vec3 water = Water(at - flow * t * 0.7, t);
				vec3 white_water = Rapids(water, at, rapids ? along : 0.9, flow, t, white);
				if (rapids) {
					ground = mix(ground, white_water, smoothstep(0.0, 0.12, depth));
					/* Water full of air is hard to see through. */
					alpha = max(alpha, mix(0.8, 1.0, white));
				} else {
					/* The foam is carried on into the still water and falls apart. */
					float spill = (1.0 - smoothstep(0.0, 0.55, along)) * white * smoothstep(0.0, 0.12, depth);
					ground = mix(ground, white_water, spill);
					alpha = max(alpha, spill);
				}
			}
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
			if (water) {
				/* Water on a slope is a river stepping down: rapids. Which way is downhill follows from how the height changes across the screen. */
				vec2 ax = dFdx(at), ay = dFdy(at);
				float zx = dFdx(v_tile.z), zy = dFdy(v_tile.z);
				float det = ax.x * ay.y - ax.y * ay.x;
				vec2 slope = abs(det) > 1e-8 ? vec2(zx * ay.y - zy * ax.y, ax.x * zy - ay.x * zx) / det : vec2_splat(0.0);
				if (dot(slope, slope) > 0.25) {
					vec2 flow = -normalize(slope);
					float white;
					ground = Rapids(ground, at, dot(v_tile.xy - 0.5, flow) + 0.5, flow, t, white);
				}
			}
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

	ground = GroundWeather(ground, v_texcoord0.xy, land, v_color0.g * 2.0, v_tile.z * u_layers.w);

	/* The colour of the vertices has the light of late morning in it. To light the ground for the time of day, find which way
	 * the ground faces from how its position changes across the screen. The baked light makes slopes steeper than they are, so do the same. */
	vec3 world = vec3(v_texcoord0.xy * 16.0, v_tile.z * u_layers.w);
	vec3 normal = cross(dFdx(world), dFdy(world));
	vec3 eye = mul(u_invView, vec4(0.0, 0.0, 0.0, 1.0)).xyz;
	if (dot(normal, eye - world) < 0.0) normal = -normal;
	normal = normalize(vec3(normal.xy * (12.0 / u_layers.w), normal.z));
	/* In shadow only the light from all around is left. */
	float sun = ShadowVisibility(v_texcoord1);
	gl_FragColor = vec4(ApplyHaze(ground * v_color0.rgb * 2.0 * Relight(normal, sun), v_haze.x), alpha);
}
