$input a_position, a_color0
$output v_color0, v_texcoord0

/*
 * Terrain vertex shader for the OpenTTD 3D viewport.
 * Positions are in tiles and height levels; the model matrix scales them to world units.
 * The colour holds the lighting at half brightness, and the ground texture's layer in alpha.
 * Ground textures cover exactly one tile, so the position doubles as texture coordinate.
 */

#include <bgfx_shader.sh>

void main()
{
	gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
	v_color0 = a_color0;
	v_texcoord0 = a_position.xy;
}
