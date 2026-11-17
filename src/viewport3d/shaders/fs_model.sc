$input v_color0, v_texcoord0, v_texcoord1, v_haze

/*
 * Fragment shader for 3D models in the OpenTTD 3D viewport.
 */

#include <bgfx_shader.sh>
#include "shadow.sh"
#include "haze.sh"

void main()
{
	float sun = v_texcoord0.y > 0.0 ? ShadowVisibility(v_texcoord1) : 1.0;
	gl_FragColor = vec4(ApplyHaze(v_color0.rgb * (v_texcoord0.x + v_texcoord0.y * sun), v_haze.x), v_color0.a);
}
