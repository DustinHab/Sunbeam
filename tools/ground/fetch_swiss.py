"""Downloads swisstopo elevation and aerial image tiles for a sector around a viewpoint.

Usage: fetch_swiss.py <output folder> <east> <north> <azimuth from> <azimuth to> <km from> <km to> <collection> <resolution>
East and north in LV95 meters, azimuths in degrees, resolution in meters.
Collection: alti (swissALTI3D), image (SWISSIMAGE), surface (swissSURFACE3D Raster).
Source: Federal Office of Topography swisstopo, data.geo.admin.ch (free to use with attribution).
"""
import concurrent.futures
import math
import os
import re
import sys
import time

import requests

SETS = {
    'alti': 'ch.swisstopo.swissalti3d',
    'image': 'ch.swisstopo.swissimage-dop10',
    'surface': 'ch.swisstopo.swisssurface3d-raster',
}
API = 'https://data.geo.admin.ch/api/stac/v0.9/collections/{}/items'


def to_wgs(e, n):
    y = (e - 2600000) / 1e6
    x = (n - 1200000) / 1e6
    lon = 2.6779094 + 4.728982 * y + 0.791484 * y * x + 0.1306 * y * x * x - 0.0436 * y ** 3
    lat = 16.9023892 + 3.238272 * x - 0.270978 * y * y - 0.002528 * x * x - 0.0447 * y * y * x - 0.0140 * x ** 3
    return lon * 100 / 36, lat * 100 / 36


def in_sector(te, tn, e0, n0, a0, a1, r0, r1):
    """Returns whether a one-kilometer tile (south-west corner te, tn in km) lies in the sector. Tested at the corners and the center."""
    for dx, dy in ((0, 0), (1, 0), (0, 1), (1, 1), (0.5, 0.5)):
        de = (te + dx) * 1000 - e0
        dn = (tn + dy) * 1000 - n0
        d = math.hypot(de, dn) / 1000
        az = math.degrees(math.atan2(de, dn)) % 360
        if r0 <= d <= r1 and (a0 <= az <= a1 or d < 1.5):
            return True
    return False


def main():
    out, e0, n0, a0, a1, r0, r1, name, gsd = sys.argv[1:10]
    e0, n0, a0, a1, r0, r1 = (float(v) for v in (e0, n0, a0, a1, r0, r1))
    os.makedirs(out, exist_ok=True)
    session = requests.Session()
    session.headers['User-Agent'] = 'sunbeam-tools/1.0'

    # The tiles that are needed.
    want = set()
    span = int(r1) + 2
    for te in range(int(e0 / 1000) - span, int(e0 / 1000) + span + 1):
        for tn in range(int(n0 / 1000) - span, int(n0 / 1000) + span + 1):
            if in_sector(te, tn, e0, n0, a0, a1, r0, r1):
                want.add((te, tn))
    print(len(want), 'tiles in the sector', flush=True)

    # The catalog, queried in blocks of about 10 km.
    found = {}
    cells = sorted({(te // 10, tn // 10) for te, tn in want})
    pat = re.compile(r'_(\d{4})_(\d{4})-(\d{4})')
    for i, (ce, cn) in enumerate(cells):
        w, s = to_wgs(ce * 10000 + 5, cn * 10000 + 5)
        e, n = to_wgs(ce * 10000 + 9995, cn * 10000 + 9995)
        url = API.format(SETS[name])
        params = {'bbox': f'{w:.6f},{s:.6f},{e:.6f},{n:.6f}', 'limit': 100}
        while url:
            for attempt in range(5):
                try:
                    r = session.get(url, params=params, timeout=60)
                    r.raise_for_status()
                    break
                except Exception as ex:
                    time.sleep(2 + 3 * attempt)
            else:
                raise SystemExit(f'catalog not reachable: {url}')
            d = r.json()
            for f in d.get('features', []):
                for key, a in f['assets'].items():
                    if not key.endswith('.tif'):
                        continue
                    if abs(float(a.get('eo:gsd', 0)) - float(gsd)) > 1e-6:
                        continue
                    m = pat.search(key)
                    if not m:
                        continue
                    year, te, tn = int(m.group(1)), int(m.group(2)), int(m.group(3))
                    if (te, tn) in want and (year, a['href']) > found.get((te, tn), (0, '')):
                        found[(te, tn)] = (year, a['href'])
            url = next((l['href'] for l in d.get('links', []) if l.get('rel') == 'next'), None)
            params = None
        if i % 10 == 0:
            print(f'catalog {i + 1}/{len(cells)}, {len(found)} found', flush=True)
    print(len(found), 'tiles in the catalog,', len(want) - len(found), 'missing (outside Switzerland or lake)', flush=True)

    def fetch(item):
        (te, tn), (year, href) = item
        path = os.path.join(out, f'{te}-{tn}.tif')
        if os.path.exists(path) and os.path.getsize(path) > 1000:
            return 0
        for attempt in range(6):
            try:
                r = session.get(href, timeout=120)
                r.raise_for_status()
                with open(path + '.part', 'wb') as fh:
                    fh.write(r.content)
                os.replace(path + '.part', path)
                return len(r.content)
            except Exception:
                time.sleep(2 + 4 * attempt)
        print('failed', href, flush=True)
        return -1

    done = size = bad = 0
    t = time.time()
    with concurrent.futures.ThreadPoolExecutor(12) as pool:
        for got in pool.map(fetch, sorted(found.items())):
            done += 1
            if got < 0:
                bad += 1
            else:
                size += got
            if done % 100 == 0:
                print(f'{done}/{len(found)}  {size / 1e6:.0f} MB  {time.time() - t:.0f} s', flush=True)
    with open(os.path.join(out, 'years.txt'), 'w') as fh:
        for (te, tn), (year, href) in sorted(found.items()):
            fh.write(f'{te}-{tn} {year}\n')
    print(f'done: {done} tiles, {size / 1e6:.0f} MB new, {bad} errors, {time.time() - t:.0f} s', flush=True)


main()
