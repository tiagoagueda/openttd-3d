$input v_color0, v_texcoord0, v_texcoord1, v_haze, v_tile, v_local

/*
 * Fragment shader for 3D models in the OpenTTD 3D viewport.
 * The colour of the model is multiplied with its texture: v_tile has the place in the texture and its layer in s_modelTextures,
 * or a layer below 0 for parts without a texture.
 */

#include <bgfx_shader.sh>

SAMPLER2DARRAY(s_modelTextures, 0);
#include "shadow.sh"
#include "haze.sh"
#include "light.sh"

/**
 * Whether a window has light behind it.
 * @param local The place in the frame of the model.
 * @param lot The lot of the window, from WindowLot() in material.sh.
 * @return 1 when it is lit.
 */
float WindowLit(vec3 local, float lot)
{
	if (lot < 0.0) return 1.0;
	if (lot >= 2.0) {
		/* A large pane: every stretch of it, about the size of a room, draws its own lot.
		 * The lot of the pane is rounded, as the tiny differences from pixel to pixel would otherwise show as speckles. */
		vec3 room = floor(local * vec3(0.55, 0.55, 0.62) + vec3_splat(0.37));
		lot = fract(sin(dot(room, vec3(12.9898, 78.233, 37.719)) + floor((lot - 2.0) * 64.0 + 0.5) * 7.31) * 43758.5453);
	}
	return step(lot, u_lightDirect.w);
}

void main()
{
	float sun = v_texcoord0.y > 0.0 ? ShadowVisibility(v_texcoord1) : 1.0;
	vec3 surface = texture2DArray(s_modelTextures, vec3(v_tile.xy, max(floor(v_tile.z + 0.5), 0.0))).rgb;
	surface = mix(vec3_splat(1.0), surface, step(-0.5, v_tile.z));
	vec3 colour = v_color0.rgb * surface * Light(v_texcoord0.y, sun);
	/* At night, windows with light behind them and lamps shine by themselves. */
	colour = mix(colour, WINDOW_GLOW * mix(vec3_splat(1.0), surface, 0.5), v_texcoord0.x * WindowLit(v_local, v_haze.y));
	colour = mix(colour, min(v_color0.rgb * surface * 1.3, vec3_splat(1.0)), v_texcoord0.z);
	gl_FragColor = vec4(ApplyHaze(colour, v_haze.x), v_color0.a);
}
