$input v_texcoord0, v_texcoord1

/*
 * Fragment shader for sprites of the 2D game that stand on the map of the OpenTTD 3D viewport.
 */

#include <bgfx_shader.sh>

SAMPLER2D(s_sprites, 0);
#include "shadow.sh"

void main()
{
	vec4 colour = texture2D(s_sprites, v_texcoord0.xy);
	if (colour.a < 0.5) discard;
	float sun = ShadowVisibility(v_texcoord1);
	gl_FragColor = vec4(colour.rgb * mix(0.55, 1.0, sun), 1.0);
}
