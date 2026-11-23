# Credits and sources

Sunbeam's own code is under the MIT licence, see [LICENSE](LICENSE). It builds on
the work and data listed here. Please keep these notices when you pass the
program or its data on.

## Methods and code

| Part | Source | Licence |
|---|---|---|
| Volumetric clouds: layers, weather map, shape and detail noise, lighting (`shader/clouds.hlsli`) and the textures in `clouds/` | [three-geospatial](https://github.com/takram-design-engineering/three-geospatial), `packages/clouds`, Copyright (c) 2024 Shota Matsuda | MIT, text in [clouds/LICENSE-three-geospatial.txt](clouds/LICENSE-three-geospatial.txt) |
| Tileable volume noise used for those textures and in `shader/noise.hlsl` | [TileableVolumeNoise](https://github.com/sebh/TileableVolumeNoise), Copyright(c) 2017 Sébastien Hillaire | MIT, text below |
| Water: mean Fresnel term, masking and sun glitter of a rough surface (`meanFresnel`, `lambdaOf`, `glitter` in `shader/ground.hlsli`) | Bruneton, Neyret, Holzschuch: "Real-time Realistic Ocean Lighting using Seamless Transitions from Geometry to BRDF" and its reference implementation, Copyright (c) 2009 INRIA | BSD 3-Clause, text below |
| Hash functions `hash11`, `hash12`, `hash42` | "Hash without Sine", Copyright (c) 2014 David Hoskins | MIT, text below |
| Tone curve `aces` | Krzysztof Narkowicz, "ACES Filmic Tone Mapping Curve" | CC0 |

Further methods, implemented from the publications: multiple scattering in
clouds after Wrenninge and Kulla; slope statistics of the sea after Cox and Munk;
wave spectrum after Pierson and Moskowitz; whitecap coverage after Callaghan et
al. (2008); interleaved gradient noise after Jimenez; positions of sun and moon
from the low-precision series of the Astronomical Almanac.

## Data

| Data | Source | Terms |
|---|---|---|
| Moon map and relief (`images/`) | CGI Moon Kit, <https://svs.gsfc.nasa.gov/4720>. Credit: NASA's Scientific Visualization Studio | Public domain |
| Mountain panorama (`ground/mountains_*.png`): swissALTI3D, swissSURFACE3D Raster, SWISSIMAGE | Source: Federal Office of Topography swisstopo | Free use with reference to the source |
| Chicago skyline (`ground/city_*.png`): lidar, 3D Elevation Program, Cook County 2017 | Credit: U.S. Geological Survey | Public domain |
| Building outlines, names and heights for the skyline | © OpenStreetMap contributors, <https://www.openstreetmap.org/copyright> | Open Database License |
| Live weather (fetched at run time, optional) | [Weather data by Open-Meteo.com](https://open-meteo.com/) | CC BY 4.0 |
| Place search (at run time, when a name is entered) | Open-Meteo geocoding; location data based on [GeoNames](https://www.geonames.org) | CC BY 4.0 |

Details on the ground strips are in [ground/SOURCES.txt](ground/SOURCES.txt), on
the moon in [images/SOURCES.txt](images/SOURCES.txt).

## Models

The ships, the dinghy, the airliner and the gull (`models/`) are the work of
their authors, converted for this program: fewer triangles, smaller textures
with the base colours only, scaled to metres. Further changes are listed in
[models/SOURCES.txt](models/SOURCES.txt).

| Model | Based on | Licence |
|---|---|---|
| Container ship | ["Container Ship"](https://sketchfab.com/3d-models/container-ship-aaa41cca946b4a08bc08cf692b7757be) by [RM02](https://sketchfab.com/RM02) | [CC BY 4.0](http://creativecommons.org/licenses/by/4.0/) |
| Freighter | ["Cargo ship"](https://sketchfab.com/3d-models/cargo-ship-b7c97df584824ca682d26daabf401f87) by [hungry_drifter](https://sketchfab.com/hungry_drifter) | [CC BY 4.0](http://creativecommons.org/licenses/by/4.0/) |
| Sailing dinghy | ["Yacht"](https://sketchfab.com/3d-models/yacht-ae42c1609c25412cbfe40baf9728d987) by [MesXwi](https://sketchfab.com/MesXwi) | [CC BY 4.0](http://creativecommons.org/licenses/by/4.0/) |
| Gull | ["seagull v1"](https://sketchfab.com/3d-models/seagull-v1-f3fa166ff6bd4ffe8133f6f6eed74d64) by [Julian Johnson-Mortimer](https://sketchfab.com/FreddyFoxFreddy) | [CC BY 4.0](http://creativecommons.org/licenses/by/4.0/) |
| Airliner | ["Boeing 737-200 — European Aviation G-CEAH"](https://sketchfab.com/3d-models/boeing-737-200-european-aviation-g-ceah-e3224d0aa8c54f41a612ac74e35e045e) by [mgg8](https://sketchfab.com/mgg8) | [CC0 1.0](http://creativecommons.org/publicdomain/zero/1.0/) |

These models keep their own licences; the MIT licence of the program does not
replace them.

## Licence texts

### MIT (TileableVolumeNoise, Hash without Sine)

Copyright(c) 2017 Sébastien Hillaire
Copyright (c) 2014 David Hoskins

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### BSD 3-Clause (ocean lighting reference implementation)

Real-time Realistic Ocean Lighting using Seamless Transitions from Geometry to BRDF
Copyright (c) 2009 INRIA
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. Neither the name of the copyright holders nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
THE POSSIBILITY OF SUCH DAMAGE.
