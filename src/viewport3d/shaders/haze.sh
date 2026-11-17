/*
 * Haze in the distance for the OpenTTD 3D viewport: far away things fade into the colour of the sky at the horizon.
 */

/* rgb: colour of the haze. */
uniform vec4 u_hazeColour;
/* x: distance where the haze starts. y: one over the distance over which it thickens. z: how thick it gets at most. */
uniform vec4 u_hazeParams;

/**
 * How much haze there is between the camera and a point; for vertex shaders.
 * @param world The point in world units.
 * @return 0 for none, up to u_hazeParams.z.
 */
float HazeAmount(vec4 world)
{
	float distance = length(mul(u_view, world).xyz);
	return clamp((distance - u_hazeParams.x) * u_hazeParams.y, 0.0, 1.0) * u_hazeParams.z;
}

/**
 * Add haze to a colour; for fragment shaders.
 */
vec3 ApplyHaze(vec3 colour, float amount)
{
	return mix(colour, u_hazeColour.rgb, amount);
}
