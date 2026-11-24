"""Turns a star catalogue and a map of the Milky Way into the files the program carries.

Usage:
  convert.py stars <stars.bin> <faintest magnitude> <catalogue> [<catalogue> ...]
  convert.py milkyway <map.exr> <milkyway.jpg> [width]

stars: a catalogue is one of these, told apart by its first line:
- an extract of the Hipparcos catalogue (ESA 1997) from the ESA archive,
  https://gea.esac.esa.int/archive/, table public.hipparcos, as CSV with the
  columns hip, vmag, ra, de, pmra, pmde, b_v. The archive returns at most 50000
  rows per query, so the extract comes in parts, for example
    SELECT hip,vmag,ra,de,pmra,pmde,b_v FROM public.hipparcos WHERE vmag<7.8
  and likewise from 7.8 to 8.6 and from 8.6 to 9.05;
- the main table of the same catalogue as the data centers carry it
  (hip_main.dat, catalogue I/239);
- the Bright Star Catalogue, 5th revised edition (bsc5.dat, Hoffleit and Warren
  1991, catalogue V/50).
Written are all stars up to the faintest magnitude, brightest first: the mark
SBS1, the number of stars, then eight bytes per star: right ascension as a
fraction of the circle in 16 bits, declination as a fraction of the right
angle in 16 bits with sign, magnitude in thousandths, color index B-V in
fiftieths, one byte unused. Places are for the equator and equinox of J2000;
the stars of Hipparcos are moved on by their proper motion from 1991.25 to
2026.

milkyway: the map is milkyway_2020_4k.exr from "Deep Star Maps 2020" of NASA's
Scientific Visualization Studio (https://svs.gsfc.nasa.gov/4851): the glow of
the stars too faint to be seen one by one, in celestial coordinates. Written
is an image with right ascension to the right from 0 h and declination from
+90 at the top, its brightest region at 1, as the square root of the
brightness in 8 bits.
"""
import math
import struct
import sys


def hipparcos(fields):
    """A star of Hipparcos from its magnitude, place, proper motion and B-V as text: right ascension and declination
    in degrees for 2026, magnitude, B-V. None without a place or a magnitude."""
    mag, ra, dec, pmra, pmde, bv = fields
    try:
        mag, ra, dec = float(mag), float(ra), float(dec)
    except ValueError:
        return None
    try:
        # Proper motion in thousandths of a second of arc per year; in right
        # ascension it is given times the cosine of the declination.
        years = 2026.0 - 1991.25
        ra += float(pmra) * years / 3.6e6 / max(math.cos(math.radians(dec)), 1e-6)
        dec += float(pmde) * years / 3.6e6
    except ValueError:
        pass
    try:
        bv = float(bv)
    except ValueError:
        bv = 0.6
    return ra % 360.0, max(-90.0, min(90.0, dec)), mag, bv


def extract(lines):
    for line in lines[1:]:
        f = line.split(',')
        if len(f) == 7:
            yield hipparcos(f[1:])


def main_table(lines):
    for line in lines:
        f = line.split('|')
        if len(f) >= 38:
            yield hipparcos((f[5], f[8], f[9], f[12], f[13], f[37]))


def bright(lines):
    for line in lines:
        try:
            ra = (int(line[75:77]) + int(line[77:79]) / 60 + float(line[79:83]) / 3600) * 15
            dec = int(line[84:86]) + int(line[86:88]) / 60 + int(line[88:90]) / 3600
            mag = float(line[102:107])
        except ValueError:
            continue
        if line[83] == '-':
            dec = -dec
        try:
            bv = float(line[109:114])
        except ValueError:
            bv = 0.6
        yield ra, dec, mag, bv


def stars(target, faintest, sources):
    found = []
    for source in sources:
        with open(source, encoding='latin-1') as f:
            lines = f.read().splitlines()
        if lines[0].startswith('hip,vmag,ra,de,pmra,pmde,b_v'):
            rows = extract(lines)
        elif lines[0].startswith('H|'):
            rows = main_table(lines)
        else:
            rows = bright(lines)
        found += [(row[2], row[0], row[1], row[3]) for row in rows if row and row[2] <= faintest]
    found.sort()
    with open(target, 'wb') as f:
        f.write(b'SBS1' + struct.pack('<I', len(found)))
        for mag, ra, dec, bv in found:
            f.write(struct.pack('<HhhbB', int(ra / 360 * 65536) & 0xffff, round(dec / 90 * 32767), round(mag * 1000),
                                max(-127, min(127, round(bv * 50))), 0))
    print(len(found), 'stars up to magnitude', faintest)
    for mag, ra, dec, bv in found[:5]:
        print(f'  {mag:6.2f}  RA {ra / 15:7.4f} h  Dec {dec:8.4f}  B-V {bv:5.2f}')
    for limit in range(2, int(faintest) + 1):
        print(f'  brighter than {limit}: {sum(1 for star in found if star[0] < limit)}')


def read_exr(path):
    """An OpenEXR image as NASA's maps are: scan lines, ZIP compression, half floats. Returns rows x columns x 3, red first."""
    import zlib

    import numpy as np

    data = open(path, 'rb').read()
    if struct.unpack('<I', data[:4])[0] != 20000630:
        sys.exit('not an OpenEXR file: ' + path)
    head, pos = {}, 8
    while data[pos] != 0:
        end = data.index(b'\0', pos)
        kind = data.index(b'\0', end + 1)
        size = struct.unpack('<I', data[kind + 1:kind + 5])[0]
        head[data[pos:end].decode()] = data[kind + 5:kind + 5 + size]
        pos = kind + 5 + size
    pos += 1
    x0, y0, x1, y1 = struct.unpack('<4i', head['dataWindow'])
    w, h = x1 - x0 + 1, y1 - y0 + 1
    names, at, listed = [], 0, head['channels']
    while listed[at] != 0:
        end = listed.index(b'\0', at)
        names.append(listed[at:end].decode())
        if struct.unpack('<i', listed[end + 1:end + 5])[0] != 1:
            sys.exit('only half floats are read')
        at = end + 17
    packing = head['compression'][0]
    if packing not in (2, 3):
        sys.exit('only ZIP compression is read')
    per = 16 if packing == 3 else 1
    blocks = (h + per - 1) // per
    out = np.zeros((h, w, len(names)), np.float32)
    for start in struct.unpack(f'<{blocks}Q', data[pos:pos + 8 * blocks]):
        y, size = struct.unpack('<ii', data[start:start + 8])
        raw = data[start + 8:start + 8 + size]
        rows = min(per, h - (y - y0))
        full = rows * w * len(names) * 2
        if size < full:
            # Each byte is stored as its difference to the one before, and the two
            # halves of the block are the even and the odd bytes.
            packed = np.frombuffer(zlib.decompress(raw), np.uint8).astype(np.int64)
            packed[1:] -= 128
            packed = (np.cumsum(packed) & 255).astype(np.uint8)
            half = (full + 1) // 2
            plain = np.empty(full, np.uint8)
            plain[0::2] = packed[:half]
            plain[1::2] = packed[half:]
            raw = plain.tobytes()
        block = np.frombuffer(raw, '<f2').reshape(rows, len(names), w)
        out[y - y0:y - y0 + rows] = block.transpose(0, 2, 1)
    return out[:, :, [names.index(c) for c in 'RGB']]


def milkyway(source, target, width):
    import numpy as np
    from PIL import Image

    img = np.maximum(read_exr(source), 0)
    # The map has right ascension growing to the left with 0 h in the middle; the
    # program reads it growing to the right from the left edge.
    img = np.roll(img[:, ::-1], img.shape[1] // 2, axis=1)
    # What few single pixels reach does not set the scale.
    top = float(np.percentile(img.max(axis=2), 99.9))
    out = Image.fromarray(np.round(np.sqrt(np.clip(img / top, 0, 1)) * 255).astype(np.uint8))
    if width and width != out.width:
        out = out.resize((width, width // 2), Image.LANCZOS)
    if target.lower().endswith(('.jpg', '.jpeg')):
        out.save(target, quality=93, subsampling=0)
    else:
        out.save(target, optimize=True)
    print(out.width, 'x', out.height, 'brightest region', top)


if __name__ == '__main__':
    if len(sys.argv) < 4 or sys.argv[1] not in ('stars', 'milkyway'):
        sys.exit(__doc__)
    if sys.argv[1] == 'stars':
        if len(sys.argv) < 5:
            sys.exit(__doc__)
        stars(sys.argv[2], float(sys.argv[3]), sys.argv[4:])
    else:
        milkyway(sys.argv[2], sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 0)
