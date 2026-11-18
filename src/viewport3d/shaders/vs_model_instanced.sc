$input a_position, a_normal, a_color0, a_texcoord0, i_data0, i_data1, i_data2, i_data3, i_data4
$output v_color0, v_texcoord0, v_texcoord1, v_haze, v_tile

/*
 * Vertex shader for many copies of the same 3D model in the OpenTTD 3D viewport, such as trees.
 * Like vs_model, but each copy brings its own model matrix (i_data0 to i_data3) and owner colour (i_data4).
 */

#include <bgfx_shader.sh>
#include "haze.sh"

/* xyz: direction towards the light. */
uniform vec4 u_modelSun;
/* x: light from all around. y: light from that direction. */
uniform vec4 u_modelLight;
/* World to shadow map. */
uniform mat4 u_shadowMtx;

void main()
{
	mat4 model = mtxFromCols(i_data0, i_data1, i_data2, i_data3);
	vec4 world = mul(model, vec4(a_position, 1.0));
	gl_Position = mul(u_viewProj, world);
	vec3 normal = normalize(mul(model, vec4(a_normal, 0.0)).xyz);
	vec3 colour = mix(a_color0.rgb * i_data4.rgb, a_color0.rgb, a_color0.a);
	colour = ModelWeather(colour, a_color0.a, normal, i_data3.xyz, a_position.z, 1.0);
	v_color0 = vec4(colour, 1.0);
	v_texcoord0 = vec3(u_modelLight.x, u_modelLight.y * max(dot(normal, u_modelSun.xyz), 0.0), 0.0);
	v_texcoord1 = mul(u_shadowMtx, world);
	v_haze = vec2(HazeAmount(world), 0.0);
	/* Where in which texture; a layer below 0 is no texture. */
	v_tile = a_texcoord0.xyz;
}
