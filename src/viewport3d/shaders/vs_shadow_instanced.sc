$input a_position, i_data0, i_data1, i_data2, i_data3

/*
 * Vertex shader that draws many copies of a model into the shadow map of the OpenTTD 3D viewport.
 */

#include <bgfx_shader.sh>

void main()
{
	mat4 model = mtxFromCols(i_data0, i_data1, i_data2, i_data3);
	gl_Position = mul(u_viewProj, mul(model, vec4(a_position, 1.0)));
}
