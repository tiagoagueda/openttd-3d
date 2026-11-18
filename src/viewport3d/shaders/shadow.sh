/*
 * Shadows of the sun for the OpenTTD 3D viewport.
 * The shadow map holds the depth, seen from the sun, of everything that casts a shadow.
 */

SAMPLER2DSHADOW(s_shadow, 1);

/* World to shadow map: xy the place in the map, z the depth to compare with. */
uniform mat4 u_shadowMtx;
/* x: 1 when there are shadows. y: depth bias. z: size of a texel of the map. w: how much clouds take the shadows away. */
uniform vec4 u_shadowParams;

/**
 * How much sun reaches a point.
 * @param coord The point in shadow map coordinates.
 * @return 0 in full shadow, 1 in full sun.
 */
float ShadowVisibility(vec4 coord)
{
	if (u_shadowParams.x < 0.5) return 1.0;
	vec3 c = coord.xyz / coord.w;
	if (c.x <= 0.0 || c.x >= 1.0 || c.y <= 0.0 || c.y >= 1.0 || c.z >= 1.0) return 1.0;
	float depth = c.z - u_shadowParams.y;
	float t = u_shadowParams.z * 0.75;
	/* Four samples soften the edges of shadows. */
	float sun = shadow2D(s_shadow, vec3(c.xy + vec2(-t, -t), depth))
		+ shadow2D(s_shadow, vec3(c.xy + vec2(t, -t), depth))
		+ shadow2D(s_shadow, vec3(c.xy + vec2(-t, t), depth))
		+ shadow2D(s_shadow, vec3(c.xy + vec2(t, t), depth));
	/* Fade out towards the edges of the map, instead of ending in a line. */
	float edge = max(abs(c.x - 0.5), abs(c.y - 0.5)) * 2.0;
	return mix(sun * 0.25, 1.0, max(smoothstep(0.8, 1.0, edge), u_shadowParams.w));
}
