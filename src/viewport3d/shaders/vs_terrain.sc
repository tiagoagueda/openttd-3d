$input a_position, a_color0
$output v_color0

/*
 * Terrain vertex shader for the OpenTTD 3D viewport.
 * Positions are in tiles and height levels; the model matrix scales them to world units.
 * Lighting is already baked into the vertex colour.
 */

#include <bgfx_shader.sh>

void main()
{
	gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
	v_color0 = a_color0;
}
