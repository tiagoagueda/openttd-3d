$input v_color0, v_texcoord0

/*
 * Terrain fragment shader for the OpenTTD 3D viewport.
 */

#include <bgfx_shader.sh>

SAMPLER2DARRAY(s_ground, 0);

void main()
{
	float layer = floor(v_color0.a * 255.0 + 0.5);
	vec3 ground = texture2DArray(s_ground, vec3(v_texcoord0, layer)).rgb;
	gl_FragColor = vec4(ground * v_color0.rgb * 2.0, 1.0);
}
