$input v_texcoord0

/*
 * Fragment shader for sprites of the 2D game that stand on the map of the OpenTTD 3D viewport.
 */

#include <bgfx_shader.sh>

SAMPLER2D(s_sprites, 0);

void main()
{
	vec4 colour = texture2D(s_sprites, v_texcoord0);
	if (colour.a < 0.5) discard;
	gl_FragColor = vec4(colour.rgb, 1.0);
}
