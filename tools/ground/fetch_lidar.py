"""Downloads USGS (3DEP) lidar points for an area and computes a surface model from them.

The point clouds are public as Entwine Point Tiles: an octree in which each
node is a small LAZ file. Only the nodes above the area are loaded, down to
the requested depth. For each grid cell the highest return (roofs, tree
canopy) and the lowest return (ground) are kept.

Usage: fetch_lidar.py <data set> <west> <south> <east> <north> <depth> <cell in m> <output without extension>
The data set is the name of a folder below the address in BASE. West to north
in degrees. Writes <output>_top.npy, <output>_low.npy and
<output>.txt with west edge, north edge, cell size, reference longitude,
reference latitude and the scale factor of the projection.
Source: U.S. Geological Survey, 3D Elevation Program (public domain).
"""
import concurrent.futures
import io
import json
import math
import sys
import time

import laspy
import numpy as np
import requests

BASE = 'https://s3-us-west-2.amazonaws.com/usgs-lidar-public/'
R = 6378137.0


def merc_x(lon):
    return math.radians(lon) * R


def merc_y(lat):
    return R * math.log(math.tan(math.pi / 4 + math.radians(lat) / 2))


def main():
    name, west, south, east, north, depth, cell, out = sys.argv[1:9]
    west, south, east, north, cell = float(west), float(south), float(east), float(north), float(cell)
    depth = int(depth)
    root = BASE + name + '/'
    session = requests.Session()
    ept = session.get(root + 'ept.json', timeout=60).json()
    bx0, by0, bz0, bx1, by1, bz1 = ept['bounds']
    side0 = bx1 - bx0
    ax0, ax1, ay0, ay1 = merc_x(west), merc_x(east), merc_y(south), merc_y(north)

    # Local grid in meters: origin at the south-west corner.
    lat0 = (south + north) / 2
    k = math.cos(math.radians(lat0))       # Mercator stretches by 1 / cos(latitude)
    width, height = (ax1 - ax0) * k, (ay1 - ay0) * k
    nx, ny = int(math.ceil(width / cell)), int(math.ceil(height / cell))
    print(f'area {width:.0f} x {height:.0f} m, grid {nx} x {ny}', flush=True)

    # The nodes of the tree above the area.
    hier = {}

    def load(key):
        for attempt in range(5):
            try:
                r = session.get(root + f'ept-hierarchy/{key}.json', timeout=60)
                r.raise_for_status()
                hier.update(r.json())
                return
            except Exception:
                time.sleep(1 + 2 * attempt)
        raise SystemExit('cannot read hierarchy: ' + key)

    load('0-0-0-0')
    nodes = []
    stack = [(0, 0, 0, 0)]
    while stack:
        d, x, y, z = stack.pop()
        key = f'{d}-{x}-{y}-{z}'
        if key not in hier:
            continue
        side = side0 / (1 << d)
        if bx0 + x * side > ax1 or bx0 + (x + 1) * side < ax0 or by0 + y * side > ay1 or by0 + (y + 1) * side < ay0:
            continue
        if hier[key] == -1:
            load(key)
        if hier[key] > 0:
            nodes.append((key, hier[key]))
        if d < depth:
            for i in range(8):
                stack.append((d + 1, 2 * x + (i & 1), 2 * y + ((i >> 1) & 1), 2 * z + (i >> 2)))
    total = sum(c for _, c in nodes)
    print(f'{len(nodes)} nodes, {total / 1e6:.1f} million points', flush=True)

    top = np.full((ny, nx), -1e4, np.float32)
    low = np.full((ny, nx), 1e4, np.float32)

    def fetch(item):
        key, _ = item
        for attempt in range(6):
            try:
                r = session.get(root + f'ept-data/{key}.laz', timeout=120)
                r.raise_for_status()
                las = laspy.read(io.BytesIO(r.content))
                cls = np.asarray(las.classification)
                keep = (cls != 7) & (cls != 18)      # noise points
                x = (np.asarray(las.x)[keep] - ax0) * k
                y = (np.asarray(las.y)[keep] - ay0) * k
                z = np.asarray(las.z)[keep].astype(np.float32)
                col = np.floor(x / cell).astype(np.int32)
                row = ny - 1 - np.floor(y / cell).astype(np.int32)
                ok = (col >= 0) & (col < nx) & (row >= 0) & (row < ny)
                return row[ok], col[ok], z[ok], len(r.content)
            except Exception:
                time.sleep(1 + 2 * attempt)
        print('failed', key, flush=True)
        return None

    t = time.time()
    done = size = used = 0
    with concurrent.futures.ThreadPoolExecutor(24) as pool:
        for got in pool.map(fetch, nodes):
            done += 1
            if got is None:
                continue
            row, col, z, n = got
            size += n
            used += len(z)
            if len(z):
                flat = row.astype(np.int64) * nx + col
                order = np.argsort(flat, kind='stable')
                flat, z = flat[order], z[order]
                first = np.concatenate(([True], flat[1:] != flat[:-1]))
                idx = np.nonzero(first)[0]
                cells = flat[idx]
                hi = np.maximum.reduceat(z, idx)
                lo = np.minimum.reduceat(z, idx)
                tf, lf = top.reshape(-1), low.reshape(-1)
                tf[cells] = np.maximum(tf[cells], hi)
                lf[cells] = np.minimum(lf[cells], lo)
            if done % 500 == 0:
                print(f'{done}/{len(nodes)}  {size / 1e6:.0f} MB  {used / 1e6:.0f} million points  {time.time() - t:.0f} s', flush=True)
    np.save(out + '_top.npy', top)
    np.save(out + '_low.npy', low)
    with open(out + '.txt', 'w') as fh:
        fh.write(f'0 {ny * cell} {cell} {west} {south} {k}\n')
    filled = (top > -1000).mean()
    print(f'done: {size / 1e6:.0f} MB, {used / 1e6:.0f} million points in the area, {filled * 100:.1f} % of cells filled, {time.time() - t:.0f} s', flush=True)


main()
