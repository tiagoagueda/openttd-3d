# OpenGFX3D

3D models for the experimental 3D view of this fork (see `docs/viewport3d.md`).
The 3D view draws a vehicle with a model from this package when the package has one for it.
Everything else keeps the sprites of the 2D game, so the package can grow one model at a time.

All models here are made for this project; do not add models copied from elsewhere without checking their licence.

## Files

* `opengfx3d.json`: the index. It lists which model replaces which vehicles.
* `*.glb`: the models, as binary glTF 2.0 files.
* `make_models.py`: makes the sample models (`bus.glb`, `lorry.glb`) from simple shapes. Run it in this folder after changing it.

The build copies the package to `opengfx3d/` in the build folder. The game looks for a folder of that name in all places where it looks for its data, such as your personal OpenTTD folder.

## The index

```json
{
	"name": "OpenGFX3D",
	"vehicles": [
		{ "type": "road", "ids": [0, 1, 2, 3], "model": "bus.glb" },
		{ "type": "train", "grfid": "44440111", "ids": [12], "model": "engine.glb", "scale": 1.2 }
	]
}
```

* `type`: `train`, `road`, `ship` or `aircraft`.
* `grfid`: the ID of the NewGRF that defines the vehicles, as the eight hexadecimal digits the game shows for it. Leave it out for the vehicles of the base game.
* `ids`: the numbers of the vehicles within that NewGRF. For the base game these count from 0 per type, in the order of `src/table/engines.h`: road vehicle 0 is the MPS Regal Bus.
* `model`: a file in this folder.
* `scale`: optional; world units per unit of the model.

## Models

* Binary glTF 2.0 (`.glb`), as exported by Blender and other programs, with the data inside the file.
* The front of the vehicle points to +Z, up is +Y. The ground is at height 0 and the middle of the vehicle is at the origin.
* One unit is one sixteenth of a tile. A road vehicle is about 9 long, 3 wide and 3.5 high.
* Triangles take the base colour of their material. Textures, animations and skins are not read yet.
* A material with a name that starts with `company` takes the colour of the owner, multiplied with its base colour: white becomes the company colour, grey a darker shade of it.
* Each wagon or part of a vehicle is drawn with the model of its own vehicle ID.
