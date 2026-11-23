"""Prepares the surface model of a city for the panorama.

From the grids of fetch_lidar.py (highest and lowest return per cell) and the
building outlines from OpenStreetMap it writes:
  <output>_height.npy       surface in meters, gaps filled, water at lake level
  <output>_ground.npy       height of the ground below
  <output>_buildings.npy    number of the building per cell, 0 = none
  <output>_buildings.json   for each number name, height, levels, year of construction
Towers that did not exist yet at the time of the lidar survey are added from
their outlines and height tags.

Usage: city_mesh.py <grid without extension> <outlines.json> <output without extension>
"""
import json
import math
import re
import sys

import cv2
import numpy as np
from scipy import ndimage

R = 6378137.0


def number(text):
    if text is None:
        return None
    m = re.match(r'\s*([0-9.]+)\s*(m|ft|\')?', str(text))
    if not m:
        return None
    v = float(m.group(1))
    return v * 0.3048 if m.group(2) in ('ft', "'") else v


def main():
    src, outlines, out = sys.argv[1:4]
    top = np.load(src + '_top.npy')
    low = np.load(src + '_low.npy')
    _, north, cell, west, south, k = (float(v) for v in open(src + '.txt').read().split())
    ny, nx = top.shape
    ax0 = math.radians(west) * R
    ay0 = R * math.log(math.tan(math.pi / 4 + math.radians(south) / 2))

    def to_grid(lon, lat):
        x = (math.radians(lon) * R - ax0) * k / cell
        y = ny - (R * math.log(math.tan(math.pi / 4 + math.radians(lat) / 2)) - ay0) * k / cell
        return x, y

    have = top > -1000
    # Lake level: the median of the lowest returns in the eastern fifth, where there is only water.
    east = low[:, int(nx * 0.8):]
    lake = float(np.median(east[east < 1000]))
    print(f'lake level {lake:.2f} m, filled {have.mean() * 100:.1f} %', flush=True)

    # Remove single outliers far above their surroundings (birds, cranes); masts that span several cells stay.
    filled = np.where(have, top, lake).astype(np.float32)
    around = cv2.dilate(filled, np.array([[1, 1, 1], [1, 0, 1], [1, 1, 1]], np.uint8))
    spike = filled - around > 12
    filled[spike] = around[spike]
    print(f'{int(spike.sum())} single outliers removed', flush=True)

    # Close small gaps on roofs and streets from their surroundings; large gaps are water.
    known = have.copy()
    value = np.where(known, filled, -1e4).astype(np.float32)
    for _ in range(4):
        grown = cv2.dilate(value, np.ones((3, 3), np.uint8))
        near = cv2.dilate(known.astype(np.uint8), np.ones((3, 3), np.uint8)) > 0
        take = ~known & near
        value[take] = grown[take]
        known |= take
    # What is still open after that is water (surfaces barely above the lake are dropped later, in city_surface.py).
    water = ~known
    height = np.where(known, value, lake).astype(np.float32)

    # Ground: the lowest return in the surroundings, smoothed.
    base = np.where(low < 1000, low, lake).astype(np.float32)
    span = int(round(40 / cell)) | 1
    ground = cv2.erode(base, np.ones((span, span), np.uint8))
    ground = cv2.blur(ground, (span * 2 + 1, span * 2 + 1))
    ground = np.minimum(ground, height)
    ground[water] = lake

    # Buildings from the outlines.
    data = json.load(open(outlines, encoding='utf-8'))
    building = np.zeros((ny, nx), np.int32)
    info = {}
    parts = []
    for el in data['elements']:
        tags = el.get('tags', {})
        rings = []
        if el['type'] == 'way' and 'geometry' in el:
            rings.append(el['geometry'])
        elif el['type'] == 'relation':
            for m in el.get('members', []):
                if m.get('role') in ('outer', '') and 'geometry' in m:
                    rings.append(m['geometry'])
        polys = []
        for ring in rings:
            if len(ring) < 3:
                continue
            pts = np.array([to_grid(p['lon'], p['lat']) for p in ring], np.float64)
            polys.append(np.round(pts).astype(np.int32))
        if not polys:
            continue
        h = number(tags.get('height')) or number(tags.get('building:height'))
        levels = number(tags.get('building:levels'))
        if 'building:part' in tags and 'building' not in tags:
            lo = number(tags.get('min_height')) or 0.0
            if h is None and levels:
                h = levels * 3.9
            if h:
                allp = np.concatenate(polys)
                parts.append((polys, h, lo, allp.min(0), allp.max(0)))
            continue
        idx = len(info) + 1
        cv2.fillPoly(building, polys, idx)
        year = number(tags.get('start_date', '')[:4]) if tags.get('start_date') else None
        allp = np.concatenate(polys)
        info[idx] = {'name': tags.get('name', ''), 'height': h, 'levels': levels, 'year': year, 'kind': tags.get('building', ''),
                     'polys': polys, 'lo': allp.min(0), 'hi': allp.max(0)}

    # Height above the ground according to the outline tags: building parts with their own height, otherwise the whole outline.
    told = np.zeros((ny, nx), np.float32)

    def stamp(polys, lo, hi, value):
        x0, y0 = np.clip(lo - 1, 0, [nx - 1, ny - 1])
        x1, y1 = np.clip(hi + 2, 1, [nx, ny])
        if x1 - x0 < 2 or y1 - y0 < 2:
            return
        c = np.zeros((y1 - y0, x1 - x0), np.uint8)
        cv2.fillPoly(c, [q - np.array([x0, y0]) for q in polys], 1)
        w = told[y0:y1, x0:x1]
        np.maximum(w, np.where(c > 0, value, 0).astype(np.float32), out=w)

    for polys, ph, lo, plo, phi in parts:
        stamp(polys, plo, phi, ph)
    for b in info.values():
        h = b['height'] or ((b['levels'] or 0) * 3.9)
        if h:
            x0, y0 = np.clip(b['lo'], 0, [nx - 1, ny - 1])
            x1, y1 = np.clip(b['hi'] + 1, 1, [nx, ny])
            if not (told[y0:y1, x0:x1] > 0).any():
                stamp(b['polys'], b['lo'], b['hi'], h)

    # Towers that are missing from the lidar survey or were still under construction then: there the tagged height is much greater than the model.
    rel = height - ground
    added = []
    skip = ('cadillac palace', 'fire department', 'board of trade')
    for idx, b in info.items():
        x0, y0 = np.clip(b['lo'] - 2, 0, [nx - 1, ny - 1])
        x1, y1 = np.clip(b['hi'] + 3, 1, [nx, ny])
        if x1 - x0 < 4 or y1 - y0 < 4:
            continue
        win = (slice(y0, y1), slice(x0, x1))
        mask = building[win] == idx
        if mask.sum() < 50:
            continue
        want = float(np.percentile(told[win][mask], 90))
        if want < 80 or any(k in (b['name'] or '').lower() for k in skip):
            continue
        there = float(np.percentile(rel[win][mask], 90))
        if there < 0.45 * want:
            added.append((b['name'], want, there))
            b['added'] = True
            hw, gw, tw = height[win], ground[win], told[win]
            sel = mask & (tw > 0)
            hw[sel] = np.maximum(hw[sel], gw[sel] + tw[sel])
    for name, h, there in sorted(added, key=lambda a: -a[1]):
        print(f'  added: {name[:40]:40s} {h:6.1f} m (in the lidar survey {there:5.1f} m)', flush=True)

    # Outlines of tall buildings are often traced from the roof in an aerial
    # image and are then offset from the building. Each raised cell therefore
    # gets the number of the nearest outline close by; raised cells far from
    # any outline get a number of their own.
    raised = (height - ground) > 6
    dist, (iy, ix) = ndimage.distance_transform_edt(building == 0, return_indices=True)
    near = raised & (building == 0) & (dist < 30 / cell)
    building[near] = building[iy[near], ix[near]]
    del dist, iy, ix
    rest, count = ndimage.label(raised & (building == 0))
    first = len(info) + 1
    building[rest > 0] = rest[rest > 0] + first - 1
    print(f'{int(near.sum())} cells assigned to an outline, {count} structures without an outline', flush=True)

    np.save(out + '_height.npy', height)
    np.save(out + '_ground.npy', ground)
    np.save(out + '_buildings.npy', building)
    for b in info.values():
        del b['polys'], b['lo'], b['hi']
    json.dump({'lake': lake, 'buildings': info}, open(out + '_buildings.json', 'w', encoding='utf-8'), ensure_ascii=False)
    for name in ('_height', '_ground', '_buildings'):
        open(out + name + '.txt', 'w').write(f'0 {north} {cell}\n')
    print('done:', len(info), 'buildings', flush=True)


main()
