$input v_texcoord0

/*
 * Fragment shader for the rain and snow of the OpenTTD 3D viewport: soft streaks and soft round flakes.
 */

#include <bgfx_shader.sh>

/* x: half the width of a drop in world units. y: half its length. z: 1 for snow, 0 for rain. w: seconds. */
uniform vec4 u_dropShape;
/* rgb: colour of the drops. a: how see-through they are at most. */
uniform vec4 u_dropColour;

void main()
{
	/* A streak is brightest along its middle and fades towards its ends; a flake fades from its middle. */
	float streak = (1.0 - abs(v_texcoord0.x)) * (1.0 - v_texcoord0.y * v_texcoord0.y);
	float flake = 1.0 - smoothstep(0.35, 1.0, length(v_texcoord0.xy));
	float alpha = mix(streak, flake, u_dropShape.z) * v_texcoord0.z * u_dropColour.a;
	gl_FragColor = vec4(u_dropColour.rgb, alpha);
}
