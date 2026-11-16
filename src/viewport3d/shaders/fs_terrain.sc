$input v_color0, v_texcoord0

/*
 * Terrain fragment shader for the OpenTTD 3D viewport.
 */

#include <bgfx_shader.sh>

SAMPLER2DARRAY(s_ground, 0);

void main()
{
	vec3 ground = texture2DArray(s_ground, vec3(v_texcoord0.xy, floor(v_texcoord0.z + 0.5))).rgb;
	gl_FragColor = vec4(ground * v_color0.rgb * 2.0, 1.0);
}
