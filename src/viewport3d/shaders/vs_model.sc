$input a_position, a_normal, a_color0, a_texcoord0
$output v_color0, v_texcoord0, v_texcoord1, v_haze, v_tile, v_local

/*
 * Vertex shader for 3D models in the OpenTTD 3D viewport.
 * The model matrix puts the model in its place on the map.
 * What a corner is made of is in the alpha of its colour; see material.sh.
 */

#include <bgfx_shader.sh>
#include "haze.sh"
#include "light.sh"
#include "material.sh"

/* rgb: colour of the owner. */
uniform vec4 u_modelTint;
/* World to shadow map. */
uniform mat4 u_shadowMtx;

void main()
{
	gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
	vec3 normal = normalize(mul(u_model[0], vec4(a_normal, 0.0)).xyz);
	vec3 material = Material(a_color0.a);
	vec3 colour = mix(a_color0.rgb, a_color0.rgb * u_modelTint.rgb, material.x);
	colour = ModelWeather(colour, a_color0.a, normal, mul(u_model[0], vec4(0.0, 0.0, 0.0, 1.0)).xyz, a_position.z, step(1.0, u_modelTint.a));
	v_color0 = vec4(colour, min(u_modelTint.a, 1.0));
	/* x: glow of a lit window. y: how much the surface faces the sun or the moon. z: glow of a lamp. */
	v_texcoord0 = vec3(material.y, max(dot(normal, u_lightDirection.xyz), 0.0), material.z);
	vec4 world = mul(u_model[0], vec4(a_position, 1.0));
	v_texcoord1 = mul(u_shadowMtx, world);
	/* y: the lot of a window. */
	v_haze = vec2(HazeAmount(world), WindowLot(a_color0.a, mul(u_model[0], vec4(0.0, 0.0, 0.0, 1.0)).xyz, step(1.5, u_modelTint.a)));
	v_local = a_position;
	/* Where in which texture; a layer below 0 is no texture. */
	v_tile = a_texcoord0.xyz;
}
