"""Build stock enemy weapons with player-compatible collision names.

Same recipe as make_dhampir_weapons.py: CAnimatedMeleeWeapon (player weapon
type 0) looks up parts named 'norm_collision', 'rage_collision1' and
'fury_collision2' inside the blade model and fails fatally when they are
missing.

KAGAN: keep both stock collision meshes and both FX markers. Reuse the
larger collision mesh for Fury as a third real collision entry.

DHAMPIR: keep the original scimitar mesh and marker. Reuse its stock
collision hull for Normal, Rage and Fury.

NINJA: NINJAM_BLADE.SMB (shared by both ninja variants) ships no collision
meshes. Add three real meshes using the stock Kagan sword hull fitted to the
ninja blade bounds. Duplicate its static pose so the player weapon instance
allocates part matrices. Preserve the original parts, markers and render data.
"""

import argparse
import struct
import sys
from pathlib import Path

GAME = Path(r"H:\GOG\BloodRayne 2 Terminal Cut")
sys.path.insert(0, str(GAME / "MODDING" / "tools"))
from pod3 import Pod3  # noqa: E402

KAGAN = "MODELS\\WEAPON_KAGAN_SWORD.SMB"
NINJA = "MODELS\\NINJAM_BLADE.SMB"
DHAMPIR_SCIMITAR = "MODELS\\DHAMPIR_SCIMITAR.SMB"
COLLISION_NAMES = (b"norm_collision", b"rage_collision1", b"fury_collision2")
NAME_SIZE = 32
PART_HEADER_SIZE = NAME_SIZE + 2 + 52
TRANSFORM_SIZE = 28


def model_header(data: bytes) -> tuple[int, int, int, int]:
    version, parts, collisions, markers, materials, frames = struct.unpack_from("<6I", data)
    assert version == 110 and frames == 1
    return parts, collisions, markers, 28 + materials * 360


def collision_meshes(data: bytes, start: int, count: int) -> tuple[list[bytes], int]:
    meshes = []
    for _ in range(count):
        mesh = start + NAME_SIZE
        version, vertices, triangles = struct.unpack_from("<3I", data, mesh)
        assert version == 3 and vertices > 0 and triangles > 0
        start = mesh + 12 + vertices * 12 + triangles * 7
        meshes.append(data[mesh:start])
    return meshes, start


def kagan_sword(source: bytes) -> bytes:
    _, count, _, start = model_header(source)
    assert count == 2 and source.startswith(b"collision\0", start)
    (normal, rage), _ = collision_meshes(source, start, count)
    assert source.startswith(b"collision1\0", start + NAME_SIZE + len(normal))
    return animated_sword(source, (normal, rage, rage))


def dhampir_scimitar(source: bytes) -> bytes:
    _, count, _, start = model_header(source)
    assert count == 1
    (hull,), _ = collision_meshes(source, start, count)
    return animated_sword(source, (hull,) * len(COLLISION_NAMES))


def ninja_sword(source: bytes, donor: bytes) -> bytes:
    parts, collisions, markers, collision_start = model_header(source)
    assert collisions == 0 and markers == 2
    part_start = collision_start + markers * NAME_SIZE + 24
    part_offsets = [part_start + i * PART_HEADER_SIZE for i in range(parts)]
    assert all(struct.unpack_from("<I", source, pos + NAME_SIZE + 2)[0] == 3
               for pos in part_offsets)
    blade = next(pos for pos in part_offsets if source.startswith(b"blade_intact\0", pos))
    target_bounds = struct.unpack_from("<6f", source, blade + NAME_SIZE + 6)

    _, donor_collisions, _, donor_start = model_header(donor)
    assert donor_collisions >= 1 and donor.startswith(b"collision\0", donor_start)
    mesh_start = donor_start + NAME_SIZE
    version, vertex_count, triangle_count = struct.unpack_from("<3I", donor, mesh_start)
    assert version == 3 and vertex_count > 0 and triangle_count > 0
    vertex_start = mesh_start + 12
    triangle_start = vertex_start + vertex_count * 12
    vertices = list(struct.iter_unpack("<3f", donor[vertex_start:triangle_start]))
    axes = list(zip(*vertices))
    low = [min(axis) for axis in axes]
    high = [max(axis) for axis in axes]
    assert all(hi > lo for lo, hi in zip(low, high))
    scales = [(target_bounds[i + 3] - target_bounds[i]) / (high[i] - low[i])
              for i in range(3)]
    fitted_vertices = b"".join(struct.pack("<3f", *(
        target_bounds[i] + (point[i] - low[i]) * scales[i] for i in range(3)
    )) for point in vertices)
    # Each triangle has three 16-bit indices followed by one byte of flags per
    # triangle, in a separate array (CCollisionMesh::loadBinary, 0x57C540).
    triangles = donor[triangle_start:triangle_start + triangle_count * 7]
    mesh = donor[mesh_start:vertex_start] + fitted_vertices + triangles
    return animated_sword(source, (mesh,) * len(COLLISION_NAMES))


def animated_sword(source: bytes, meshes: tuple[bytes, ...]) -> bytes:
    parts, collisions, markers, collision_start = model_header(source)
    _, collision_end = collision_meshes(source, collision_start, collisions)
    part_start = collision_end + markers * NAME_SIZE + 24
    # Parent indices are shared by all frames, followed by one complete array
    # of quaternion/position pairs per frame. Collisions precede the markers.
    existing_parts = parts + collisions
    parent_start = part_start + parts * PART_HEADER_SIZE
    transform_start = parent_start + 2 * (existing_parts + markers)
    marker_parents = parent_start + existing_parts * 2
    marker_transforms = transform_start + existing_parts * TRANSFORM_SIZE
    frame_end = marker_transforms + markers * TRANSFORM_SIZE
    payload_start = (frame_end + 15) & ~15
    assert len(meshes) == len(COLLISION_NAMES) and len(meshes) >= collisions
    added_count = len(meshes) - collisions
    pose = (source[transform_start:marker_transforms]
            + struct.pack("<7f", 1, 0, 0, 0, 0, 0, 0) * added_count
            + source[marker_transforms:frame_end])
    # CModelInstance allocates matrices only for animated models (0x57CE4D).
    # The player blade unconditionally reads them for its collision bounds.
    frame_count = 2
    result = bytearray(source[:collision_start])
    struct.pack_into("<I", result, 8, len(meshes))
    struct.pack_into("<I", result, 20, frame_count)
    for name, mesh in zip(COLLISION_NAMES, meshes):
        result.extend(name.ljust(NAME_SIZE, b"\0") + mesh)
    result.extend(source[collision_end:marker_parents])
    result.extend(struct.pack("<h", -1) * added_count)
    result.extend(source[marker_parents:transform_start])
    result.extend(pose * frame_count)
    result.extend(bytes(-len(result) % 16))
    result.extend(source[payload_start:])
    return bytes(result)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--weapon", choices=("all", "kagan", "ninja", "scimitar"), default="all")
    parser.add_argument("--game", type=Path, default=GAME)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    models = Pod3(args.game / "W32MODEL.POD")
    out = args.out or args.game / "models"
    out.mkdir(parents=True, exist_ok=True)
    kagan = models.read(models.by_name[KAGAN])
    if args.weapon in ("all", "kagan"):
        (out / "br2_kagan_sword.smb").write_bytes(kagan_sword(kagan))
    if args.weapon in ("all", "ninja"):
        ninja = models.read(models.by_name[NINJA])
        (out / "br2_ninja_blade.smb").write_bytes(ninja_sword(ninja, kagan))
    if args.weapon in ("all", "scimitar"):
        scimitar = models.read(models.by_name[DHAMPIR_SCIMITAR])
        (out / "br2_dhampir_scimitar.smb").write_bytes(dhampir_scimitar(scimitar))


if __name__ == "__main__":
    main()
