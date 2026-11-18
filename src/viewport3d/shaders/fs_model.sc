$input v_color0, v_texcoord0, v_texcoord1, v_haze, v_tile

/*
 * Fragment shader for 3D models in the OpenTTD 3D viewport.
 * The colour of the model is multiplied with its texture: v_tile has the place in the texture and its layer in s_modelTextures,
 * or a layer below 0 for parts without a texture.
 */

#include <bgfx_shader.sh>

SAMPLER2DARRAY(s_modelTextures, 0);
#include "shadow.sh"
#include "haze.sh"

void main()
{
	float sun = v_texcoord0.y > 0.0 ? ShadowVisibility(v_texcoord1) : 1.0;
	vec3 surface = texture2DArray(s_modelTextures, vec3(v_tile.xy, max(floor(v_tile.z + 0.5), 0.0))).rgb;
	vec3 colour = v_color0.rgb * mix(vec3_splat(1.0), surface, step(-0.5, v_tile.z));
	gl_FragColor = vec4(ApplyHaze(colour * (v_texcoord0.x + v_texcoord0.y * sun), v_haze.x), v_color0.a);
}
