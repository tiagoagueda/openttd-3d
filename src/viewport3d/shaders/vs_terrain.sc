$input a_position, a_color0, a_texcoord0
$output v_color0, v_texcoord0

/*
 * Terrain vertex shader for the OpenTTD 3D viewport.
 * Positions are in tiles and height levels; the model matrix scales them to world units.
 * The colour holds the lighting at half brightness; a_texcoord0.x is the layer of the ground texture.
 * Ground textures cover exactly one tile, so the position doubles as texture coordinate.
 */

#include <bgfx_shader.sh>

void main()
{
	gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
	v_color0 = a_color0;
	v_texcoord0 = vec3(a_position.xy, a_texcoord0.x);
}
