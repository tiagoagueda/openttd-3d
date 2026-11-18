$input v_texcoord0

/*
 * Fragment shader for the sky of the OpenTTD 3D viewport: pale and hazy at the horizon, deeper blue above.
 */

#include <bgfx_shader.sh>
#include "haze.sh"

/* From the screen back to the world. */
uniform mat4 u_skyInvViewProj;
/* rgb: colour of the sky high up. w: how well the stars show. */
uniform vec4 u_skyColour;
/* xyz: direction towards the sun. */
uniform vec4 u_skySun;
/* xyz: direction towards the moon. */
uniform vec4 u_skyMoon;

void main()
{
	vec4 near = mul(u_skyInvViewProj, vec4(v_texcoord0.xy, 0.0, 1.0));
	vec4 far = mul(u_skyInvViewProj, vec4(v_texcoord0.xy, 1.0, 1.0));
	vec3 direction = normalize(far.xyz / far.w - near.xyz / near.w);
	float height = clamp(direction.z, 0.0, 1.0);
	vec3 colour = mix(u_hazeColour.rgb, u_skyColour.rgb, smoothstep(0.0, 0.45, height));

	/* Stars: the sky is cut in small cells, and now and then a cell has a star in it. */
	if (u_skyColour.w > 0.0 && direction.z > 0.0) {
		vec3 cell = floor(direction * 170.0);
		float lot = fract(sin(dot(cell, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
		vec3 middle = (cell + vec3_splat(0.5)) / 170.0;
		float star = step(0.985, lot) * (1.0 - smoothstep(0.0010, 0.0032, length(direction - normalize(middle))));
		colour += vec3_splat(star * u_skyColour.w * (0.5 + 30.0 * (lot - 0.985)) * smoothstep(0.0, 0.25, height));
	}

	/* The sun with its glare, and the moon. */
	float to_sun = dot(direction, u_skySun.xyz);
	float low = 1.0 - smoothstep(0.0, 0.5, u_skySun.z);
	vec3 sun_colour = mix(vec3(1.0, 0.97, 0.88), vec3(1.0, 0.62, 0.30), low);
	float above = smoothstep(-0.03, 0.02, direction.z);
	colour += sun_colour * (0.35 * pow(max(to_sun, 0.0), 24.0) * smoothstep(-0.2, 0.1, u_skySun.z)) * above;
	colour = mix(colour, sun_colour, smoothstep(0.9990, 0.9994, to_sun) * above);
	float to_moon = dot(direction, u_skyMoon.xyz);
	colour += vec3(0.5, 0.6, 0.8) * (0.12 * pow(max(to_moon, 0.0), 60.0) * u_skyColour.w);
	colour = mix(colour, vec3(0.92, 0.94, 0.98), smoothstep(0.99945, 0.99965, to_moon) * above * smoothstep(-0.05, 0.05, u_skyMoon.z));
	gl_FragColor = vec4(colour, 1.0);
}
