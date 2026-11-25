"""Converts the models that move through the image from glTF into the form the program loads.

Run with Blender (4.x) in the background:

    blender --background --factory-startup --python tools/models/convert.py -- <folder with the sources> [name ...]

The sources are the unpacked glTF downloads named in models.json (one folder each, containing scene.gltf). For every
model this writes models/<name>.mesh and models/<name>.png:

  .mesh  head (magic SBM2, counts, extent, wing measures), then the vertices with 28 bytes each (position in meters:
         right, up, forward; normal as three signed bytes and one that marks thin material; texture position; color
         factor of the material with its layer in a), then the indices with 32 bits, then the lamps with 13 numbers
         each (position, color times strength, axis and cosine of the half angle of the sector it shines into, period
         and lit fraction of its flashing, phase).
  .png   the surface colors: one square layer per image the materials use, one below the other.

What models.json says per model:

  source     folder of the download
  forward    axis that points to the bow or nose, as Blender shows the model after import (+X, -Y, ...)
  up         axis that points up
  length     length in meters the model is scaled to; or scale: factor on the size as it is
  draft      meters between the lowest point and the water line (ships); the water line becomes height 0
  center     without draft: name of the part whose middle becomes the origin (default: the middle of everything)
  skip       parts whose name contains one of these are left out
  triangles  the mesh is reduced to about this many
  layer      edge length of a layer of the surface colors in pixels
  wing       birds: half width of the body, length of a wing (measured sideways), height of the wing root, meters
  thin       materials that light shines through: sailcloth, feathers
  lights     lamps for the night: kind (see LAMPS), then position in meters in the finished model (right, up,
             forward), optionally the phase of its flashing (0 to 1)
  crew       people set into the model, each in a fixed pose: source (a .glb file below the folder with the
             sources), forward and up as above, action (name of an animation in it) and frame, height in meters
             when standing, at (where the hips go, meters in the finished model: right, up, forward), turn (where
             the person faces, degrees clockwise from forward seen from above) and colors (linear color per
             material name; the others keep theirs)
"""
import json
import math
import os
import struct
import sys

import bpy
import numpy as np
from mathutils import Vector

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, '..', '..', 'models'))
AXES = {'+X': (1, 0, 0), '-X': (-1, 0, 0), '+Y': (0, 1, 0), '-Y': (0, -1, 0), '+Z': (0, 0, 1), '-Z': (0, 0, -1)}
SIDE = (math.sin(math.radians(56.25)), 0.0, math.cos(math.radians(56.25)))
# The lamps there are: color, axis of the sector they shine into (right, up, forward), its half angle in degrees,
# period of the flashing in seconds (0: steady), lit fraction of it, strength. Ships and aircraft carry the same
# set by international rule: white masthead lights that shine ahead and to both sides, red to port and green to
# starboard, white astern; aircraft add a red beacon and white flashes.
LAMPS = {
    'masthead': ((1.0, 0.94, 0.82), (0, 0, 1), 112.5, 0, 1, 1.0),
    'port': ((1.0, 0.05, 0.03), (-SIDE[0], 0, SIDE[2]), 56.25, 0, 1, 0.4),
    'starboard': ((0.04, 1.0, 0.4), SIDE, 56.25, 0, 1, 0.4),
    'stern': ((1.0, 0.94, 0.82), (0, 0, -1), 67.5, 0, 1, 0.5),
    'allround': ((1.0, 0.94, 0.82), (0, 1, 0), 180, 0, 1, 0.012),
    'cabin': ((1.0, 0.7, 0.4), (0, 1, 0), 180, 0, 1, 0.15),
    # An aircraft is seen from much nearer than a ship, and its position lights are weaker.
    'left': ((1.0, 0.05, 0.03), (-SIDE[0], 0, SIDE[2]), 56.25, 0, 1, 0.05),
    'right': ((0.04, 1.0, 0.4), SIDE, 56.25, 0, 1, 0.05),
    'tail': ((1.0, 0.94, 0.82), (0, 0, -1), 67.5, 0, 1, 0.04),
    'beacon': ((1.0, 0.05, 0.03), (0, 1, 0), 180, 1.0, 0.12, 0.2),
    'flash': ((1.0, 1.0, 1.0), (0, 1, 0), 180, 1.2, 0.05, 0.5),
    'landing': ((1.0, 0.97, 0.9), (0, 0, 1), 14, 0, 1, 2.5),
}


def materials_of(gltf_path):
    """Per material name: color factor, image file or None, and whether its transparency counts."""
    g = json.load(open(gltf_path, encoding='utf-8'))
    images = [i.get('uri') for i in g.get('images', [])]
    textures = [images[t['source']] if 'source' in t else None for t in g.get('textures', [])]
    out = {}
    for m in g.get('materials', []):
        pbr = m.get('pbrMetallicRoughness', {})
        tex = pbr.get('baseColorTexture')
        out[m.get('name')] = (pbr.get('baseColorFactor', [1, 1, 1, 1]), textures[tex['index']] if tex else None,
                              m.get('alphaMode', 'OPAQUE') != 'OPAQUE')
    return out


def layer_pixels(path, size, see_through):
    """An image as size x size RGBA bytes, rows from the top."""
    img = bpy.data.images.load(path)
    img.scale(size, size)
    px = np.empty(size * size * 4, dtype=np.float32)
    img.pixels.foreach_get(px)
    bpy.data.images.remove(img)
    px = px.reshape(size, size, 4)[::-1]
    if not see_through:
        px[:, :, 3] = 1
    return np.clip(np.rint(px * 255), 0, 255).astype(np.uint8)


def figure(entry, sources):
    """A person in a fixed pose, in the frame of the finished model: per corner of its triangles position, normal
    and color (linear)."""
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=os.path.join(sources, entry['source']))
    new = [o for o in bpy.data.objects if o not in before]
    arm = next(o for o in new if o.type == 'ARMATURE')
    body = [o for o in new if o.type == 'MESH' and o.parent == arm]

    def corners():
        depsgraph = bpy.context.evaluated_depsgraph_get()
        pos, nrm, col = [], [], []
        for o in body:
            ev = o.evaluated_get(depsgraph)
            me = ev.to_mesh()
            me.calc_loop_triangles()
            world = np.array(ev.matrix_world)
            co = np.empty(len(me.vertices) * 3, dtype=np.float32)
            me.vertices.foreach_get('co', co)
            co = co.reshape(-1, 3) @ world[:3, :3].T + world[:3, 3]
            cn = np.empty(len(me.loops) * 3, dtype=np.float32)
            me.corner_normals.foreach_get('vector', cn)
            cn = cn.reshape(-1, 3) @ np.linalg.inv(world[:3, :3])
            tri_loops = np.empty(len(me.loop_triangles) * 3, dtype=np.int32)
            me.loop_triangles.foreach_get('loops', tri_loops)
            tri_verts = np.empty(len(me.loop_triangles) * 3, dtype=np.int32)
            me.loop_triangles.foreach_get('vertices', tri_verts)
            tri_mat = np.empty(len(me.loop_triangles), dtype=np.int32)
            me.loop_triangles.foreach_get('material_index', tri_mat)
            colors = []
            for m in o.data.materials:
                c = entry.get('colors', {}).get(m.name) if m else None
                if c is None:
                    bsdf = m.node_tree.nodes.get('Principled BSDF') if m and m.use_nodes else None
                    c = list(bsdf.inputs['Base Color'].default_value)[:3] if bsdf else [0.5, 0.5, 0.5]
                colors.append(c)
            pos.append(co[tri_verts])
            nrm.append(cn[tri_loops])
            col.append(np.array(colors, dtype=np.float64)[np.repeat(tri_mat, 3)])
            ev.to_mesh_clear()
        return np.concatenate(pos), np.concatenate(nrm), np.concatenate(col)

    # The size from the person standing at rest, then the pose.
    data = arm.animation_data or arm.animation_data_create()
    for track in list(data.nla_tracks):
        data.nla_tracks.remove(track)
    arm.data.pose_position = 'REST'
    rest, _, _ = corners()
    up = np.array(AXES[entry['up']], dtype=np.float64)
    scale = entry['height'] / float(np.ptp(rest @ up))
    arm.data.pose_position = 'POSE'
    action = next(a for a in bpy.data.actions if a.name.split('|')[-1] == entry['action'])
    data.action = action
    if hasattr(data, 'action_slot') and getattr(action, 'slots', None):
        data.action_slot = action.slots[0]
    bpy.context.scene.frame_set(int(entry.get('frame', 0)))
    pos, nrm, col = corners()
    hips = np.array(arm.matrix_world @ arm.pose.bones['Hips'].head)
    for o in new:
        bpy.data.objects.remove(o, do_unlink=True)

    f = np.array(AXES[entry['forward']], dtype=np.float64)
    frame = np.stack([np.cross(f, up), up, f])
    turn = math.radians(entry.get('turn', 0))
    # Clockwise seen from above: forward turns toward the right.
    spin = np.array([[math.cos(turn), 0, math.sin(turn)], [0, 1, 0], [-math.sin(turn), 0, math.cos(turn)]])
    pos = ((pos - hips) @ frame.T) * scale @ spin.T + np.array(entry['at'])
    nrm = nrm @ frame.T @ spin.T
    nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
    return pos, nrm, col


def convert(name, spec, sources):
    src = os.path.join(sources, spec['source'])
    gltf = os.path.join(src, 'scene.gltf')
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=gltf)
    scene = bpy.context.scene
    skip = spec.get('skip', [])
    parts = [o for o in scene.objects if o.type == 'MESH' and not any(s in o.name for s in skip)]
    if not parts:
        raise RuntimeError('no mesh in ' + gltf)

    # The middle that becomes the origin, before the parts are joined.
    def extent(objects):
        lo, hi = np.full(3, 1e30), np.full(3, -1e30)
        for o in objects:
            for c in o.bound_box:
                w = np.array(o.matrix_world @ Vector(c))
                lo, hi = np.minimum(lo, w), np.maximum(hi, w)
        return lo, hi
    lo, hi = extent(parts)
    middle = (lo + hi) / 2
    if 'center' in spec:
        clo, chi = extent([o for o in parts if spec['center'] in o.name])
        middle = (clo + chi) / 2

    bpy.ops.object.select_all(action='DESELECT')
    for o in parts:
        o.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    if len(parts) > 1:
        bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    before = sum(len(p.vertices) - 2 for p in obj.data.polygons)
    want = spec.get('triangles', 0)
    if want and before > want:
        mod = obj.modifiers.new('reduce', 'DECIMATE')
        mod.ratio = want / before
        bpy.ops.object.modifier_apply(modifier=mod.name)
    mod = obj.modifiers.new('triangles', 'TRIANGULATE')
    bpy.ops.object.modifier_apply(modifier=mod.name)

    mesh = obj.data
    count = len(mesh.loops)
    corner_vertex = np.empty(count, dtype=np.int32)
    mesh.loops.foreach_get('vertex_index', corner_vertex)
    co = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get('co', co)
    world = np.array(obj.matrix_world)
    pos = co.reshape(-1, 3)[corner_vertex] @ world[:3, :3].T + world[:3, 3]
    normal = np.empty(count * 3, dtype=np.float32)
    mesh.corner_normals.foreach_get('vector', normal)
    normal = normal.reshape(-1, 3) @ np.linalg.inv(world[:3, :3])
    uv = np.zeros(count * 2, dtype=np.float32)
    if mesh.uv_layers.active:
        mesh.uv_layers.active.data.foreach_get('uv', uv)
    uv = uv.reshape(-1, 2)
    uv[:, 1] = 1 - uv[:, 1]
    start = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get('loop_start', start)
    slot = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get('material_index', slot)
    corner_slot = np.repeat(slot, 3)
    order = (start[:, None] + np.arange(3)[None, :]).ravel()
    pos, normal, uv = pos[order], normal[order], uv[order]

    # Into the frame of the program: right, up, forward. Right is forward x up, which
    # reverses the handedness; the triangles are turned around to match.
    f, u = np.array(AXES[spec['forward']], dtype=np.float64), np.array(AXES[spec['up']], dtype=np.float64)
    frame = np.stack([np.cross(f, u), u, f])
    long_ = float((hi - lo) @ np.abs(f))
    scale = spec['length'] / long_ if 'length' in spec else spec.get('scale', 1.0)
    origin = middle.copy()
    pos = ((pos - origin) @ frame.T) * scale
    if 'draft' in spec:
        pos[:, 1] -= pos[:, 1].min() + spec['draft']
    normal = normal @ frame.T
    normal /= np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-9)

    # Surface colors: one layer per image, a white one for materials without.
    known = materials_of(gltf)
    size = spec.get('layer', 256)
    layers, index = [], {}
    tint = np.zeros((len(mesh.materials) or 1, 4), dtype=np.uint8)
    tint[:] = (255, 255, 255, 0)
    thin = np.zeros(len(tint), dtype=np.int8)
    for i, m in enumerate(mesh.materials):
        factor, image, see_through = known.get(m.name if m else None, ([1, 1, 1, 1], None, False))
        thin[i] = 127 if m and m.name in spec.get('thin', []) else 0
        key = (image, see_through)
        if key not in index:
            index[key] = len(layers)
            if image:
                layers.append(layer_pixels(os.path.join(src, image), size, see_through))
            else:
                layers.append(np.full((size, size, 4), 255, dtype=np.uint8))
        # The factor is linear; the program reads the vertex color as sRGB like an image.
        srgb = [255 * (c ** (1 / 2.2)) for c in factor[:3]]
        tint[i] = (round(srgb[0]), round(srgb[1]), round(srgb[2]), index[key])
    corner_tint, corner_thin = tint[corner_slot], thin[corner_slot]

    # People on board, colored by their materials on a white layer.
    for entry in spec.get('crew', []):
        fpos, fnrm, fcol = figure(entry, sources)
        key = (None, False)
        if key not in index:
            index[key] = len(layers)
            layers.append(np.full((size, size, 4), 255, dtype=np.uint8))
        ftint = np.zeros((len(fpos), 4), dtype=np.uint8)
        ftint[:, :3] = np.clip(np.rint(255 * np.power(np.clip(fcol, 0, 1), 1 / 2.2)), 0, 255)
        ftint[:, 3] = index[key]
        pos = np.concatenate([pos, fpos])
        normal = np.concatenate([normal, fnrm])
        uv = np.concatenate([uv, np.zeros((len(fpos), 2), dtype=uv.dtype)])
        corner_tint = np.concatenate([corner_tint, ftint])
        corner_thin = np.concatenate([corner_thin, np.zeros(len(fpos), dtype=corner_thin.dtype)])
        print('CREW %s: %d triangles at %s' % (entry['action'], len(fpos) // 3, entry['at']))

    # Triangles turned around, equal corners merged.
    turn = (np.arange(len(pos)).reshape(-1, 3)[:, ::-1]).ravel()
    pos, normal, uv, corner_tint, corner_thin = pos[turn], normal[turn], uv[turn], corner_tint[turn], corner_thin[turn]
    packed = np.zeros(len(pos), dtype=[('p', '<f4', 3), ('n', 'i1', 4), ('t', '<f4', 2), ('c', 'u1', 4)])
    packed['p'] = pos
    packed['n'][:, :3] = np.clip(np.rint(normal * 127), -127, 127)
    packed['n'][:, 3] = corner_thin
    packed['t'] = uv
    packed['c'] = corner_tint
    assert packed.dtype.itemsize == 28
    vertices, indices = np.unique(packed, return_inverse=True)
    lo, hi = pos.min(axis=0), pos.max(axis=0)
    wing = spec.get('wing', [0, 0, 0])
    lamps = []
    for entry in spec.get('lights', []):
        color, axis, half, period, lit, strength = LAMPS[entry[0]]
        phase = entry[4] if len(entry) > 4 else 0.0
        lamps.append(struct.pack('<13f', *entry[1:4], *[c * strength for c in color], *axis, math.cos(math.radians(half)) if half < 180 else -2.0,
                                 period, lit, phase))
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, name + '.mesh'), 'wb') as out:
        out.write(struct.pack('<4s3I9fI', b'SBM2', len(vertices), len(indices), len(layers), *lo, *hi, *wing, len(lamps)))
        out.write(vertices.tobytes())
        out.write(indices.astype('<u4').tobytes())
        out.write(b''.join(lamps))
    sheet = np.concatenate(layers, axis=0)
    img = bpy.data.images.new(name, size, size * len(layers), alpha=True)
    img.pixels.foreach_set((sheet[::-1].astype(np.float32) / 255).ravel())
    img.filepath_raw = os.path.join(OUT, name + '.png')
    img.file_format = 'PNG'
    img.save()
    print('MODEL %s: %d of %d triangles, %d vertices, %d layers of %d px, %d lamps, extent %s .. %s m' % (
        name, len(indices) // 3, before, len(vertices), len(layers), size, len(lamps), np.round(lo, 2).tolist(), np.round(hi, 2).tolist()))


def main():
    argv = sys.argv[sys.argv.index('--') + 1:]
    specs = json.load(open(os.path.join(HERE, 'models.json'), encoding='utf-8'))
    for name in argv[1:] or list(specs):
        convert(name, specs[name], argv[0])


main()
