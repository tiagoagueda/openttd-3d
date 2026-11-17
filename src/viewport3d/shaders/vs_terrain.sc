$input a_position, a_color0, a_texcoord0
$output v_color0, v_texcoord0, v_texcoord1, v_haze, v_tile

/*
 * Terrain vertex shader for the OpenTTD 3D viewport.
 * Positions are in tiles and height levels; the model matrix scales them to world units.
 * The colour holds the lighting at half brightness; a_texcoord0.x is the layer of the ground texture
 * and a_texcoord0.yz the position within the tile.
 * Ground textures cover exactly one tile, so the position doubles as texture coordinate.
 */

#include <bgfx_shader.sh>
#include "haze.sh"

/* World to shadow map. */
uniform mat4 u_shadowMtx;

void main()
{
	gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
	v_color0 = a_color0;
	v_texcoord0 = vec3(a_position.xy, a_texcoord0.x);
	v_tile = a_texcoord0.yz;
	vec4 world = mul(u_model[0], vec4(a_position, 1.0));
	v_texcoord1 = mul(u_shadowMtx, world);
	v_haze = vec2(HazeAmount(world), 0.0);
}
