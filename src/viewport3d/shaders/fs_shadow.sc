/*
 * Fragment shader for the shadow map of the OpenTTD 3D viewport; only the depth matters.
 */

#include <bgfx_shader.sh>

void main()
{
	gl_FragColor = vec4_splat(0.0);
}
