$input a_position, a_texcoord0, a_texcoord1
$output v_texcoord0

/*
 * Vertex shader for sprites of the 2D game that stand on the map of the OpenTTD 3D viewport.
 * Positions are in tiles and height levels; the model matrix scales them to world units.
 * a_texcoord0.xy moves the vertex away from that position: x to the right of the camera and
 * y straight up, both in pixels of the 2D game. a_texcoord0.zw is the place in the sprite atlas.
 * a_texcoord1.x is how much nearer to the camera the sprite counts as being when deciding what is in front.
 */

#include <bgfx_shader.sh>

/* xyz: world units to the right of the camera per pixel. w: world units of height per pixel. */
uniform vec4 u_spriteAxes;
/* xyz: direction towards the camera. w: world units towards the camera per unit of a_texcoord1.x. */
uniform vec4 u_spriteNearer;

void main()
{
	vec4 world = mul(u_model[0], vec4(a_position, 1.0));
	world.xyz += u_spriteAxes.xyz * a_texcoord0.x + vec3(0.0, 0.0, u_spriteAxes.w) * a_texcoord0.y;
	gl_Position = mul(u_viewProj, world);
	/* Keep the place on screen, but take the depth of a point that is nearer to the camera. */
	vec4 nearer = mul(u_viewProj, vec4(world.xyz + u_spriteNearer.xyz * (u_spriteNearer.w * a_texcoord1.x), 1.0));
	gl_Position.z = nearer.z / nearer.w * gl_Position.w;
	v_texcoord0 = vec3(a_texcoord0.zw, 0.0);
}
