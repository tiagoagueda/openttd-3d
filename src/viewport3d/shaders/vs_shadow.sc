$input a_position

/*
 * Vertex shader that draws what casts shadows into the shadow map of the OpenTTD 3D viewport.
 */

#include <bgfx_shader.sh>

void main()
{
	gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
}
