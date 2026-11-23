"""Computes the images of a ground for Sunbeam from an elevation model and an aerial image.

The camera is the same as in the program: pinhole camera, horizontal view,
half the image width = focal length / 2.2. A strip of 3840 by 1088 pixels
covers the full width and the region around the horizon; on a screen 3840
pixels wide each pixel of the strip maps to exactly one screen pixel.

Written per pixel: surface color and coverage (a.png), surface normal,
distance and open sky (b.png), horizon elevation in four directions towards
the light source (c.png), and for a city night lights and gloss (d.png).
From these the program lights the ground itself for any time of day.

Usage: panorama.py <scene.json> [width] [subdivision]
       panorama.py write <folder>
The second form rewrites a.png, b.png and c.png from the raw.npz in the folder.

The scene file is a JSON object with these keys:
  out                 name of the output folder, created next to the scene file;
                      _<width> is appended if the width is not 3840
  east, north         viewpoint in the coordinates of the grids, in meters
  heading             view direction in degrees, clockwise from north
  eye_height          height of the eye in meters; without it the terrain
                      height plus above_ground (default 2) is used
  near, far           range of distances in meters in which the terrain is
                      sampled (near defaults to 150)
  elevation           elevation grid (.npy with the .txt next to it)
  image               aerial image grid (.npy with the .txt next to it)
  elevation_fine, image_fine
                      optional finer grids for the foreground, used up to
                      fine_reach meters with a transition of fine_blend
                      meters (default 1500), and only if width times
                      subdivision is at least 3000
  city                true for a city: elevation is then <output>_height.npy
                      of city_mesh.py, and instead of image the keys ground,
                      buildings and buildings_info name its files
                      <output>_ground.npy, <output>_buildings.npy and
                      <output>_buildings.json, see city_surface.py
"""
import json
import math
import os
import sys
import time

import cv2
import numpy as np

R_EFF = 6371000.0 * 7 / 6        # Earth radius, enlarged to account for refraction
TX = 0.5 / 1.1                   # half the image width as a tangent
FULL_W, FULL_H = 3840, 1088
TOP_ROWS = 512.2                 # number of rows of the strip above the horizon
SUN_OFFSETS = (-27.0, -9.0, 9.0, 27.0)   # directions towards the light source, degrees from the view direction
DIST_MIN, DIST_MAX = 50.0, 400000.0      # range of the stored distance
HOR_MIN, HOR_MAX = -30.0, 60.0           # range of the stored horizon elevation in degrees


class Grid:
    """A raster in map coordinates, north at the top."""

    def __init__(self, path):
        self.a = np.load(path, mmap_mode='r')
        west, north, cell = (float(v) for v in open(os.path.splitext(path)[0] + '.txt').read().split())
        self.west, self.north, self.cell = west, north, cell
        self.h, self.w = self.a.shape[:2]

    def load(self, fix=False):
        self.a = np.array(self.a)
        if fix:
            self.a[self.a < -1000] = 0

    def sample(self, e, n):
        x = (e - self.west) / self.cell - 0.5
        y = (self.north - n) / self.cell - 0.5
        x0 = np.clip(np.floor(x), 0, self.w - 2).astype(np.int32)
        y0 = np.clip(np.floor(y), 0, self.h - 2).astype(np.int32)
        fx = np.clip(x - x0, 0, 1).astype(np.float32)
        fy = np.clip(y - y0, 0, 1).astype(np.float32)
        a = self.a
        if a.ndim == 3:
            fx = fx[..., None]
            fy = fy[..., None]
        top = a[y0, x0] * (1 - fx) + a[y0, x0 + 1] * fx
        bot = a[y0 + 1, x0] * (1 - fx) + a[y0 + 1, x0 + 1] * fx
        return top * (1 - fy) + bot * fy


class Stack:
    """Fine data for the foreground, coarse data for the distance, with a smooth transition in between.

    The fine elevation model contains trees and houses, the coarse one only
    the ground: without a transition there would be a step in the forest at
    the edge of the fine model.
    """

    def __init__(self, fine, coarse, ce, cn, reach, blend):
        self.fine, self.coarse = fine, coarse
        self.ce, self.cn, self.reach, self.blend = ce, cn, reach, blend
        self.cell = coarse.cell

    def cell_at(self, d):
        return self.fine.cell if self.fine is not None and d < self.reach else self.coarse.cell

    def sample(self, e, n):
        v = self.coarse.sample(e, n)
        if self.fine is None:
            return v
        e, n = np.broadcast_arrays(e, n)
        d = np.hypot(e - self.ce, n - self.cn)
        near = d < self.reach
        if near.any():
            f = self.fine
            en, nn = e[near], n[near]
            x = (en - f.west) / f.cell - 0.5
            y = (f.north - nn) / f.cell - 0.5
            x0 = np.clip(np.floor(x), 0, f.w - 2).astype(np.int32)
            y0 = np.clip(np.floor(y), 0, f.h - 2).astype(np.int32)
            inside = (x >= 0) & (x <= f.w - 1) & (y >= 0) & (y <= f.h - 1)
            a = f.a
            c00, c01, c10, c11 = a[y0, x0], a[y0, x0 + 1], a[y0 + 1, x0], a[y0 + 1, x0 + 1]
            if a.ndim == 3:
                ok = inside & (np.minimum(np.minimum(c00.max(-1), c01.max(-1)), np.minimum(c10.max(-1), c11.max(-1))) > 0)
            else:
                ok = inside & (np.minimum(np.minimum(c00, c01), np.minimum(c10, c11)) > -1000)
            fx = np.clip(x - x0, 0, 1).astype(np.float32)
            fy = np.clip(y - y0, 0, 1).astype(np.float32)
            if a.ndim == 3:
                fx, fy = fx[:, None], fy[:, None]
            vf = (c00 * (1 - fx) + c01 * fx) * (1 - fy) + (c10 * (1 - fx) + c11 * fx) * fy
            t = np.clip((self.reach - d[near]) / self.blend, 0, 1)
            w = (t * t * (3 - 2 * t) * ok).astype(np.float32)
            if a.ndim == 3:
                w = w[:, None]
            v = np.array(v, np.float32)
            v[near] = v[near] * (1 - w) + vf * w
        return v


def srgb_to_linear(v):
    v = v / 255.0
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(v):
    v = np.clip(v, 0, 1)
    return np.where(v <= 0.0031308, v * 12.92, 1.055 * v ** (1 / 2.4) - 0.055)


def oct_encode(n):
    """Maps a unit vector to two numbers in 0..1."""
    s = np.abs(n).sum(axis=-1, keepdims=True)
    p = n[..., :2] / np.maximum(s, 1e-9)
    neg = n[..., 2:3] < 0
    q = (1 - np.abs(p[..., ::-1])) * np.where(p >= 0, 1.0, -1.0)
    p = np.where(neg, q, p)
    return p * 0.5 + 0.5


def main():
    cfg = json.load(open(sys.argv[1], encoding='utf-8-sig'))
    W = int(sys.argv[2]) if len(sys.argv) > 2 else FULL_W
    S = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    H = FULL_H * W // FULL_W
    base = os.path.dirname(os.path.abspath(sys.argv[1]))
    out = os.path.join(base, cfg['out'] + ('' if W == FULL_W else f'_{W}'))
    os.makedirs(out, exist_ok=True)
    focal = W / (2 * TX)
    t_top = TOP_ROWS * (W / FULL_W) / focal
    t_bot = (H - TOP_ROWS * (W / FULL_W)) / focal

    t0 = time.time()
    ce, cn = cfg['east'], cfg['north']
    reach, blend = cfg.get('fine_reach', 0), cfg.get('fine_blend', 1500)
    coarse = Grid(cfg['elevation'])
    coarse.load(fix=True)
    fine = None
    if cfg.get('elevation_fine') and W * S >= 3000:
        fine = Grid(cfg['elevation_fine'])
        fine.load()
    dem = Stack(fine, coarse, ce, cn, reach, blend)
    city = img = None
    if cfg.get('city'):
        # A city: facades and lights instead of an aerial image, see city_surface.py.
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import city_surface
        city = city_surface.City(cfg, coarse, Grid)
    else:
        coarse = Grid(cfg['image'])
        coarse.load()
        fine = None
        if cfg.get('image_fine') and W * S >= 3000:
            fine = Grid(cfg['image_fine'])
            fine.load()
        img = Stack(fine, coarse, ce, cn, reach, blend)
    print(f'loaded in {time.time() - t0:.0f} s', flush=True)

    ground = float(dem.sample(np.array([ce]), np.array([cn]))[0])
    ch = cfg.get('eye_height') or ground + cfg.get('above_ground', 2.0)
    az0 = math.radians(cfg['heading'])
    print(f'viewpoint {ce:.0f} {cn:.0f}, terrain {ground:.0f} m, eye {ch:.0f} m, heading {cfg["heading"]}°', flush=True)

    # Distances at which the terrain is sampled along each view direction.
    step = 8e-5 * (FULL_W * 3) / (W * S)
    D = []
    d = float(cfg.get('near', 150.0))
    while d < cfg['far']:
        D.append(d)
        d += max(dem.cell_at(d) * max(1.0, (FULL_W * 3) / (W * S) / 2), d * step)
    D = np.array(D, np.float64)
    K = len(D)
    curv = D * D / (2 * R_EFF)
    WS, HS = W * S, H * S
    ty = t_top - (np.arange(HS) + 0.5) / HS * (t_top + t_bot)
    print(f'{W} x {H}, subdivision {S}, {K} steps per direction', flush=True)

    alpha = np.zeros((H, W), np.float32)
    alb = np.zeros((H, W, 3), np.float32)
    nrm = np.zeros((H, W, 3), np.float32)
    dist = np.zeros((H, W), np.float32)
    rep = np.full((H, W, 2), np.nan, np.float64)     # position of the surface at the center of each pixel
    emi = np.zeros((H, W, 3), np.float32)            # night lights
    glo = np.zeros((H, W), np.float32)               # gloss: fraction of glass
    f_e, f_n = math.sin(az0), math.cos(az0)          # view direction
    r_e, r_n = math.cos(az0), -math.sin(az0)         # to the right
    CH = 64 * S
    mid = S // 2
    for c0 in range(0, WS, CH):
        ii = np.arange(c0, min(c0 + CH, WS))
        C = len(ii)
        tx = ((ii + 0.5) / WS * 2 - 1) * TX
        az = az0 + np.arctan(tx)
        hs = np.sqrt(1 + tx * tx)
        sa, ca = np.sin(az), np.cos(az)
        Z = dem.sample(ce + sa[:, None] * D[None, :], cn + ca[:, None] * D[None, :]).astype(np.float64) - curv[None, :] - ch
        T = Z / D[None, :]
        M = np.maximum.accumulate(T, axis=1)
        hd = np.full((HS, C), np.nan, np.float64)
        for c in range(C):
            t = ty / hs[c]
            k = np.searchsorted(M[c], t, side='left')
            kk = np.clip(k, 1, K - 1)
            d0, d1 = D[kk - 1], D[kk]
            z0, z1 = Z[c, kk - 1], Z[c, kk]
            den = (z1 - z0) - t * (d1 - d0)
            f = np.clip(np.where(np.abs(den) > 1e-9, (t * d0 - z0) / np.where(np.abs(den) > 1e-9, den, 1), 1.0), 0, 1)
            dh = d0 + f * (d1 - d0)
            dh[k == 0] = D[0]
            dh[k >= K] = np.nan
            hd[:, c] = dh
        hit = ~np.isnan(hd)
        rows, cols = np.nonzero(hit)
        dd = hd[rows, cols]
        e = ce + sa[cols] * dd
        n = cn + ca[cols] * dd
        # Surface normal from the slope, taken over a span that matches the distance.
        delta = np.maximum(np.where(dd < reach, 0.5, 1.0) * dem.cell if dem.fine is not None else dem.cell, dd * (TX * 2 / W) * 0.75)
        gx = (dem.sample(e + delta, n) - dem.sample(e - delta, n)) / (2 * delta)
        gy = (dem.sample(e, n + delta) - dem.sample(e, n - delta)) / (2 * delta)
        inv = 1 / np.sqrt(gx * gx + gy * gy + 1)
        ne, nn, nu = -gx * inv, -gy * inv, inv
        if city is not None:
            # Height of the hit from the view ray: on a wall it lies between ground and roof.
            zt = ch + ty[rows] / hs[cols] * dd + dd * dd / (2 * R_EFF)
            col, light, shine, keep = city.material(e, n, zt, ne, nn, nu)
            # Water is not land: the program draws it itself.
            rows, cols, dd, e, n = rows[keep], cols[keep], dd[keep], e[keep], n[keep]
            col, light, shine, ne, nn, nu = col[keep], light[keep], shine[keep], ne[keep], nn[keep], nu[keep]
        else:
            # Color from the aerial image.
            col = srgb_to_linear(img.sample(e, n).astype(np.float32))
        ncam = np.stack([ne * r_e + nn * r_n, nu, ne * f_e + nn * f_n], axis=-1).astype(np.float32)

        a_s = np.zeros((HS, C), np.float32)
        a_s[rows, cols] = 1
        c_s = np.zeros((HS, C, 3), np.float32)
        c_s[rows, cols] = col
        n_s = np.zeros((HS, C, 3), np.float32)
        n_s[rows, cols] = ncam
        d_s = np.zeros((HS, C), np.float32)
        d_s[rows, cols] = np.log(dd)
        x0 = c0 // S
        x1 = x0 + C // S

        def fold(v):
            return v.reshape((H, S, C // S, S) + v.shape[2:]).sum(axis=(1, 3))

        if city is not None:
            l_s = np.zeros((HS, C, 3), np.float32)
            l_s[rows, cols] = light
            g_s = np.zeros((HS, C), np.float32)
            g_s[rows, cols] = shine
            emi[:, x0:x1] = fold(l_s)
            glo[:, x0:x1] = fold(g_s)
        alpha[:, x0:x1] = fold(a_s) / (S * S)
        alb[:, x0:x1] = fold(c_s)
        nrm[:, x0:x1] = fold(n_s)
        dist[:, x0:x1] = fold(d_s)
        # Position at the center of the pixel; if there is none, that of another subpixel.
        e_s = np.full((HS, C), np.nan)
        n_s2 = np.full((HS, C), np.nan)
        e_s[rows, cols] = e
        n_s2[rows, cols] = n
        e4 = e_s.reshape(H, S, C // S, S)
        n4 = n_s2.reshape(H, S, C // S, S)
        re, rn = e4[:, mid, :, mid].copy(), n4[:, mid, :, mid].copy()
        for sy in range(S):
            for sx in range(S):
                miss = np.isnan(re)
                re[miss] = e4[:, sy, :, sx][miss]
                rn[miss] = n4[:, sy, :, sx][miss]
        rep[:, x0:x1, 0] = re
        rep[:, x0:x1, 1] = rn
        if (c0 // CH) % 10 == 0:
            print(f'column {c0 // S}/{W}  {time.time() - t0:.0f} s', flush=True)

    cnt = np.maximum(alpha * (S * S), 1e-6)
    alb /= cnt[..., None]
    emi /= cnt[..., None]
    glo /= cnt
    dist = np.exp(dist / cnt)
    nrm /= np.maximum(np.linalg.norm(nrm, axis=-1, keepdims=True), 1e-6)
    cover = alpha > 0
    print(f'coverage {cover.mean() * 100:.1f} %, distance {dist[cover].min():.0f} to {dist[cover].max():.0f} m, {time.time() - t0:.0f} s', flush=True)

    # Horizon elevation seen from each pixel in the directions the light can come from.
    pe, pn = rep[cover][:, 0], rep[cover][:, 1]
    pz = dem.sample(pe, pn).astype(np.float64)
    hor = np.zeros((len(pe), 4), np.float32)
    steps = []
    s = 1.5
    while s < min(45000, cfg['far']):
        steps.append(s)
        s *= 1.09
    for m, off in enumerate(SUN_OFFSETS):
        a = az0 + math.radians(off)
        best = np.full(len(pe), -10.0)
        for s in steps:
            z = dem.sample(pe + math.sin(a) * s, pn + math.cos(a) * s) - s * s / (2 * R_EFF)
            np.maximum(best, (z - pz) / s, out=best)
        hor[:, m] = np.degrees(np.arctan(best))
        print(f'horizon {off:+.0f}°  {time.time() - t0:.0f} s', flush=True)
    # Open sky: the fraction of the sky hemisphere that the surrounding terrain leaves unobstructed.
    open_sky = np.zeros(len(pe), np.float32)
    ring = []
    s = 2.0
    while s < 4000:
        ring.append(s)
        s *= 1.3
    for q in range(12):
        a = math.radians(q * 30 + 15)
        best = np.zeros(len(pe))
        for s in ring:
            z = dem.sample(pe + math.sin(a) * s, pn + math.cos(a) * s)
            np.maximum(best, (z - pz) / s, out=best)
        open_sky += (1 / (1 + best * best)).astype(np.float32) / 12
    print(f'open sky {time.time() - t0:.0f} s', flush=True)

    horizon = np.zeros((H, W, 4), np.float32)
    horizon[cover] = hor
    sky = np.ones((H, W), np.float32)
    sky[cover] = open_sky

    # Where there is no surface, extend the values of the neighbors: otherwise
    # filtered sampling at the edge of the silhouette mixes in empty pixels.
    def spread(v, known, rounds=6):
        v = v.copy()
        k = known.astype(np.float32)
        ker = np.ones((3, 3), np.float32)
        for _ in range(rounds):
            num = cv2.filter2D(v * (k[..., None] if v.ndim == 3 else k), -1, ker, borderType=cv2.BORDER_REPLICATE)
            den = cv2.filter2D(k, -1, ker, borderType=cv2.BORDER_REPLICATE)
            fill = (k == 0) & (den > 0)
            if v.ndim == 3:
                v[fill] = num[fill] / den[fill][:, None]
            else:
                v[fill] = num[fill] / den[fill]
            k[fill] = 1
        return v

    alb = spread(alb, cover)
    nrm = spread(nrm, cover)
    nrm /= np.maximum(np.linalg.norm(nrm, axis=-1, keepdims=True), 1e-6)
    dist = spread(dist, cover)
    horizon = spread(horizon, cover)
    sky = spread(sky, cover)
    if city is not None:
        glo = spread(glo, cover)
        np.savez_compressed(os.path.join(out, 'lights.npz'), emi=emi, glo=glo)
        write_lights(out, emi, glo)

    np.savez_compressed(os.path.join(out, 'raw.npz'), alpha=alpha, alb=alb, nrm=nrm, dist=dist, horizon=horizon, sky=sky)
    json.dump({'width': W, 'height': H, 'eye': ch, 'rows_above_horizon': TOP_ROWS * W / FULL_W}, open(os.path.join(out, 'values.json'), 'w'))
    write(out, alpha, alb, nrm, dist, horizon, sky)
    print(f'written to {out}, {time.time() - t0:.0f} s', flush=True)


LIGHT_MAX = 24.0     # highest luminance that d.png can hold, as in city_surface.py and ground.hlsli


def write_lights(out, emi, glo):
    """Night lights and gloss. The lights are stored as a square root so that low levels are kept."""
    H, W = glo.shape
    d8 = np.zeros((H, W, 4), np.uint8)
    d8[..., :3] = np.round(np.sqrt(np.clip(emi / LIGHT_MAX, 0, 1)) * 255)
    d8[..., 3] = np.round(np.clip(glo, 0, 1) * 255)
    cv2.imwrite(os.path.join(out, 'd.png'), d8[..., [2, 1, 0, 3]])


def write(out, alpha, alb, nrm, dist, horizon, sky):
    """Writes the images in the form the program reads."""
    H, W = alpha.shape
    a8 = np.zeros((H, W, 4), np.uint8)
    a8[..., :3] = np.round(linear_to_srgb(alb) * 255)
    a8[..., 3] = np.round(alpha * 255)
    cv2.imwrite(os.path.join(out, 'a.png'), a8[..., [2, 1, 0, 3]])
    b16 = np.zeros((H, W, 4), np.float32)
    # The vertical is the axis: terrain almost always faces upward, so the mapping stays continuous there.
    b16[..., :2] = oct_encode(nrm[..., [0, 2, 1]])
    b16[..., 2] = np.log(np.clip(dist, DIST_MIN, DIST_MAX) / DIST_MIN) / math.log(DIST_MAX / DIST_MIN)
    b16[..., 3] = sky
    cv2.imwrite(os.path.join(out, 'b.png'), np.round(np.clip(b16, 0, 1) * 65535).astype(np.uint16)[..., [2, 1, 0, 3]])
    c16 = (horizon - HOR_MIN) / (HOR_MAX - HOR_MIN)
    cv2.imwrite(os.path.join(out, 'c.png'), np.round(np.clip(c16, 0, 1) * 65535).astype(np.uint16)[..., [2, 1, 0, 3]])


if len(sys.argv) > 2 and sys.argv[1] == 'write':
    # Rewrite only the images from the stored values: panorama.py write <folder>
    d = np.load(os.path.join(sys.argv[2], 'raw.npz'))
    write(sys.argv[2], d['alpha'], d['alb'], d['nrm'], d['dist'], d['horizon'], d['sky'])
else:
    main()
