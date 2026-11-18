/*
 * Weather and seasons on the ground of the OpenTTD 3D viewport: grass that changes colour through the year, wet ground and snow.
 * Needs weather.sh, which haze.sh brings along.
 */

/* Smooth noise from 0 to 1, of which the lumps are about one unit across. */
float GroundNoise(vec2 p)
{
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	float a = fract(sin(dot(i, vec2(127.1, 311.7))) * 43758.5453);
	float b = fract(sin(dot(i + vec2(1.0, 0.0), vec2(127.1, 311.7))) * 43758.5453);
	float c = fract(sin(dot(i + vec2(0.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
	float d = fract(sin(dot(i + vec2(1.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

/**
 * The colour of the ground in this weather and season.
 * @param ground The colour of the ground on a clear summer day, before the light.
 * @param at The place on the map, in tiles.
 * @param land How much snow can lie here: 1 on the landscape, less on roads, 0 on water.
 * @param light The light on the ground here, 1 on flat land.
 * @param z The height of the ground in world units; there is more snow higher up.
 */
vec3 GroundWeather(vec3 ground, vec2 at, float land, float light, float z)
{
	/* Only what is green changes with the season. */
	float green = clamp((ground.g - max(ground.r, ground.b)) * 7.0, 0.0, 1.0) * step(0.001, land);
	vec3 grass = ground;
	grass = mix(grass, grass * vec3(0.96, 1.10, 0.86), u_season.z);
	grass = mix(grass, grass * vec3(1.36, 1.08, 0.62), u_season.w);
	grass = mix(grass, grass * vec3(1.12, 0.96, 0.78), 0.8 * u_season.x);
	float grey = dot(grass, vec3(0.30, 0.59, 0.11));
	grass = mix(grass, mix(grass, vec3_splat(grey), 0.35) * vec3(1.06, 0.95, 0.84), u_season.y);
	ground = mix(ground, grass, green);

	/* Snow comes in patches that grow until they meet. */
	float lumps = 0.65 * GroundNoise(at * 0.9) + 0.35 * GroundNoise(at * 3.1 + vec2(7.3, 1.9));
	float snow = smoothstep(0.0, 0.3, SnowCover(z) * 1.3 - lumps) * land;
	/* Snow is so light that slopes would stand out as lighter and darker tiles; take half of that away. */
	vec3 white = SNOW_COLOUR * (0.94 + 0.06 * GroundNoise(at * 9.0)) / sqrt(clamp(light, 0.5, 1.6));

	/* Wet ground is darker. */
	ground *= 1.0 - 0.13 * u_weather.y * step(0.001, land);
	return mix(ground, white, snow);
}
