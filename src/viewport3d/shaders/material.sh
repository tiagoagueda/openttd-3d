/*
 * What the corners of 3D models are made of, for the OpenTTD 3D viewport; for vertex shaders, after light.sh.
 *
 * The alpha of the colour of a corner says what it is made of: 0 takes the colour of the owner, multiplied with its own colour;
 * 48 is a lamp, which glows at night; 96 to 152 is a window and 160 a large pane of glass, which may have light behind them at night;
 * 255 is anything else. Which windows of a building are lit is drawn by lot from where the building stands and the number of
 * the window (the alpha). Behind a large pane are many rooms, so there the fragment shader draws lots again for each stretch of it.
 * An owner colour with an alpha of 2 lights all windows, for vehicles, which do not stay in one place.
 */

/**
 * What a corner is made of, from the alpha of its colour.
 * @param alpha The alpha of the colour.
 * @return x: 1 when it takes the colour of the owner. y: how much it glows when it is a window with light behind it. z: how much it glows as a lamp.
 */
vec3 Material(float alpha)
{
	float a = alpha * 255.0;
	float company = 1.0 - step(24.0, a);
	float lamp = step(24.0, a) * (1.0 - step(80.0, a));
	float window = step(80.0, a) * (1.0 - step(170.0, a));
	return vec3(company, window * u_lightAmbient.w, lamp * u_lightAmbient.w);
}

/**
 * The lot of a window, which decides whether there is light behind it; see WindowLit() in fs_model.sc.
 * @param alpha The alpha of the colour.
 * @param origin Where the model stands.
 * @param all_lit Whether all windows have light behind them.
 * @return -1 for a window that is always lit, 0 to 1 for the lot of a window, 2 to 3 for the lot of a large pane.
 */
float WindowLot(float alpha, vec3 origin, float all_lit)
{
	float a = alpha * 255.0;
	vec2 p = fract(origin.xy * vec2(0.1031, 0.1130)) + vec2(a * 0.137, origin.z * 0.071);
	float lot = fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453) * 0.999;
	return mix(lot + 2.0 * step(156.0, a), -1.0, all_lit);
}
