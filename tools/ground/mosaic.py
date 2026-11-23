"""Assembles the one-kilometer tiles of a folder into one image and stores it as .npy.

Usage: mosaic.py <folder with tiles> <output.npy> [pixels per kilometer]
If the tiles are finer than requested, they are downscaled by area averaging.
Also writes <output>.txt with west edge, north edge (LV95, meters) and cell size.
"""
import os
import sys

import cv2
import numpy as np


def main():
    src, dst = sys.argv[1], sys.argv[2]
    names = [f[:-4] for f in os.listdir(src) if f.endswith('.tif')]
    tiles = sorted((int(n.split('-')[0]), int(n.split('-')[1])) for n in names)
    e0 = min(t[0] for t in tiles)
    e1 = max(t[0] for t in tiles) + 1
    n0 = min(t[1] for t in tiles)
    n1 = max(t[1] for t in tiles) + 1
    first = cv2.imread(os.path.join(src, f'{tiles[0][0]}-{tiles[0][1]}.tif'), cv2.IMREAD_UNCHANGED)
    px = int(sys.argv[3]) if len(sys.argv) > 3 else first.shape[0]
    cell = 1000.0 / px
    shape = ((n1 - n0) * px, (e1 - e0) * px) + first.shape[2:]
    print('mosaic', shape, first.dtype, f'{np.prod(shape) * first.dtype.itemsize / 1e9:.2f} GB', flush=True)
    out = np.lib.format.open_memmap(dst, mode='w+', dtype=first.dtype, shape=shape)
    out[:] = -9999 if first.dtype == np.float32 else 0
    for i, (te, tn) in enumerate(tiles):
        a = cv2.imread(os.path.join(src, f'{te}-{tn}.tif'), cv2.IMREAD_UNCHANGED)
        if a is None:
            print('unreadable', te, tn, flush=True)
            continue
        if a.shape[0] != px:
            a = cv2.resize(a, (px, px), interpolation=cv2.INTER_AREA)
        if a.ndim == 3:
            a = a[:, :, ::-1]   # BGR to RGB
        r = (n1 - 1 - tn) * px
        c = (te - e0) * px
        out[r:r + px, c:c + px] = a
        if i % 20 == 0:
            print(i, '/', len(tiles), flush=True)
    out.flush()
    with open(os.path.splitext(dst)[0] + '.txt', 'w') as fh:
        fh.write(f'{e0 * 1000} {n1 * 1000} {cell}\n')
    print('done', flush=True)


main()
