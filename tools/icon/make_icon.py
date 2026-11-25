"""Draws the program icon: a low sun over the sea on a rounded tile, with light shafts fanning out from it.

Usage: make_icon.py <sunbeam.ico> [preview.png]

Every size is drawn on its own, four times larger and then reduced. Below 32
pixels the shafts and the glitter on the water are left out; they would only
blur the sun.
"""
import io
import math
import struct
import sys

import numpy as np
from PIL import Image

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
OVER = 4


def mix(a, b, t):
    t = np.clip(t, 0.0, 1.0)[..., None]
    return np.asarray(a, np.float32) * (1 - t) + np.asarray(b, np.float32) * t


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def draw(size):
    n = size * OVER
    y, x = np.mgrid[0:n, 0:n].astype(np.float32) + 0.5
    u, v = x / n, y / n
    small = size < 32

    # The tile: a square with round corners, a little inset.
    inset, radius = (0.04, 0.2) if small else (0.06, 0.22)
    qx = np.maximum(np.abs(u - 0.5) - (0.5 - inset - radius), 0)
    qy = np.maximum(np.abs(v - 0.5) - (0.5 - inset - radius), 0)
    edge = np.sqrt(qx * qx + qy * qy) - radius
    cover = smooth(0.5 / size, -0.5 / size, edge)

    horizon = 0.68
    sun = (0.5, 0.66)
    sun_r = 0.16 if small else 0.13

    # Sky: deep blue at the top, warm toward the horizon.
    sky = mix((24, 44, 92), (58, 96, 164), v / 0.4)
    sky = mix(sky, (240, 150, 80), (v - 0.38) / (horizon - 0.38))
    # Sea: dark, a little lighter toward the horizon.
    sea = mix((40, 70, 98), (12, 30, 48), (v - horizon) / (1 - horizon))
    col = np.where((v < horizon)[..., None], sky, sea)

    dx, dy = u - sun[0], v - sun[1]
    dist = np.sqrt(dx * dx + dy * dy)
    if not small:
        # Shafts: bright and dark sectors fanning upward from the sun.
        ang = np.arctan2(dx, -dy)
        shafts = 0.5 + 0.5 * np.cos(ang * 9.0)
        reach = smooth(0.95, 0.15, dist) * smooth(-1.4, -0.2, -np.abs(ang)) * (v < horizon)
        col = col + (shafts - 0.45)[..., None] * reach[..., None] * np.array([70, 56, 34], np.float32)
        # Glitter path on the water below the sun.
        path = np.exp(-(dx / (0.05 + 0.18 * (v - horizon))) ** 2) * (v > horizon)
        ripple = 0.55 + 0.45 * np.cos((v - horizon) * 120.0)
        col = col + (path * ripple * smooth(1.0, horizon, v))[..., None] * np.array([230, 170, 100], np.float32)
    # Glow around the sun, over sky and water.
    glow = np.exp(-dist / (0.12 if small else 0.1))
    col = col + glow[..., None] * np.array([255, 190, 110], np.float32) * (0.5 if small else 0.7)
    if not small:
        # The horizon line, faint.
        line = np.exp(-((v - horizon) * n / (0.8 * OVER)) ** 2) * 0.35
        col = col + line[..., None] * np.array([255, 200, 150], np.float32)
    # The disc: almost white at the center, golden at the rim, its lower part
    # already behind the horizon.
    disc = smooth(sun_r + 0.6 / size, sun_r - 0.6 / size, dist) * smooth(horizon + 0.5 / size, horizon - 0.5 / size, v)
    face = mix((255, 250, 232), (255, 206, 120), dist / sun_r)
    col = col * (1 - disc[..., None]) + face * disc[..., None]

    col = np.clip(col, 0, 255)
    rgba = np.concatenate([col, (cover * 255)[..., None]], axis=2).astype(np.uint8)
    img = Image.fromarray(rgba, 'RGBA')
    # Reduced with premultiplied colors, so the rim does not get a dark fringe.
    pre = np.asarray(img).astype(np.float32)
    pre[..., :3] *= pre[..., 3:4] / 255.0
    pre = Image.fromarray(pre.astype(np.uint8), 'RGBA').resize((size, size), Image.BOX)
    out = np.asarray(pre).astype(np.float32)
    a = np.maximum(out[..., 3:4], 1e-3)
    out[..., :3] = np.clip(out[..., :3] * 255.0 / a, 0, 255)
    return Image.fromarray(out.astype(np.uint8), 'RGBA')


def write_ico(path, images):
    # Every image as PNG; Windows reads PNG entries in icons of any size.
    blobs = []
    for img in images:
        buf = io.BytesIO()
        img.save(buf, 'PNG', optimize=True)
        blobs.append(buf.getvalue())
    head = struct.pack('<HHH', 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries = b''
    for img, blob in zip(images, blobs):
        w = img.width if img.width < 256 else 0
        entries += struct.pack('<BBBBHHII', w, w, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    with open(path, 'wb') as f:
        f.write(head + entries + b''.join(blobs))


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    images = [draw(s) for s in SIZES]
    write_ico(sys.argv[1], images)
    print('written', sys.argv[1], [i.size[0] for i in images])
    if len(sys.argv) > 2:
        # All sizes side by side on a light and on a dark ground.
        w = sum(SIZES) + 10 * len(SIZES)
        sheet = Image.new('RGBA', (w, 2 * 256 + 30), (0, 0, 0, 0))
        for row, ground in enumerate([(243, 243, 243, 255), (32, 32, 32, 255)]):
            band = Image.new('RGBA', (w, 256 + 10), ground)
            x = 5
            for img in images:
                band.alpha_composite(img, (x, 256 - img.height))
                x += img.width + 10
            sheet.paste(band, (0, row * (256 + 15)))
        sheet.save(sys.argv[2])
