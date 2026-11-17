$input a_position
$output v_texcoord0

/*
 * Vertex shader for the sky of the OpenTTD 3D viewport: one triangle that covers the screen, at the far end of the depth.
 */

#include <bgfx_shader.sh>

void main()
{
	gl_Position = vec4(a_position.xy, 1.0, 1.0);
	v_texcoord0 = vec3(a_position.xy, 0.0);
}
