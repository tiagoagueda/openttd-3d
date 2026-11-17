$input v_texcoord0, v_texcoord1, v_haze

/*
 * Fragment shader for sprites of the 2D game that stand on the map of the OpenTTD 3D viewport.
 */

#include <bgfx_shader.sh>

SAMPLER2D(s_sprites, 0);
#include "shadow.sh"
#include "haze.sh"

void main()
{
	vec4 colour = texture2D(s_sprites, v_texcoord0.xy);
	if (colour.a < 0.5) discard;
	float sun = max(ShadowVisibility(v_texcoord1), v_haze.y);
	gl_FragColor = vec4(ApplyHaze(colour.rgb * mix(0.55, 1.0, sun), v_haze.x), 1.0);
}
