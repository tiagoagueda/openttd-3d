/*
 * The light of the moment for the OpenTTD 3D viewport: the sun by day, the moon and lamps by night.
 */

/* rgb: light from all around. w: how much lamps and lit windows glow, 0 by day to 1 at night. */
uniform vec4 u_lightAmbient;
/* rgb: light from the sun or the moon. w: the share of the windows of buildings that has light behind it. */
uniform vec4 u_lightDirect;
/* xyz: direction towards the sun or the moon. */
uniform vec4 u_lightDirection;

/* The light that the terrain and the sprites of the 2D game have in their colours already: Lighting3D::REFERENCE_*. */
#define REFERENCE_SUN vec3(-0.4683, 0.5620, 0.6817)
#define REFERENCE_AMBIENT 0.45
#define REFERENCE_DIRECT 0.80

/* The colour of a window with light behind it. */
#define WINDOW_GLOW vec3(1.0, 0.84, 0.52)

/**
 * The light on a surface.
 * @param facing How much the surface faces the light, 0 to 1.
 * @param sun How much of the light from the sun or the moon reaches it, 0 in shadow to 1.
 */
vec3 Light(float facing, float sun)
{
	return u_lightAmbient.rgb + u_lightDirect.rgb * (facing * sun);
}

/**
 * What to multiply a colour with that was lit by the reference light, to light it with the light of the moment.
 * @param normal The direction the surface faces.
 * @param sun How much of the light from the sun or the moon reaches it, 0 in shadow to 1.
 */
vec3 Relight(vec3 normal, float sun)
{
	float before = REFERENCE_AMBIENT + REFERENCE_DIRECT * max(dot(normal, REFERENCE_SUN), 0.0);
	return Light(max(dot(normal, u_lightDirection.xyz), 0.0), sun) / before;
}
