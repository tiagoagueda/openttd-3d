$input v_color0, v_texcoord0, v_texcoord1, v_haze

/*
 * Terrain fragment shader for the OpenTTD 3D viewport.
 * Water moves with small waves, and foam washes along the edge of the water on coasts.
 */

#include <bgfx_shader.sh>

SAMPLER2DARRAY(s_ground, 0);
#include "shadow.sh"
#include "haze.sh"

/* x: layer of the water texture. y: first layer of the coasts. z: one past their last layer. w: seconds, for the movement. */
uniform vec4 u_water;

/* Whether a colour of a ground texture is water. */
float IsWaterColour(vec3 c)
{
	return step(c.r + 0.06, c.b) * step(c.g, c.b + 0.02);
}

void main()
{
	float layer = floor(v_texcoord0.z + 0.5);
	vec2 at = v_texcoord0.xy;
	vec3 ground = texture2DArray(s_ground, vec3(at, layer)).rgb;

	bool water = abs(layer - u_water.x) < 0.5;
	bool coast = layer > u_water.y - 0.5 && layer < u_water.z - 0.5;
	if (water || coast) {
		float wet = water ? 1.0 : IsWaterColour(ground);
		/* Waves of a few sizes and directions, moving at their own speeds, lighten and darken the water. */
		float t = u_water.w;
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

	/* In shadow only the light from all around is left, about half of the light on flat ground. */
	float sun = ShadowVisibility(v_texcoord1);
	gl_FragColor = vec4(ApplyHaze(ground * v_color0.rgb * 2.0 * mix(0.5, 1.0, sun), v_haze.x), 1.0);
}
