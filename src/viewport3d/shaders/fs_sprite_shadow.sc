$input v_texcoord0

/*
 * Fragment shader that draws sprites into the shadow map of the OpenTTD 3D viewport; only where the sprite is not transparent.
 */

#include <bgfx_shader.sh>

SAMPLER2D(s_sprites, 0);

void main()
{
	if (texture2D(s_sprites, v_texcoord0.xy).a < 0.5) discard;
	gl_FragColor = vec4_splat(0.0);
}
