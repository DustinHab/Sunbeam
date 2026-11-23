"""Lights a computed ground for checking, roughly as the program does.

Usage: preview.py <folder> <output.png> <sun offset from the view direction in degrees> <sun elevation in degrees> [exposure]
"""
import json
import math
import os
import sys

import cv2
import numpy as np

SUN_OFFSETS = np.array((-27.0, -9.0, 9.0, 27.0))
K_RLH = np.array((5.5e-6, 13.0e-6, 22.4e-6))
K_MIE = 10e-6
R_EFF = 6371000.0 * 7 / 6
TX = 0.5 / 1.1


def main():
    src, dst, off, alt = sys.argv[1], sys.argv[2], float(sys.argv[3]), float(sys.argv[4])
    expo = float(sys.argv[5]) if len(sys.argv) > 5 else 0.2
    d = np.load(os.path.join(src, 'raw.npz'))
    meta = json.load(open(os.path.join(src, 'values.json')))
    alpha, alb, nrm, dist, horizon, sky = (d[k] for k in ('alpha', 'alb', 'nrm', 'dist', 'horizon', 'sky'))
    H, W = alpha.shape
    focal = W / (2 * TX)
    a, e = math.radians(off), math.radians(alt)
    L = np.array((math.sin(a) * math.cos(e), math.sin(e), math.cos(a) * math.cos(e)), np.float32)
    # Horizon in the direction of the sun, interpolated between the four stored directions.
    f = np.clip((off - SUN_OFFSETS[0]) / (SUN_OFFSETS[1] - SUN_OFFSETS[0]), 0, 2.999)
    i = int(f)
    hor = horizon[..., i] * (1 - (f - i)) + horizon[..., i + 1] * (f - i)
    x = np.clip((alt - hor + 0.7) / 1.4, 0, 1)
    shadow = x * x * (3 - 2 * x)
    ndl = np.clip((nrm * L).sum(axis=-1), 0, 1)
    air = 1 / max(math.sin(e) + 0.15 * (alt + 3.885) ** -1.253, 0.02) if alt > -3 else 40
    sun = 22 / math.pi * np.exp(-(K_RLH * 8000 + K_MIE * 1200 * 1.1) * air * 0.75)
    amb = np.array((0.22, 0.34, 0.62)) * (0.25 + 0.75 * min(1.0, max(0.0, (alt + 4) / 20)))
    lit = alb * (sun * (ndl * shadow)[..., None] + amb * sky[..., None])

    ty = (meta['rows_above_horizon'] - (np.arange(H) + 0.5)) / focal
    tx = ((np.arange(W) + 0.5) / W * 2 - 1) * TX
    hs = np.sqrt(1 + tx * tx)
    eye = meta['eye']
    dh = dist / np.sqrt(1 + (ty[:, None] / hs[None, :]) ** 2)       # horizontal distance
    z = eye + ty[:, None] / hs[None, :] * dh + dh * dh / (2 * R_EFF)

    def mean_density(scale):
        x = (z - eye) / scale
        safe = np.where(np.abs(x) < 1e-3, 1.0, x)
        return math.exp(-eye / scale) * np.where(np.abs(x) < 1e-3, 1 - x / 2, (1 - np.exp(-safe)) / safe)

    tau = K_RLH * (mean_density(8000.0) * dist)[..., None] + K_MIE * 1.1 * (mean_density(1200.0) * dist)[..., None]
    T = np.exp(-tau)
    # Sky: bright and warm at the horizon towards the sun, blue above.
    up = np.clip(ty, 0, None)[:, None, None]
    toward = np.exp(-((np.degrees(np.arctan(tx)) - off) / 18) ** 2)[None, :, None]
    low = np.array((0.95, 0.98, 1.05)) * (0.55 + 0.9 * toward) * (0.3 + 0.7 * min(1.0, max(0.05, (alt + 6) / 14)))
    if alt < 8:
        low = low * (1 - toward * 0.5) + toward * 0.5 * np.array((1.6, 0.9, 0.45)) * (0.3 + 0.7 * max(0.0, (alt + 6) / 14))
    high = np.array((0.16, 0.30, 0.62)) * min(1.0, max(0.03, (alt + 8) / 16))
    sky_color = high + (low - high) * np.exp(-up / 0.07)
    haze = (low * np.ones((H, 1, 1)))
    col = lit * T + haze * (1 - T)
    col = sky_color * (1 - alpha[..., None]) + col * alpha[..., None]
    col = 1 - np.exp(-col * expo * 4)
    out = np.clip(col, 0, 1) ** (1 / 2.2)
    cv2.imwrite(dst, np.round(out[..., ::-1] * 255).astype(np.uint8))
    print(dst, out.shape)


main()
