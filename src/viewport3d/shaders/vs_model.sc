$input a_position, a_normal, a_color0
$output v_color0, v_texcoord0, v_texcoord1, v_haze

/*
 * Vertex shader for 3D models in the OpenTTD 3D viewport.
 * The model matrix puts the model in its place on the map.
 * A corner with alpha 0 takes the colour of the owner, multiplied with its own colour.
 */

#include <bgfx_shader.sh>
#include "haze.sh"

/* rgb: colour of the owner. */
uniform vec4 u_modelTint;
/* xyz: direction towards the light. */
uniform vec4 u_modelSun;
/* x: light from all around. y: light from that direction. */
uniform vec4 u_modelLight;
/* World to shadow map. */
uniform mat4 u_shadowMtx;

void main()
{
	gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
	vec3 normal = normalize(mul(u_model[0], vec4(a_normal, 0.0)).xyz);
	vec3 colour = mix(a_color0.rgb * u_modelTint.rgb, a_color0.rgb, a_color0.a);
	v_color0 = vec4(colour, u_modelTint.a);
	/* Light from all around, and light from the sun, which shadows can take away. */
	v_texcoord0 = vec3(u_modelLight.x, u_modelLight.y * max(dot(normal, u_modelSun.xyz), 0.0), 0.0);
	vec4 world = mul(u_model[0], vec4(a_position, 1.0));
	v_texcoord1 = mul(u_shadowMtx, world);
	v_haze = vec2(HazeAmount(world), 0.0);
}
