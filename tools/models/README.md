# Model tools

`convert.py` turns the models that move through the image (ships, a dinghy, an airliner, a gull) from glTF into the meshes and colour sheets in `models\` at the repository root. It is only needed to convert them again, for instance after changing `models.json`.

1. Download the models named in `models\SOURCES.txt` from Sketchfab as glTF and unpack each archive into a folder of its own inside one common folder. The folder names are the `source` entries in `models.json`.
2. Run the conversion with Blender 4.x in the background. Without names it converts all models.
   ```
   blender --background --factory-startup --python tools\models\convert.py -- <folder with the sources> [name ...]
   ```

`models.json` holds what the conversion needs to know about each model: which way it faces, its size, the water line of a ship, parts to leave out, how far the mesh is reduced, which materials let light through, and where its lamps are. The keys are described at the top of `convert.py`.

To look at a converted model, put a file `props.txt` into the folder of a `Sunbeam.exe --sky` run; its format is described in `src\check.cpp`.
