/*
 * Weather and seasons for the OpenTTD 3D viewport: what clouds do to the light, snow on models and the colours of leaves through the year.
 */

/* x: how much snow lies on the ground. y: how wet the ground is. z: how much cloud covers the sky. w: light of a flash of lightning. */
uniform vec4 u_weather;
/* x: how far leaves have turned to autumn colours. y: how far the leaves are gone. z: how fresh the green of spring is. w: how dry the grass is. */
uniform vec4 u_season;

/* x: height of the highest ground around, in world units; 0 when snow does not depend on height. y: how much snow lies on the
 * highest ground even when there is none below, in the cold season. z: height levels per world unit. */
uniform vec4 u_snowHeight;

/* The colour of fresh snow. */
#define SNOW_COLOUR vec3(0.93, 0.95, 0.98)

/**
 * Under clouds everything is a little darker and greyer; lightning lights it up for a moment. For fragment shaders.
 */
vec3 WeatherGrade(vec3 colour)
{
	float grey = dot(colour, vec3(0.30, 0.59, 0.11));
	colour = mix(colour, vec3_splat(grey), 0.28 * u_weather.z) * (1.0 - 0.15 * u_weather.z);
	return colour + (colour * 0.9 + vec3(0.10, 0.10, 0.12)) * u_weather.w;
}

/**
 * How much snow lies at a height: more the higher up, as it is colder there. At five height levels it is the snow of the weather;
 * lower down less and higher up more, up to twice as much at sixteen levels. In the cold season the highest ground around can be
 * white while there is none in the valleys.
 * @param z The height in world units.
 * @return The snow on the ground there, from 0 to 1, like u_weather.x.
 */
float SnowCover(float z)
{
	if (u_snowHeight.x <= 0.0) return u_weather.x;
	float levels = max(z * u_snowHeight.z, 0.0);
	float more = clamp(0.5 + 0.1 * levels, 0.5, 2.2);
	return clamp(u_weather.x * more + u_snowHeight.y * smoothstep(0.6, 1.0, z / u_snowHeight.x), 0.0, 1.0);
}

/**
 * A number from 0 to 1 that is the same for everything of one model on the map, and differs from its neighbours.
 */
float WeatherRandom(vec2 place, float salt)
{
	return fract(sin(dot(floor(place * 4.0), vec2(12.9898, 78.233)) + salt) * 43758.5453);
}

/**
 * The colour of a corner of a model in this weather and season; for vertex shaders.
 * Leaves change colour through the year, each tree at its own time and to its own colour, and snow lies on what faces up.
 * @param colour The colour of the corner.
 * @param kind The alpha of the colour of the corner, which tells leaves from the rest.
 * @param normal Which way the surface faces, in the world.
 * @param origin Where the model stands on the map, in world units.
 * @param height How high the corner is above the foot of the model, in units of the model.
 * @param snow How much snow can lie on this model, from 0 to 1.
 * @param z The height of the corner in the world, in world units.
 */
vec3 ModelWeather(vec3 colour, float kind, vec3 normal, vec3 origin, float height, float snow, float z)
{
	float leaf = step(0.72, kind) * step(kind, 0.91);
	float own = WeatherRandom(origin.xy, 0.0);
	float hue = WeatherRandom(origin.xy, 3.7);

	/* Spring: a lighter, fresher green, and blossom on some trees. */
	vec3 fresh = colour * vec3(1.05, 1.16, 0.80) + vec3(0.02, 0.05, 0.0);
	fresh = mix(fresh, vec3(0.96, 0.82, 0.86), 0.65 * step(0.78, hue));
	/* Autumn: yellow, orange or red, keeping the light and dark of the tree. */
	float light = clamp(dot(colour, vec3(0.30, 0.59, 0.11)) / 0.34, 0.6, 1.5);
	vec3 turned = mix(vec3(0.84, 0.66, 0.14), vec3(0.80, 0.40, 0.10), smoothstep(0.25, 0.60, hue));
	turned = mix(turned, vec3(0.62, 0.17, 0.09), smoothstep(0.70, 0.90, hue)) * light;
	/* Winter: only the twigs are left. */
	vec3 twigs = vec3(0.37, 0.31, 0.26) * mix(0.85, 1.1, hue);

	vec3 leaves = mix(colour, fresh, u_season.z * (0.6 + 0.4 * own));
	leaves = mix(leaves, turned, smoothstep(own * 0.55, own * 0.55 + 0.45, u_season.x));
	leaves = mix(leaves, twigs, smoothstep(own * 0.5, own * 0.5 + 0.5, u_season.y));
	colour = mix(colour, leaves, leaf);

	/* Snow on what faces up, except on what lies on the ground, where the ground has its own snow; this keeps rails and road markings in sight. */
	float lying = smoothstep(0.35, 0.75, normal.z) * smoothstep(0.5, 1.6, height) * smoothstep(0.15, 0.6, SnowCover(z)) * snow;
	return mix(colour, SNOW_COLOUR, 0.92 * lying);
}
