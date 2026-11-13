"""Builds the Ocarina of Time VR room backdrop from a 6-direction capture at each spot (oot_vr_test.py --capture).

python -I tools/nx2/oot_vr_room.py --capture D:/lxc/capture --out <user dir>/oot_vr_room

Writes room.bin (vertices and triangles in metres around the centre spot, then the faces as RGBA pixels:
one row per spot, six faces across) and room.png (the same pixels, to look at). Both come from your own game: keep them out of the repository.
"""
import argparse
import json
import math
import os
import struct
import sys

import compression.zstd
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from oot_vr_test import CAPTURE_FACES, CAPTURE_SPOTS  # noqa: E402

# The capture's projection: near 0.6 m, far 1000 m (CaptureNearM in svc_nx2_oot_vr.cpp), clip depth -1..1 stored as (z + 1) / 2.
NEAR, FAR = 0.6, 1000.0
FACE_SIZE = 1024
MAGIC = b"OVR3"


def entries(face_dir):
    with open(os.path.join(face_dir, "manifest.jsonl"), encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]


def pick(face_dir):
    """The presented picture (HUD hidden in capture mode) and the full-size depth target the frame used most."""
    found = entries(face_dir)
    colour = [e for e in found if e.get("kind") == "present" and "png" in e][-1]
    depths = [e for e in found if e.get("kind") == "final_depth" and "raw" in e]
    size = max(e["width"] * e["height"] for e in depths)
    depth = max((e for e in depths if e["width"] * e["height"] == size), key=lambda e: e["uses"])
    return colour, depth


def distances(face_dir, depth):
    raw = compression.zstd.decompress(open(os.path.join(face_dir, depth["raw"]), "rb").read())
    w, h = depth["width"], depth["height"]
    values = struct.unpack(f"<{w * h}f", raw[: w * h * 4])
    a = (FAR + NEAR) / (FAR - NEAR)
    b = 2 * FAR * NEAR / (FAR - NEAR)
    return w, h, [b / max(a - (2 * d - 1), 1e-6) for d in values]


def rotate(yaw, pitch, v):
    """Turns a view-space vector (x right, y up, -z forward) by pitch up, then yaw left."""
    x, y, z = v
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    y, z = y * cp - z * sp, y * sp + z * cp
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    return x * cy + z * sy, y, -x * sy + z * cy


def unrotate(yaw, pitch, v):
    """The inverse of rotate: a room vector into the view space of the face at yaw and pitch."""
    x, y, z = v
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    x, z = x * cy - z * sy, x * sy + z * cy
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    return x, y * cp + z * sp, -y * sp + z * cp


def hidden_from_centre(centre, point, margin):
    """True when the centre capture saw a surface in front of point (so point fills a hole)."""
    for (yaw, pitch), (w, h, dist) in centre.items():
        x, y, z = unrotate(yaw, pitch, point)
        if -z <= max(abs(x), abs(y)):
            continue
        px = min(int((x / -z + 1) / 2 * w), w - 1)
        py = min(int((1 - y / -z) / 2 * h), h - 1)
        return -z > dist[py * w + px] * margin
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--capture", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--grid", type=int, default=128, help="vertices across each face")
    ap.add_argument("--edge", type=float, default=1.15, help="largest distance ratio inside one triangle")
    ap.add_argument("--near", type=float, default=0.6, help="metres: nearer triangles (Link's own hat and shield) are left out")
    ap.add_argument("--hidden", type=float, default=1.05,
                    help="an extra spot's triangle is kept when it is this many times farther than the centre's surface")
    ap.add_argument("--skirt", type=int, default=3,
                    help="grid cells past each face edge, with the edge's colour, to close the cracks between faces")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    spots = [s for s in CAPTURE_SPOTS if os.path.isdir(os.path.join(a.capture, f"{s[0]}_front"))]
    faces = len(CAPTURE_FACES)
    atlas = Image.new("RGB", (FACE_SIZE * faces, FACE_SIZE * len(spots)))
    vertices, indices, skin = [], [], []
    centre = {}
    n = a.grid
    for r, (spot, sx, sy, sz) in enumerate(spots):
        extra = r > 0
        kept = 0
        for f, (name, yaw, pitch) in enumerate(CAPTURE_FACES):
            face_dir = os.path.join(a.capture, f"{spot}_{name}")
            colour, depth = pick(face_dir)
            atlas.paste(Image.open(os.path.join(face_dir, colour["png"])).convert("RGB").resize(
                (FACE_SIZE, FACE_SIZE), Image.LANCZOS), (f * FACE_SIZE, r * FACE_SIZE))
            w, h, dist = distances(face_dir, depth)
            if not extra:
                centre[(yaw, pitch)] = (w, h, dist)
            first = len(vertices)
            grid = []
            cells = n + 2 * a.skirt
            for j in range(cells + 1):
                for i in range(cells + 1):
                    u, v = (i - a.skirt) / n, (j - a.skirt) / n
                    tu, tv = min(max(u, 0.0), 1.0), min(max(v, 0.0), 1.0)
                    z = dist[min(int(tv * h), h - 1) * w + min(int(tu * w), w - 1)]
                    x, y, zz = rotate(yaw, pitch, ((2 * u - 1) * z, (1 - 2 * v) * z, -z))
                    vertices.append((x + sx, y + sy, zz + sz, (f + tu) / faces, (r + tv) / len(spots)))
                    grid.append(z)
            middle = (cells // 2) * (cells + 1) + cells // 2
            print(f"{spot} {name}: centre distance {grid[middle]:.2f} m")
            for j in range(cells):
                for i in range(cells):
                    q = [j * (cells + 1) + i, j * (cells + 1) + i + 1, (j + 1) * (cells + 1) + i,
                         (j + 1) * (cells + 1) + i + 1]
                    for tri in ((q[0], q[2], q[1]), (q[1], q[2], q[3])):
                        zs = [grid[t] for t in tri]
                        if max(zs) >= FAR * 0.9 or min(zs) < a.near:
                            continue
                        if max(zs) / min(zs) <= a.edge:
                            if extra:
                                mid = [sum(vertices[first + t][k] for t in tri) / 3 for k in range(3)]
                                if not hidden_from_centre(centre, mid, a.hidden):
                                    continue
                                kept += 1
                            indices += [first + t for t in tri]
                            continue
                        if not extra:
                            skin += [first + t for t in tri]
        if extra:
            print(f"{spot}: {kept} triangles the centre could not see")
    atlas.save(os.path.join(a.out, "room.png"))
    with open(os.path.join(a.out, "room.bin"), "wb") as out:
        indices = skin + indices
        out.write(MAGIC + struct.pack("<IIIII", len(vertices), len(indices), len(skin), *atlas.size))
        for vertex in vertices:
            out.write(struct.pack("<5f", *vertex))
        out.write(struct.pack(f"<{len(indices)}I", *indices))
        out.write(atlas.convert("RGBA").tobytes())
    print(f"wrote {len(vertices)} vertices, {len(indices) // 3} triangles ({len(skin) // 3} skin across depth edges) to {a.out}")


if __name__ == "__main__":
    main()
