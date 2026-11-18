$input a_position, a_texcoord0
$output v_texcoord0

/*
 * Vertex shader for the rain and snow of the OpenTTD 3D viewport.
 * Every drop or flake has a place in a cube (a_position, each from 0 to 1) that is repeated all over the world;
 * the cube around the spot the camera looks at is drawn. Drops move through it with the time and come back in at the other side.
 * a_texcoord0.xy is the corner of the drop, each -1 or 1; a_texcoord0.zw are two numbers from 0 to 1 that differ per drop.
 */

#include <bgfx_shader.sh>

/* xyz: middle of the cube in world units. w: length of its sides. */
uniform vec4 u_dropBox;
/* xyz: how far the drops have moved, in sides of the cube. w: how well the drops show, from 0 to 1. */
uniform vec4 u_dropMove;
/* x: half the width of a drop in world units. y: half its length. z: 1 for snow, 0 for rain. w: seconds, for the swaying of snow. */
uniform vec4 u_dropShape;
/* xyz: the way the drops fall, of length 1. */
uniform vec4 u_dropFall;
/* xyz: the way the camera looks, of length 1. */
uniform vec4 u_dropLook;

void main()
{
	vec3 place = u_dropBox.xyz + (fract(a_position + u_dropMove.xyz) - vec3_splat(0.5)) * u_dropBox.w;
	/* Snow sways as it falls. */
	float sway = u_dropShape.z * u_dropBox.w * 0.006;
	place.x += sway * sin(u_dropShape.w * (0.9 + a_texcoord0.z) + a_texcoord0.w * 40.0);
	place.y += sway * cos(u_dropShape.w * (0.7 + a_texcoord0.w) + a_texcoord0.z * 40.0);

	/* Rain is a streak along the way it falls; a flake is a square that faces the camera. */
	vec3 side = normalize(cross(u_dropFall.xyz, u_dropLook.xyz));
	vec3 along = mix(u_dropFall.xyz, normalize(cross(side, u_dropLook.xyz)), u_dropShape.z);
	float size = 0.7 + 0.6 * a_texcoord0.z;
	place += side * (a_texcoord0.x * u_dropShape.x * size) + along * (a_texcoord0.y * u_dropShape.y * size);

	gl_Position = mul(u_viewProj, vec4(place, 1.0));
	/* Drops near the sides of the cube fade out, so the cube does not show. */
	vec3 from_middle = abs(fract(a_position + u_dropMove.xyz) - vec3_splat(0.5)) * 2.0;
	float edge = 1.0 - smoothstep(0.8, 1.0, max(from_middle.x, max(from_middle.y, from_middle.z)));
	v_texcoord0 = vec3(a_texcoord0.xy, edge * u_dropMove.w * (0.6 + 0.4 * a_texcoord0.w));
}
