$input v_texcoord0

/*
 * Fragment shader for the sky of the OpenTTD 3D viewport: pale and hazy at the horizon, deeper blue above.
 */

#include <bgfx_shader.sh>
#include "haze.sh"

/* From the screen back to the world. */
uniform mat4 u_skyInvViewProj;
/* rgb: colour of the sky high up. */
uniform vec4 u_skyColour;

void main()
{
	vec4 near = mul(u_skyInvViewProj, vec4(v_texcoord0.xy, 0.0, 1.0));
	vec4 far = mul(u_skyInvViewProj, vec4(v_texcoord0.xy, 1.0, 1.0));
	vec3 direction = normalize(far.xyz / far.w - near.xyz / near.w);
	float height = clamp(direction.z, 0.0, 1.0);
	vec3 colour = mix(u_hazeColour.rgb, u_skyColour.rgb, smoothstep(0.0, 0.45, height));
	gl_FragColor = vec4(colour, 1.0);
}
