"""Surfaces of a city for the panorama: facades with a window grid, roofs, night lights.

The lidar survey gives the shape of each building but no facades. They are
generated here: per building a grid of floors and window bays, with type and
color taken from the use and set by hand for well-known towers. At night a
part of the windows is lit, room by room, in the light color of the building.
"""
import json

import numpy as np
from scipy import ndimage

LIGHT = 9.0          # luminance of a lit window, in units of the program before exposure
LIGHT_MAX = 24.0     # highest value the stored image can hold

# Well-known towers: part of the name in lower case, then frame color, glass color, facade type,
# fraction of lit windows, floodlighting of the facade.
# Types: 0 glass facade, 1 ribbon windows, 2 punched windows, 3 narrow vertical windows between piers.
KNOWN = [
    ('willis', (0.022, 0.022, 0.026), (0.030, 0.030, 0.034), 1, 0.34, 0.0),
    ('875 n', (0.020, 0.020, 0.022), (0.028, 0.028, 0.032), 1, 0.38, 0.0),
    ('hancock', (0.020, 0.020, 0.022), (0.028, 0.028, 0.032), 1, 0.38, 0.0),
    ('aon center', (0.60, 0.59, 0.55), (0.040, 0.045, 0.055), 3, 0.30, 0.0),
    ('trump', (0.16, 0.19, 0.22), (0.085, 0.115, 0.145), 0, 0.30, 0.0),
    ('st. regis', (0.07, 0.12, 0.14), (0.050, 0.105, 0.125), 0, 0.34, 0.0),
    ('vista tower', (0.07, 0.12, 0.14), (0.050, 0.105, 0.125), 0, 0.34, 0.0),
    ('one prudential', (0.42, 0.41, 0.38), (0.045, 0.050, 0.060), 3, 0.30, 0.0),
    ('two prudential', (0.24, 0.25, 0.27), (0.050, 0.060, 0.075), 1, 0.30, 0.0),
    ('aqua', (0.56, 0.57, 0.58), (0.045, 0.060, 0.080), 1, 0.40, 0.0),
    ('cna', (0.30, 0.045, 0.035), (0.035, 0.030, 0.030), 2, 0.26, 0.0),
    ('blue cross', (0.06, 0.11, 0.13), (0.045, 0.100, 0.120), 0, 0.30, 0.0),
    ('lake point tower', (0.032, 0.028, 0.024), (0.036, 0.031, 0.026), 0, 0.36, 0.0),
    ('wrigley', (0.70, 0.68, 0.62), (0.050, 0.050, 0.055), 2, 0.22, 0.30),
    ('tribune tower', (0.45, 0.42, 0.36), (0.045, 0.045, 0.050), 2, 0.22, 0.12),
    ('crain communications', (0.58, 0.58, 0.56), (0.045, 0.055, 0.070), 1, 0.30, 0.0),
    ('carbide', (0.030, 0.045, 0.035), (0.030, 0.035, 0.035), 2, 0.25, 0.0),
    ('water tower place', (0.62, 0.61, 0.58), (0.045, 0.045, 0.050), 2, 0.12, 0.0),
    ('nema', (0.09, 0.10, 0.12), (0.050, 0.065, 0.085), 0, 0.36, 0.0),
    ('one museum park', (0.08, 0.11, 0.14), (0.050, 0.085, 0.115), 0, 0.36, 0.0),
    ('legacy', (0.08, 0.11, 0.15), (0.050, 0.080, 0.120), 0, 0.36, 0.0),
    ('chase tower', (0.34, 0.33, 0.31), (0.045, 0.050, 0.060), 3, 0.30, 0.0),
    ('311 south wacker', (0.40, 0.33, 0.30), (0.045, 0.050, 0.060), 2, 0.30, 0.0),
    ('franklin center', (0.36, 0.30, 0.28), (0.050, 0.055, 0.065), 2, 0.30, 0.0),
    ('900 north michigan', (0.52, 0.49, 0.43), (0.045, 0.075, 0.065), 2, 0.26, 0.0),
    ('park tower', (0.48, 0.45, 0.40), (0.045, 0.050, 0.060), 2, 0.32, 0.0),
    ('palmolive', (0.50, 0.47, 0.41), (0.045, 0.045, 0.050), 2, 0.30, 0.10),
]


def mix(*values):
    """Pseudo-random number in 0..1 from integers, one per element of the arrays."""
    x = np.uint32(0x9E3779B9)
    with np.errstate(over='ignore'):
        for v in values:
            x = (x ^ np.asarray(v).astype(np.int64).astype(np.uint32)) * np.uint32(0x85EBCA6B)
            x = x ^ (x >> np.uint32(15))
            x = x * np.uint32(0xC2B2AE35)
            x = x ^ (x >> np.uint32(13))
    return (x >> np.uint32(8)).astype(np.float32) / 16777216.0


class City:
    def __init__(self, cfg, dem, Grid):
        self.ground = Grid(cfg['ground'])
        self.ground.load()
        self.building = Grid(cfg['buildings'])
        self.building.load()
        meta = json.load(open(cfg['buildings_info'], encoding='utf-8'))
        self.lake = meta['lake']
        buildings = meta['buildings']
        n = int(self.building.a.max()) + 1
        ids = np.arange(n)
        # Height of each building above the ground: its highest point, also taken as the roof edge.
        rel = dem.a - self.ground.a
        self.top = np.zeros(n, np.float32)
        self.top[:] = ndimage.maximum(rel, labels=self.building.a, index=ids)
        self.top[0] = 0
        r1, r2, r3, r4 = mix(ids, 11), mix(ids, 23), mix(ids, 37), mix(ids, 51)
        # Facade type: tall buildings tend towards glass, low ones towards punched windows.
        tall = np.clip((self.top - 40) / 160, 0, 1)
        self.style = np.where(r1 < 0.25 + 0.35 * tall, 0, np.where(r1 < 0.70 + 0.15 * tall, 1, 2)).astype(np.int32)
        stone = np.stack([0.30 + 0.30 * r2, 0.29 + 0.28 * r2, 0.26 + 0.25 * r2], -1)
        dark = np.stack([0.05 + 0.08 * r2, 0.055 + 0.09 * r2, 0.06 + 0.10 * r2], -1)
        blue = np.stack([0.040 + 0.03 * r3, 0.060 + 0.05 * r3, 0.080 + 0.07 * r3], -1)
        grey = np.stack([0.040 + 0.02 * r3, 0.045 + 0.02 * r3, 0.052 + 0.02 * r3], -1)
        self.frame = np.where((self.style == 2)[:, None] | (r4 < 0.35)[:, None], stone, dark).astype(np.float32)
        self.glass = np.where((r4 < 0.6)[:, None], blue, grey).astype(np.float32)
        self.floor = np.full(n, 3.9, np.float32)
        self.bay = (1.4 + 1.2 * r3).astype(np.float32)
        self.group = (1 + np.floor(2 * r2)).astype(np.float32)      # bays per room
        self.lit = (0.10 + 0.24 * r1).astype(np.float32)
        self.flood = np.zeros(n, np.float32)
        self.roof = np.stack([0.16 + 0.14 * r4, 0.16 + 0.14 * r4, 0.165 + 0.14 * r4], -1).astype(np.float32)
        # Light color: offices neutral to cool, apartments and hotels warm.
        warm = np.array((1.0, 0.74, 0.46), np.float32)
        cool = np.array((0.92, 0.96, 1.0), np.float32)
        tone = np.full(n, 0.55, np.float32)
        for key, b in buildings.items():
            i = int(key)
            if i >= n:
                continue
            kind = b.get('kind', '')
            if kind in ('apartments', 'residential', 'hotel', 'dormitory'):
                tone[i] = 0.12
                self.floor[i] = 3.15
                self.lit[i] = 0.12 + 0.16 * r1[i]
                self.group[i] = 1
            name = (b.get('name') or '').lower()
            for part, frame, glass, style, lit, flood in KNOWN:
                if part in name:
                    self.frame[i], self.glass[i], self.style[i], self.lit[i], self.flood[i] = frame, glass, style, lit, flood
                    break
        tone = np.clip(tone + 0.25 * (r3 - 0.5), 0, 1)
        self.light = (warm[None, :] * (1 - tone[:, None]) + cool[None, :] * tone[:, None]).astype(np.float32)
        self.shift = (r2 * 7.0).astype(np.float32)

    def nearest(self, grid, e, n):
        x = np.clip(np.floor((e - grid.west) / grid.cell), 0, grid.w - 1).astype(np.int32)
        y = np.clip(np.floor((grid.north - n) / grid.cell), 0, grid.h - 1).astype(np.int32)
        return grid.a[y, x]

    def material(self, e, n, z, ne, nn, nu):
        """For hits on the surface: color, night light, gloss, and whether it is land (not water)."""
        g = self.ground.sample(e, n)
        rel = (z - g).astype(np.float32)
        keep = z > self.lake + 0.35
        wall = nu < 0.55
        nh = np.hypot(ne, nn) + 1e-6
        # A wall belongs to the building behind it: look one step inward.
        b = self.nearest(self.building, np.where(wall, e - ne / nh * 1.2, e), np.where(wall, n - nn / nh * 1.2, n))
        isb = b > 0
        fh, bw = self.floor[b], self.bay[b]
        u = np.where(np.abs(ne) > np.abs(nn), n, e)
        fl = np.maximum(rel, 0) / fh
        fi = np.floor(fl)
        ff = fl - fi
        bl = (u + self.shift[b]) / bw
        bi = np.floor(bl)
        bf = bl - bi
        style = self.style[b]
        win = np.where(style == 0, (ff > 0.20) & (bf > 0.07),
              np.where(style == 1, (ff > 0.34) & (ff < 0.86),
              np.where(style == 2, (ff > 0.30) & (ff < 0.80) & (bf > 0.24) & (bf < 0.76),
                       (ff > 0.10) & (bf > 0.58))))
        win &= wall & isb & (rel > 4.5) & (self.top[b] - rel > 1.5)

        col = np.where(win[:, None], self.glass[b], self.frame[b])
        col = np.where((isb & ~wall)[:, None], self.roof[b], col)
        # Without a building: trees and structures dark green, otherwise pavement, quay walls, lawn.
        tree = ~isb & (rel > 2.5)
        col = np.where(tree[:, None], np.array((0.030, 0.045, 0.022), np.float32), col)
        flat = ~isb & ~tree
        shade = 0.10 + 0.06 * mix(np.floor(e / 9), np.floor(n / 9))
        col = np.where(flat[:, None], np.stack([shade, shade, shade * 0.95], -1), col).astype(np.float32)
        gloss = np.where(win, 1.0, 0.06).astype(np.float32)

        # Lit windows, room by room. At night most floors are almost dark,
        # some are occupied, a few are brightly lit. Most windows are dim,
        # a few are bright; each has a slightly different color.
        room = np.floor(bi / self.group[b])
        floor_r = mix(b, fi, 7777)
        chance = self.lit[b] * np.where(floor_r < 0.50, 0.22, np.where(floor_r > 0.92, 2.8, 1.0))
        on = win & (mix(b, fi, room) < chance)
        strength = mix(b, fi, room, 5)
        level = LIGHT * (0.10 + 0.95 * strength * strength)
        hue = mix(b, fi, room, 9)
        warm = np.array((1.0, 0.70, 0.40), np.float32)
        green = np.array((0.86, 1.0, 0.88), np.float32)
        tint = self.light[b] * (0.9 + 0.2 * hue)[:, None]
        tint = np.where((hue < 0.16)[:, None], warm, np.where((hue > 0.93)[:, None], green, tint))
        emis = np.where(on[:, None], tint * level[:, None], 0.0).astype(np.float32)
        # Floodlit facades.
        emis += np.where((wall & isb)[:, None], self.frame[b] * (self.flood[b] * LIGHT)[:, None], 0.0)
        # Red aircraft warning lights at the roof edges of tall buildings, masts lit in white.
        high = isb & (self.top[b] > 170)
        beacon = high & wall & (self.top[b] - rel < 1.8) & (mix(b, np.floor(bi / 3)) < 0.05)
        emis = np.where(beacon[:, None], np.array((15.0, 0.8, 0.4), np.float32), emis)
        mast = isb & (self.top[b] > 380) & (rel > 0.845 * self.top[b]) & (rel > 300)
        emis = np.where(mast[:, None], np.array((0.55, 0.6, 0.75), np.float32), emis)
        # Street lamps along the shores and streets.
        lamp = flat & (rel < 6) & (mix(np.floor(e / 14), np.floor(n / 14), 3) < 0.07) & ((e / 14) % 1 < 0.2) & ((n / 14) % 1 < 0.2)
        emis = np.where(lamp[:, None], np.array((14.0, 9.5, 5.0), np.float32), emis).astype(np.float32)
        return col, emis, gloss, keep
