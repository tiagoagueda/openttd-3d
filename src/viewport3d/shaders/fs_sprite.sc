$input v_texcoord0, v_texcoord1, v_haze

/*
 * Fragment shader for sprites of the 2D game that stand on the map of the OpenTTD 3D viewport.
 */

#include <bgfx_shader.sh>

SAMPLER2D(s_sprites, 0);
#include "shadow.sh"
#include "haze.sh"
#include "light.sh"

void main()
{
	vec4 colour = texture2D(s_sprites, v_texcoord0.xy);
	if (colour.a < 0.5) discard;
	float sun = max(ShadowVisibility(v_texcoord1), v_haze.y);
	/* Sprites have the light of the 2D game painted in; take the light of flat ground at that moment out and put the light of this moment in. */
	float before = REFERENCE_AMBIENT + REFERENCE_DIRECT * REFERENCE_SUN.z;
	vec3 light = mix(u_lightAmbient.rgb * (0.55 / REFERENCE_AMBIENT), Light(REFERENCE_SUN.z, 1.0) / before, sun);
	/* Names and signs stay readable at night. */
	light = mix(light, max(light, vec3_splat(0.8)), v_haze.y);
	gl_FragColor = vec4(ApplyHaze(colour.rgb * light, v_haze.x), 1.0);
}
