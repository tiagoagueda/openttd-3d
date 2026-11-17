$input v_color0, v_texcoord0, v_texcoord1

/*
 * Terrain fragment shader for the OpenTTD 3D viewport.
 */

#include <bgfx_shader.sh>

SAMPLER2DARRAY(s_ground, 0);
#include "shadow.sh"

void main()
{
	vec3 ground = texture2DArray(s_ground, vec3(v_texcoord0.xy, floor(v_texcoord0.z + 0.5))).rgb;
	/* In shadow only the light from all around is left, about half of the light on flat ground. */
	float sun = ShadowVisibility(v_texcoord1);
	gl_FragColor = vec4(ground * v_color0.rgb * 2.0 * mix(0.5, 1.0, sun), 1.0);
}
