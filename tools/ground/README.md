# Ground tools

These scripts compute the panorama strips of terrain that Sunbeam shows at the horizon: Swiss mountains from swisstopo data, and the Chicago skyline from USGS lidar and OpenStreetMap building outlines. The finished strips are in `ground\` at the repository root. The scripts are only needed to compute them again.

`panorama.py` writes `a.png`, `b.png`, `c.png` and, for a city, `d.png` into the output folder named in the scene file. The program reads the strips as `ground\mountains_a.png` to `mountains_c.png` and `ground\city_a.png` to `city_d.png`. The scripts do not copy them there. The keys of the scene file are listed at the top of `panorama.py`. `mountains.json` and `city.json` are the scene files of the two strips in the repository; the grid paths in them are examples and assume the downloads in a folder `data` next to the scripts.

## Mountain strip

1. Download the tiles. The collection argument selects one data set per run: `alti` (swissALTI3D), `image` (SWISSIMAGE) or `surface` (swissSURFACE3D Raster).
   ```
   fetch_swiss.py <output folder> <east> <north> <azimuth from> <azimuth to> <km from> <km to> <collection> <resolution>
   ```
2. Assemble the tiles of a folder into one grid.
   ```
   mosaic.py <folder with tiles> <output.npy> [pixels per kilometer]
   ```
3. Compute the strip from a scene file that names the grids.
   ```
   panorama.py <scene.json> [width] [subdivision]
   ```
4. Optionally light the result for checking.
   ```
   preview.py <folder> <output.png> <sun offset from the view direction in degrees> <sun elevation in degrees> [exposure]
   ```

## City strip

1. Download the lidar points and reduce them to a grid of highest and lowest returns.
   ```
   fetch_lidar.py <data set> <west> <south> <east> <north> <depth> <cell in m> <output without extension>
   ```
2. Download the building outlines.
   ```
   fetch_outlines.py <west> <south> <east> <north> <output.json>
   ```
3. Build the surface model, the ground model and the building numbers.
   ```
   city_mesh.py <grid without extension> <outlines.json> <output without extension>
   ```
4. Compute the strip from a scene file with `"city": true`. `panorama.py` imports `city_surface.py` for the facades and night lights; that file is not run on its own.
   ```
   panorama.py <scene.json> [width] [subdivision]
   ```
5. Optionally light the result for checking with `preview.py`, as for the mountain strip.

## Requirements and data

Python 3 with these packages: `requests` (the three fetch scripts), `laspy` with a LAZ backend such as `lazrs` (`fetch_lidar.py`), `numpy` (all scripts except `fetch_swiss.py` and `fetch_outlines.py`), `opencv-python` (`mosaic.py`, `panorama.py`, `city_mesh.py`, `preview.py`) and `scipy` (`city_mesh.py`, `city_surface.py`).

The raw data is not part of the repository. The fetch scripts download it, and the data sets are large. Subfolders of `tools\ground` are ignored by git and can hold the downloads and intermediate files.

Sources: Federal Office of Topography swisstopo, data.geo.admin.ch (free to use with attribution). U.S. Geological Survey, 3D Elevation Program (public domain). © OpenStreetMap contributors, Open Database License.
