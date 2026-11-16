$input v_color0

/*
 * Terrain fragment shader for the OpenTTD 3D viewport.
 */

#include <bgfx_shader.sh>

void main()
{
	gl_FragColor = vec4(v_color0.rgb, 1.0);
}
