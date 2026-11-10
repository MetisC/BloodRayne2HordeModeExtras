#!/usr/bin/env python3
"""Build NPC outfits with Rayne's animations and the original secondary rigs."""

import argparse
from dataclasses import dataclass
from math import acos, degrees, dist, isfinite, sqrt
from pathlib import Path
import re
from struct import pack, pack_into, unpack_from
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools"))
from pod3 import Pod3

HEADER = 112
PART = 58
ATTACHMENT = 76
MATERIAL = 360
RIG_ENTRY = 44
SKB_BONE = 72
VERTEX = 128
OUTFITS = {
    "ephemera": ("br2_ephemera_hair", ("h_",)),
    "kestral": ("br2_kestral", ("bone_l_hair", "bone_r_hair")),
    "dhampir_f": ("br2_dhampir_f", ("cape_",)),
    "ninjaf": ("br2_ninjaf", ()),
    "ferril": ("br2_ferril", ("bone_hair", "bone_l_hair", "bone_Lhair", "bone_r_hair", "bone_Rhair")),
    "fpunk_female": ("br2_fpunk_female", ()),
    "zguest_f": ("br2_zguest_f", ("skirt_",)),
    "zpunkf": ("br2_zpunkf", ("skirt_", "hair_")),
}
# Donor rigs whose rest skeleton diverges too much from Rayne's to keep her
# rest transforms. These conform the hybrid rig to the donor's rest pose.
CONFORM_RIG = {"ferril"}
FERRIL_DIGIT_SCALE = 0.36    # shortens digits along the bone axis only; cross-section untouched
FERRIL_REPOSE_FINGERS = True  # include fingers in the arm-chain repose pass
# Head, hair, torso, lower body and arms from the native FPunk selector.
PUNK_VARIANTS = (
    ("b", "a", "a", "a", "a"),
    ("a", "a", "c", "pants_a", "b"),
    ("c", "b", "d", "pants_b", "b"),
    ("b", "a", "b", "b", "a"),
    ("c", "c", "shirt_a_03", "pants_a", "shirt_b"),
    ("b", "a", "shirt_b_02", "pants_b", "shirt_a"),
    ("a", "b", "shirt_a_01", "pants_a", "shirt_a"),
    ("c", "a", "shirt_b_04", "pants_b", "shirt_b"),
    ("a", "c", "a", "b", "a"),
    ("b", "b", "b", "a", "b"),
)
# Head, dress, hair, mask, feather, earrings, necklace.
BALLROOM_VARIANTS = (
    ("a", "a", "a", "a", "a", "a", True),
    ("b", "b", "c", "c", "b", "b", False),
    ("a", "c", "b", "d", "a", "b", True),
    ("b", "d", "a", "e", "b", "a", False),
    ("a", "e", "c", "f", "a", "b", False),
    ("b", "f", "c", "h", "b", "a", True),
    ("b", "a", "b", "g", "a", "a", True),
    ("a", "c", "a", "i", "b", "b", False),
    ("b", "e", "b", "b", "a", "a", True),
    ("a", "f", "a", "b", "b", "b", False),
)
# Hair, face, torso, upper arm, forearm, waist, thigh, shin, boot, skirt, laces.
ZERENSKI_VARIANTS = (
    ("a", "a", "a", "a", "a", "a", "a", "a", "b", "a", True),
    ("b", "b", "d", "b", "a", "b", "b", "d", "d", "b", False),
    ("c", "c", "g", "d", "d", "b", "c", "c", "b", "c", False),
    ("d", "a", "h", "c", "c", "a", "a", "d", "d", "d", False),
    ("e", "b", "j", "e", "e", "b", "c", "d", "d", "a", False),
    ("b", "c", "l", "b", "a", "b", "b", "b", "b", "d", False),
    ("a", "b", "b", "c", "a", "a", "b", "a", "b", "a", True),
    ("d", "c", "e", "b", "c", "b", "a", "b", "d", "b", True),
    ("c", "a", "f", "d", "e", "a", "c", "a", "b", "c", False),
    ("e", "b", "i", "e", "d", "b", "b", "c", "d", "d", True),
)


def name_at(data, offset, size):
    return data[offset:offset + size].split(b"\0", 1)[0].decode("ascii")


def bone_origins(skeleton, rig, count):
    origins = []
    for i in range(count):
        parent = unpack_from("<i", skeleton, 36 + SKB_BONE * i)[0]
        if not -1 <= parent < i:
            raise ValueError("Invalid parent index")
        delta = unpack_from("<3f", rig, 12 * i)
        base = origins[parent] if parent >= 0 else (0.0, 0.0, 0.0)
        origins.append(tuple(a + b for a, b in zip(base, delta)))
    return origins


@dataclass
class Model:
    data: bytes
    bones: list
    parts: int
    attachments: int
    rig: int
    descriptors: int
    geometry: int
    meshes: list
    origins: list

    @classmethod
    def read(cls, data, skeleton):
        version, lods, part_count, count, materials, attachments, _, _ = unpack_from("<8I", data)
        if (version, lods) != (6, 1) or unpack_from("<2I", skeleton) != (12, count):
            raise ValueError("Unsupported BFM or skeleton")
        bone_names = [name_at(skeleton, 8 + SKB_BONE * i, 24) for i in range(count)]
        attached = HEADER + PART * part_count
        rig = attached + ATTACHMENT * attachments + MATERIAL * materials
        desc = rig + RIG_ENTRY * count
        origins = bone_origins(skeleton, data[rig:desc], count)
        mesh_count = unpack_from("<I", data, desc)[0]
        cursor = desc + 4
        meshes = []
        for _ in range(mesh_count):
            version, _, parts = unpack_from("<3I", data, cursor)
            cursor += 12 + parts * 2
            adjacent = unpack_from("<I", data, cursor)[0]
            cursor += 4 + adjacent * 2
            render, size, kind, vertices, triangles, bones = unpack_from("<6I", data, cursor)
            if (version, render, kind, bones) != (3, 2, 4, count) or size < vertices * VERTEX + triangles * 6:
                raise ValueError("Unsupported mesh packet")
            meshes.append((cursor + 20, size, vertices))
            cursor += 24
        geometry = (cursor + 15) & ~15
        if geometry + sum(m[1] for m in meshes) != len(data):
            raise ValueError("Mesh packets do not cover the BFM")
        return cls(data, bone_names, part_count, attached, rig, desc, geometry, meshes, origins)


def target_name(name):
    if name in ("bone_left_breast", "bone_right_breast"):
        return name.removeprefix("bone_")
    # Rayne's rig has only two phalanges per finger, so Ferril's claw
    # segment rides the middle phalanx.
    name = re.sub(r" Finger(\d)2$", r" Finger\g<1>1", name)
    if " Finger4" in name:
        # Rayne has no separate pinky tracks. Let the proximal and distal
        # segments follow the corresponding ring-finger curls.
        return name.replace(" Finger4", " Finger3")
    return name


def select_punk_variant(source, skeleton, head, hair, torso, lower, arms):
    # Native FPunk setup at 0x4F1450. Eyes, hands and teeth are always on.
    selected = set()
    if torso in ("a", "b"):
        selected.update(f"acc_spike_{i:02}" for i in range(1, 5))
    foot = "pants" if lower.startswith("pants_") else "a"
    for side, word in (("l", "left"), ("r", "right")):
        selected.update((f"eye_{word}", f"teeth_{word}"))
        if torso == "a":
            selected.add(f"acc_{side}catskin")
        if hair == "a":
            selected.add(f"acc_{side}hair_a")
        else:
            selected.update((f"acc_{side}hairTop_{hair}", f"acc_{side}hairBottom_{hair}"))
        selected.update(f"part_{side}{part}" for part in (
            f"headTop_{head}", f"headBottom_{head}", f"torsoTop_{torso}", f"torsoBottom_{torso}",
            f"waist_{lower}", f"thigh_{lower}", f"shin_{lower}", f"foot_{foot}",
            f"arm_{arms}", f"forearm_{arms}", "hand",
        ))
    return select_visible_parts(source, skeleton, selected)


def select_ballroom_guest(source, skeleton, head, dress, hair, mask, feather, earrings, necklace):
    # Female guest setup at 0x58B7FC: dresses D-F reuse arms and skirts A-C.
    trim = dict(zip("abcdef", "abcabc"))[dress]
    selected = {f"acc_mask_{mask}", f"acc_feather_{feather}"}
    for side in ("l", "r"):
        selected.update(f"part_{side}{part}" for part in (
            f"headTop_{head}", f"headBottom_{head}", f"torsoTop_{dress}", f"torsoBottom_{dress}",
            f"waist_{dress}", f"arm_{trim}", f"forearm_{trim}", f"hand_{trim}", "eye", "thigh", "shin",
        ))
        selected.update(f"acc_{side}{part}" for part in (
            f"dress_front_{trim}", f"dress_back_{trim}", f"hairTop_{hair}", f"hairBottom_{hair}",
            f"earring_{earrings}",
        ))
        if necklace:
            selected.add(f"acc_{side}neck")
    return select_visible_parts(source, skeleton, selected)


def select_zerenski_assassin(source, skeleton, hair, face, torso, arm, forearm, waist, thigh, shin, boot, skirt, laces):
    # CZPunkf setup at 0x697B40. The necklace, eyes, hands and teeth are fixed.
    selected = set()
    for side in ("L", "R"):
        selected.add(f"acc_{side}Necklace_a")
        if laces:
            selected.add(f"acc_{side}CorsetLace_a")
        selected.update(f"part_{side}{part}" for part in (
            f"Hair_{hair}", f"HeadTop_{face}", f"HeadBottom_{face}", f"TorsoTop_{torso}", f"TorsoBottom_{torso}",
            f"arm_{arm}", f"Forearm_{forearm}", f"Waist_{waist}", f"Thigh_{thigh}", f"Shin_{shin}", f"Foot_{boot}",
            f"Skirt_{skirt}", "Eye_a", "Hand_a", "Teeth_a",
        ))
    return select_visible_parts(source, skeleton, selected)


def numbered_outfit_models(outfit, source, skeleton, select_variant, recipes):
    # Preserve the filename and saved key of each original variant.
    return [(outfit if i == 1 else f"{outfit}_{i}", select_variant(source, skeleton, *recipe))
            for i, recipe in enumerate(recipes, start=1)]


def select_visible_parts(source, skeleton, selected):
    data = source.data
    part_indices = {}
    parts = bytearray()
    for i in range(source.parts):
        start = HEADER + PART * i
        name = name_at(data, start, 30)
        if name in selected:
            part_indices[i] = len(parts) // PART
            parts += data[start:start + PART]
    if len(part_indices) != len(selected):
        raise ValueError("Missing or duplicate parts in the outfit variant")

    meshes = []
    geometry = []
    descriptor = source.descriptors + 4
    cursor = source.geometry
    for bone_count, size, _ in source.meshes:
        version, material, count = unpack_from("<3I", data, descriptor)
        positive = unpack_from(f"<{count}H", data, descriptor + 12)
        exclusions = descriptor + 12 + 2 * count
        blocked_count = unpack_from("<I", data, exclusions)[0]
        blocked = unpack_from(f"<{blocked_count}H", data, exclusions + 4)
        visible = [part_indices[i] for i in positive if i in part_indices]
        # Native draw at 0x60572A needs any positive and no exclusion.
        if visible and not any(i in part_indices for i in blocked):
            meshes.append(pack(f"<3I{len(visible)}HI", version, material, len(visible), *visible, 0)
                          + data[exclusions + 4 + 2 * blocked_count:bone_count + 4])
            geometry.append(data[cursor:cursor + size])
        descriptor = bone_count + 4
        cursor += size
    if not meshes:
        raise ValueError("Outfit variant has no geometry")

    header = bytearray(data[:HEADER])
    pack_into("<I", header, 8, len(part_indices))
    result = header + parts + data[source.attachments:source.descriptors]
    result += pack("<I", len(meshes)) + b"".join(meshes)
    result += bytes((-len(result)) % 16) + b"".join(geometry)
    return Model.read(result, skeleton)


def outfit_rig(source, source_skb, rayne, rayne_skb, physics, secondary_prefixes, character):
    secondary = [i for i, name in enumerate(source.bones) if name.startswith(secondary_prefixes)]
    accessory_slots = [i for i, name in enumerate(rayne.bones)
                       if name.startswith(("hair_", "ring_", "ribbin_"))]
    stock_count = len(rayne.bones)
    extra_count = max(0, len(secondary) - len(accessory_slots))
    count = stock_count + extra_count
    if (secondary_prefixes and not secondary) or count > 120:
        raise ValueError("Unexpected secondary rig")

    accessory_slots.extend(range(stock_count, count))
    names = list(rayne.bones) + [""] * extra_count
    for source_id, target_id in zip(secondary, accessory_slots):
        names[target_id] = source.bones[source_id]
    target_indices = {name: i for i, name in enumerate(names)}
    mapping = [target_indices[target_name(name)] for name in source.bones]
    lines = physics.decode("ascii").split()
    # The unquoted bone list alternates names and settings. The collision
    # and link lists use quoted names, including names containing spaces.
    start = lines.index("boneList") + 1
    joint_count = int(lines[start - 2])
    physics_bones = set(lines[start:start + 2 * joint_count:2])
    physics_bones.update(re.findall(r'"([^"]+)"', physics.decode("ascii")))
    missing = physics_bones.difference(names)
    if missing:
        raise ValueError(f"Missing outfit physics bones: {sorted(missing)}")

    bones_end = 8 + SKB_BONE * stock_count
    skeleton = bytearray(rayne_skb[:bones_end] + bytes(SKB_BONE * extra_count) + rayne_skb[bones_end:])
    pack_into("<I", skeleton, 4, count)
    # BFM rig data is stored in parallel arrays. Extend each before remapping.
    rig = bytearray()
    cursor = rayne.rig
    for stride in (12, 24, 4, 4):
        end = cursor + stride * stock_count
        rig += rayne.data[cursor:end] + bytes(stride * extra_count)
        cursor = end
    source_count = len(source.bones)
    if character in CONFORM_RIG:
        # This donor's rest skeleton diverges from Rayne's too much to keep
        # her rest transforms. Each target bone takes its rest position and
        # orientation from the donor bone of the exact same name; hierarchy,
        # flags and linkage stay Rayne's. Donor bones only reachable through
        # a target_name remap contribute nothing.
        source_indices = {name: i for i, name in enumerate(source.bones)}
        for target_id in range(stock_count):
            source_id = source_indices.get(names[target_id])
            if source_id is None:
                continue
            rig[12 * target_id:12 * target_id + 12] = \
                source.data[source.rig + 12 * source_id:source.rig + 12 * source_id + 12]
            rig[12 * count + 24 * target_id:12 * count + 24 * target_id + 24] = \
                source.data[source.rig + 12 * source_count + 24 * source_id:
                            source.rig + 12 * source_count + 24 * source_id + 24]
    origins = bone_origins(skeleton, rig, count)
    if not secondary:
        return skeleton, rig, mapping, origins

    head = target_indices["Bip01 Head"]
    source_head = source.bones.index("Bip01 Head")
    for source_id in secondary:
        target_id = mapping[source_id]
        old = 8 + SKB_BONE * source_id
        new = 8 + SKB_BONE * target_id
        skeleton[new:new + SKB_BONE] = source_skb[old:old + SKB_BONE]
        parent, mirror = unpack_from("<ii", source_skb, old + 28)
        pack_into("<ii", skeleton, new + 28, mapping[parent], mapping[mirror] if mirror >= 0 else -1)
        delta = unpack_from("<3f", source.data, source.rig + 12 * source_id)
        if parent not in secondary:
            delta = tuple(a - b for a, b in zip(source.origins[source_id], origins[mapping[parent]]))
        pack_into("<3f", rig, 12 * target_id, *delta)
        for stride, size in ((12, 24), (36, 4)):
            old = source.rig + stride * source_count + size * source_id
            new = stride * count + size * target_id
            rig[new:new + size] = source.data[old:old + size]
        child = unpack_from("<i", source.data, source.rig + 40 * source_count + 4 * source_id)[0]
        pack_into("<i", rig, 40 * count + 4 * target_id, mapping[child] if child >= 0 else -1)

    # Keep Rayne's gameplay bone indices and animation table intact. The
    # remaining accessory slots are unused leaves on the hybrid rig.
    for target_id in accessory_slots[len(secondary):]:
        pack_into("<ii", skeleton, 36 + SKB_BONE * target_id, head, -1)
        pack_into("<i", rig, 36 * count + 4 * target_id, 2)
        pack_into("<i", rig, 40 * count + 4 * target_id, -1)
    source_child = unpack_from("<i", source.data, source.rig + 40 * source_count + 4 * source_head)[0]
    if source_child >= 0:
        pack_into("<i", rig, 40 * count + 4 * head, mapping[source_child])
    else:
        # This model has no head chain. Disable Rayne's head-to-hair link.
        pack_into("<i", rig, 36 * count + 4 * head, 2)
        pack_into("<i", rig, 40 * count + 4 * head, -1)
    origins = bone_origins(skeleton, rig, count)
    if any(dist(origins[mapping[i]], source.origins[i]) > 0.00001 for i in secondary):
        raise ValueError("Secondary rig rest positions changed")
    return skeleton, rig, mapping, origins


def rebind(source, rayne, source_skb, rayne_skb, physics, character):
    outfit, secondary_prefixes = OUTFITS[character]
    if character in CONFORM_RIG:
        source = reshape_donor(source, source_skb, rayne, rayne_skb)
    skeleton, rig, mapping, origins = outfit_rig(source, source_skb, rayne, rayne_skb, physics, secondary_prefixes, character)
    bone_count = len(origins)
    header_and_parts = bytearray(source.data[:source.attachments])
    attachment_data = rayne.data[rayne.attachments:rayne.attachments + 4 * ATTACHMENT]
    if [name_at(attachment_data, i * ATTACHMENT, 24) for i in range(4)] != ["bladeL", "bladeR", "gunL", "gunR"]:
        raise ValueError("Unexpected player attachment layout")
    pack_into("<I", header_and_parts, 12, bone_count)
    pack_into("<I", header_and_parts, 20, 4)
    header_and_parts[32:112] = (f"{outfit}.skl".encode("ascii").ljust(80, b"\0")
                                if secondary_prefixes else rayne.data[32:112])
    for i in range(source.parts):
        offset = HEADER + i * PART + 30
        bone = unpack_from("<i", source.data, offset)[0]
        if not 0 <= bone < len(mapping):
            raise ValueError("Invalid part bone")
        pack_into("<i", header_and_parts, offset, mapping[bone])

    attachment_count = unpack_from("<I", source.data, 20)[0]
    materials = source.data[source.attachments + attachment_count * ATTACHMENT:source.rig]
    if len(materials) != unpack_from("<I", source.data, 16)[0] * MATERIAL:
        raise ValueError("Unexpected outfit attachment layout")
    output = header_and_parts + attachment_data + materials + rig
    descriptors = bytearray(source.data[source.descriptors:source.geometry])
    for bone_count_offset, _, _ in source.meshes:
        pack_into("<I", descriptors, bone_count_offset - source.descriptors, bone_count)
    last_descriptor_end = source.meshes[-1][0] + 4 - source.descriptors
    output += descriptors[:last_descriptor_end]
    output += bytes((-len(output)) % 16)

    cursor = source.geometry
    vertex_count = 0
    max_error = 0.0
    for _, size, vertices in source.meshes:
        packet = bytearray(source.data[cursor:cursor + size])
        for i in range(vertices):
            start = i * VERTEX
            count = unpack_from("<I", packet, start)[0]
            weights = unpack_from("<4f", packet, start + 52)
            indices = unpack_from("<4I", packet, start + 80)
            if not 1 <= count <= 4:
                raise ValueError("Invalid vertex weight count")
            combined = {}
            original = [0.0] * 3
            for slot in range(count):
                source_index = indices[slot]
                target = mapping[source_index]
                weight = weights[slot]
                local = unpack_from("<3f", packet, start + 4 + 12 * slot)
                if not (isfinite(weight) and weight >= 0):
                    raise ValueError("Invalid influence")
                if weight == 0:
                    continue
                accumulator = combined.setdefault(target, [0.0] * 4)
                accumulator[3] += weight
                for axis in range(3):
                    position = local[axis] + source.origins[source_index][axis]
                    original[axis] += position * weight
                    accumulator[axis] += (position - origins[target][axis]) * weight
            if not combined:
                raise ValueError("Unweighted vertex")
            pack_into("<I", packet, start, len(combined))
            pack_into("<12f", packet, start + 4, *([0.0] * 12))
            pack_into("<4f", packet, start + 52, 0.0, 0.0, 0.0, 0.0)
            pack_into("<4I", packet, start + 80, 0, 0, 0, 0)
            neutral = [0.0] * 3
            for slot, (target, (*offset, weight)) in enumerate(combined.items()):
                local_at = start + 4 + slot * 12
                pack_into("<3f", packet, local_at, *(a / weight for a in offset))
                pack_into("<f", packet, start + 52 + 4 * slot, weight)
                pack_into("<I", packet, start + 80 + 4 * slot, target)
                for axis, coord in enumerate(unpack_from("<3f", packet, local_at)):
                    neutral[axis] += (coord + origins[target][axis]) * unpack_from("<f", packet, start + 52 + 4 * slot)[0]
            max_error = max(max_error, dist(original, neutral))
        cursor += size
        output += packet
        vertex_count += vertices

    if character in CONFORM_RIG:
        # The 24-byte per-bone block is an AABB (min/max) over that bone's
        # influence offsets. The donor's blocks no longer match after the
        # repose and reshaping, so rebuild them on the final packets.
        rig_start = len(header_and_parts) + len(attachment_data) + len(materials)
        boxes = {}
        cursor = len(output) - sum(size for _, size, _ in source.meshes)
        for _, size, vertices in source.meshes:
            for i in range(vertices):
                start = cursor + i * VERTEX
                slots = unpack_from("<I", output, start)[0]
                weights = unpack_from("<4f", output, start + 52)
                indices = unpack_from("<4I", output, start + 80)
                for slot in range(slots):
                    if weights[slot] == 0:
                        continue
                    local = unpack_from("<3f", output, start + 4 + 12 * slot)
                    box = boxes.setdefault(indices[slot],
                                           [float("inf")] * 3 + [float("-inf")] * 3)
                    for axis in range(3):
                        box[axis] = min(box[axis], local[axis])
                        box[3 + axis] = max(box[3 + axis], local[axis])
            cursor += size
        for bone, box in boxes.items():
            pack_into("<6f", output, rig_start + 12 * bone_count + 24 * bone, *box)

    rebuilt = Model.read(output, skeleton)
    if rebuilt.parts != source.parts or max_error > 0.00001:
        raise ValueError(f"Neutral shape changed by {max_error:.8f}")
    print(f"Rebound {vertex_count} vertices across {source.parts} parts. Neutral error {max_error:.8f}.")
    return output, skeleton


def repose_donor(source, source_skb, rayne, rayne_skb):
    """Re-pose the donor's arm chains onto Rayne's rest frames.

    The 9 floats at SKB record+36 are a 3x3 matrix whose columns are the
    bone's rest axes in world space; column 2 points along the bone toward
    its child. Rayne's animation tracks are authored against her own rest
    frames, so each arm bone b gets Q_b = M_rayne(ref(b)) * M_ferril(b)^T:
    child deltas rotate by their parent's Q, and the vertex offsets and
    normal/tangent/binormal vectors weighted to b rotate by Q_b. Bones
    outside the arm set keep identity Q, so only the arms move.
    """
    data = bytearray(source.data)
    count = len(source.bones)
    name_index = {name: i for i, name in enumerate(source.bones)}
    rayne_index = {name: i for i, name in enumerate(rayne.bones)}
    parents = [unpack_from("<i", source_skb, 36 + SKB_BONE * i)[0] for i in range(count)]
    identity = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]

    def dot(a, b):
        return sum(x * y for x, y in zip(a, b))

    def norm(v):
        return sqrt(dot(v, v))

    def normalize(v):
        length = norm(v) or 1.0
        return tuple(x / length for x in v)

    def columns(skb, i):
        m = unpack_from("<9f", skb, 44 + SKB_BONE * i)
        return [(m[c], m[3 + c], m[6 + c]) for c in range(3)]

    def orthogonalized(skb, i):
        # Anchor column 2, orthogonalize column 1 against it, column 0 = cross.
        c1, c2 = columns(skb, i)[1:3]
        c2 = normalize(c2)
        d = dot(c1, c2)
        c1 = normalize(tuple(a - d * b for a, b in zip(c1, c2)))
        c0 = (c1[1] * c2[2] - c1[2] * c2[1],
              c1[2] * c2[0] - c1[0] * c2[2],
              c1[0] * c2[1] - c1[1] * c2[0])
        return c0, c1, c2

    def rotation(f_cols, r_cols):
        # Q = M_rayne * M_ferril^T, given as row-major from column triples.
        return [[sum(r_cols[k][r] * f_cols[k][c] for k in range(3))
                 for c in range(3)] for r in range(3)]

    def mat_vec(m, v):
        return tuple(sum(m[r][k] * v[k] for k in range(3)) for r in range(3))

    def det3(m):
        return (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
                - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
                + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))

    repose = set()
    for side in "LR":
        for part in ("Clavicle", "UpperArm", "Forearm", "Hand"):
            repose.add(f"Bip01 {side} {part}")
        if FERRIL_REPOSE_FINGERS:
            for n in range(5):
                for segment in ("", "1", "2"):
                    repose.add(f"Bip01 {side} Finger{n}{segment}")
    missing = repose.difference(name_index)
    if missing:
        raise ValueError(f"Missing repose bones: {sorted(missing)}")

    tip = re.compile(r"Bip01 [LR] Finger[0-4]2$")
    quats = {}
    for i in sorted(name_index[name] for name in repose):
        name = source.bones[i]
        if tip.fullmatch(name):
            continue  # the claw segment follows the middle phalanx rigidly
        ref = name if name in rayne_index else name.replace("Finger4", "Finger3")
        if ref not in rayne_index:
            raise ValueError(f"No Rayne repose reference for {name}")
        f_cols = orthogonalized(source_skb, i)
        r_cols = orthogonalized(rayne_skb, rayne_index[ref])
        q = rotation(f_cols, r_cols)
        qtq = [[sum(q[r][k] * q[c][k] for k in range(3)) for c in range(3)]
               for r in range(3)]
        if abs(det3(q) - 1.0) > 1e-5 or max(
                abs(qtq[r][c] - (r == c)) for r in range(3) for c in range(3)) > 1e-5:
            raise ValueError(f"Repose rotation for {name} is not orthonormal")
        if name in rayne_index:
            mapped = mat_vec(q, f_cols[2])
            angle = degrees(acos(min(1.0, max(-1.0, dot(mapped, r_cols[2])))))
            if angle > 0.05:
                raise ValueError(f"Repose rotation for {name} misses the child axis by {angle:.4f} deg")
        quats[i] = q
    for i in sorted(name_index[name] for name in repose):
        name = source.bones[i]
        if tip.fullmatch(name):
            quats[i] = quats[name_index[name[:-1] + "1"]]

    # Rigid carry: every child delta rotates by its parent's rotation.
    for i in range(count):
        parent = parents[i]
        if parent < 0 or parent not in quats:
            continue
        delta = unpack_from("<3f", data, source.rig + 12 * i)
        pack_into("<3f", data, source.rig + 12 * i, *mat_vec(quats[parent], delta))

    # Vertex offsets rotate about their bone origin; the three rest-space
    # vectors rotate as a weight-blended linear repose.
    cursor = source.geometry
    for _, size, vertices in source.meshes:
        for i in range(vertices):
            start = cursor + i * VERTEX
            slots = unpack_from("<I", data, start)[0]
            weights = unpack_from("<4f", data, start + 52)
            indices = unpack_from("<4I", data, start + 80)
            if not any(indices[slot] in quats for slot in range(slots) if weights[slot] != 0):
                continue
            for slot in range(slots):
                if weights[slot] == 0 or indices[slot] not in quats:
                    continue
                local = unpack_from("<3f", data, start + 4 + 12 * slot)
                pack_into("<3f", data, start + 4 + 12 * slot, *mat_vec(quats[indices[slot]], local))
            for vector in (68, 104, 116):
                v = unpack_from("<3f", data, start + vector)
                if abs(norm(v) - 1.0) > 0.01:
                    continue  # degenerate vectors stay as authored
                blended = [0.0, 0.0, 0.0]
                for slot in range(slots):
                    if weights[slot] == 0:
                        continue
                    rotated = mat_vec(quats.get(indices[slot], identity), v)
                    for axis in range(3):
                        blended[axis] += weights[slot] * rotated[axis]
                if norm(blended) > 0.001:
                    pack_into("<3f", data, start + vector, *normalize(blended))
        cursor += size
    return Model.read(bytes(data), source_skb)


def reshape_donor(source, source_skb, rayne, rayne_skb):
    """Correct a donor model's proportions before rebinding.

    Ferril matches Rayne within ±5% everywhere except a digitigrade foot and
    claw-length fingers, so there is no global scale. Instead: drop the root
    so her pelvis sits at Rayne's rest height, shrink the foot about the
    ankle until she is grounded again, and shorten each phalanx along its
    own bone axis so the digits keep their authored thickness. Bone deltas
    and per-influence vertex offsets are the only position data touched;
    the 24-byte local bounds over-cover the shrunk parts, which is harmless.
    """
    source = repose_donor(source, source_skb, rayne, rayne_skb)
    data = bytearray(source.data)
    count = len(source.bones)
    name_index = {name: i for i, name in enumerate(source.bones)}
    parents = [unpack_from("<i", source_skb, 36 + SKB_BONE * i)[0] for i in range(count)]

    def scale_floats(offset, size, factor):
        values = unpack_from(f"<{size // 4}f", data, offset)
        pack_into(f"<{size // 4}f", data, offset, *(value * factor for value in values))

    def sub(a, b):
        return tuple(x - y for x, y in zip(a, b))

    def normalize(v):
        length = dist(v, (0.0, 0.0, 0.0)) or 1.0
        return tuple(x / length for x in v)

    def vertex_positions(model):
        cursor = model.geometry
        for mesh_index, (_, size, vertices) in enumerate(model.meshes):
            for i in range(vertices):
                start = cursor + i * VERTEX
                slots = unpack_from("<I", model.data, start)[0]
                weights = unpack_from("<4f", model.data, start + 52)
                indices = unpack_from("<4I", model.data, start + 80)
                pos = [0.0, 0.0, 0.0]
                for slot in range(slots):
                    if weights[slot] == 0:
                        continue
                    local = unpack_from("<3f", model.data, start + 4 + 12 * slot)
                    for axis in range(3):
                        pos[axis] += (local[axis] + model.origins[indices[slot]][axis]) * weights[slot]
                yield mesh_index, pos
            cursor += size

    def mesh_parts(model):
        parts = []
        cursor = model.descriptors + 4
        for _ in model.meshes:
            _, _, part_count = unpack_from("<3I", model.data, cursor)
            parts.append(set(unpack_from(f"<{part_count}H", model.data, cursor + 12)))
            cursor += 12 + part_count * 2
            adjacent = unpack_from("<I", model.data, cursor)[0]
            cursor += 4 + adjacent * 2 + 24
        return parts

    # Drop the root so the pelvis sits at Rayne's rest height. The pelvis is
    # the root, so its delta is the rig's world offset and the vertices,
    # stored as offsets from bone origins, follow for free.
    pelvis = name_index["Bip01 Pelvis"]
    drop = source.origins[pelvis][1] - rayne.origins[rayne.bones.index("Bip01 Pelvis")][1]
    delta_x, delta_y, delta_z = unpack_from("<3f", data, source.rig + 12 * pelvis)
    pack_into("<3f", data, source.rig + 12 * pelvis, delta_x, delta_y - drop, delta_z)

    # Shrink the digitigrade foot about the ankle so the lowest foot vertex
    # lands where Rayne's does. Toe0 and everything below it get the scaled
    # delta; the ankle itself is the pivot and stays.
    foot_part_ids = {i for i in range(source.parts)
                     if name_at(source.data, HEADER + i * PART, 30)
                     in ("part_lfoot", "part_rfoot")}
    parts = mesh_parts(source)
    lowest = min(pos[1] for mesh_index, pos in vertex_positions(source)
                 if parts[mesh_index] & foot_part_ids)
    ankles = (name_index["Bip01 L Foot"], name_index["Bip01 R Foot"])
    ankle_y = sum(source.origins[i][1] for i in ankles) / len(ankles)
    ground = min(pos[1] for _, pos in vertex_positions(rayne))
    foot_scale = (ground - (ankle_y - drop)) / (lowest - ankle_y)
    if not 0.60 < foot_scale < 0.66:
        raise ValueError(f"Unexpected Ferril foot scale {foot_scale:.4f}")
    below_ankle = {i for i, name in enumerate(source.bones)
                   if re.fullmatch(r"Bip01 [LR] Toe0", name)}
    changed = True
    while changed:
        changed = False
        for i in range(count):
            if i not in below_ankle and parents[i] in below_ankle:
                below_ankle.add(i)
                changed = True
    foot_bones = set(ankles) | below_ankle
    for i in below_ankle:
        scale_floats(source.rig + 12 * i, 12, foot_scale)

    # Shorten each phalanx along its own axis so the digits stay as thick as
    # authored. Finger{n}2 is a leaf and reuses its parent segment's axis.
    digit = {i for i, name in enumerate(source.bones)
             if re.fullmatch(r"Bip01 [LR] Finger[0-4][12]?", name)}
    if len(digit) != 30:
        raise ValueError("Unexpected Ferril digit rig")
    digit_axes = {}
    for i in digit:
        name = source.bones[i]
        if not re.fullmatch(r"Bip01 [LR] Finger[0-4]", name):
            continue
        middle, tip = name_index[name + "1"], name_index[name + "2"]
        digit_axes[i] = normalize(sub(source.origins[middle], source.origins[i]))
        digit_axes[middle] = digit_axes[tip] = normalize(sub(source.origins[tip], source.origins[middle]))
    for i in digit:
        if re.fullmatch(r"Bip01 [LR] Finger[0-4][12]", source.bones[i]):
            scale_floats(source.rig + 12 * i, 12, FERRIL_DIGIT_SCALE)

    # Foot slots scale isotropically; digit slots keep their perpendicular
    # component and rescale only the part along the bone axis.
    cursor = source.geometry
    for _, size, vertices in source.meshes:
        for i in range(vertices):
            start = cursor + i * VERTEX
            slots = unpack_from("<I", data, start)[0]
            indices = unpack_from("<4I", data, start + 80)
            for slot in range(slots):
                bone = indices[slot]
                if bone in foot_bones:
                    scale_floats(start + 4 + 12 * slot, 12, foot_scale)
                elif bone in digit_axes:
                    local = unpack_from("<3f", data, start + 4 + 12 * slot)
                    axis = digit_axes[bone]
                    along = sum(a * b for a, b in zip(local, axis))
                    pack_into("<3f", data, start + 4 + 12 * slot,
                              *(x + (FERRIL_DIGIT_SCALE - 1.0) * along * a
                                for x, a in zip(local, axis)))
        cursor += size
    return Model.read(bytes(data), source_skb)


def dampen_cape(physics):
    lines = physics.splitlines()
    if lines[:5] != [b"Version", b"13", b"boneCount", b"25", b"boneList"]:
        raise ValueError("Unexpected Dhampir JUG header")
    for i in range(5, 55, 2):
        params = lines[i + 1].split(b",")
        if not lines[i].startswith(b"cape_") or len(params) != 17:
            raise ValueError("Unexpected Dhampir cape joint")
        # Keep gravity, constraints and collisions. Reduce velocity retained
        # per simulation step and give the free ends a light restoring spring.
        params[1] = b"0.88"
        if params[0] == b"0":
            params[0] = b"35"
        lines[i + 1] = b",".join(params)
    return b"\r\n".join(lines) + b"\r\n"


def ballroom_skirt_physics(source, rayne_dress_physics):
    count = len(source.bones)
    joints = {}
    for i, name in enumerate(source.bones):
        if not name.startswith("skirt_") or name.endswith("00"):
            continue
        child = unpack_from("<i", source.data, source.rig + 40 * count + 4 * i)[0]
        if child >= 0:
            joints[name] = (source.origins[i], unpack_from("<3f", source.data, source.rig + 12 * child))
    # The waist is anchored. Give the remaining skirt joints Rayne's dress
    # gravity with a restoring spring and lower velocity retention.
    lines = ["Version", "13", "boneCount", str(len(joints)), "boneList"]
    for name, (_, delta) in joints.items():
        x, y, z = delta
        lines.extend((name, f"35,0.89,32,0,1,{x:.8g},{y:.8g},{z:.8g},0,0.1,-0.52,0.52,-0.52,0.52,0.5,0.75,1"))
    colliders = [line for line in rayne_dress_physics.decode("ascii").splitlines()
                 if re.match(r'^"Bip01 [LR] (Thigh|Calf|Foot)"', line)]
    if len(joints) != 15 or len(colliders) != 6:
        raise ValueError("Unexpected ballroom skirt rig or leg colliders")
    links = []
    for name in joints:
        if "_" in name.removeprefix("skirt_"):
            continue
        for side in ("L", "R"):
            side_name = f"{name[:-2]}_{side}{name[-2:]}"
            width = dist(joints[name][0], joints[side_name][0])
            links.append(f'"{name}","{side_name}",{width:.8g}')
    lines += ["collideBoneCount", str(len(colliders)), *colliders, "boneLinkCount", str(len(links)), *links]
    return ("\r\n".join(lines) + "\r\n").encode("ascii")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True, help="Output directory for the BFM and SKB")
    parser.add_argument("--character", choices=OUTFITS, default="ephemera")
    args = parser.parse_args()
    models = Pod3(args.game / "W32MODEL.POD")
    common = Pod3(args.game / "COMMON.POD")

    def get(archive, name):
        return archive.read(archive.by_name[name])

    rayne_skb = get(common, r"DATA\RAYNE.SKB")
    stock_name = args.character.upper()
    source_skb = get(common, fr"DATA\{stock_name}.SKB")
    source = Model.read(get(models, fr"MODELS\{stock_name}.BFM"), source_skb)
    rayne = Model.read(get(models, r"MODELS\RAYNE.BFM"), rayne_skb)
    outfit, secondary_prefixes = OUTFITS[args.character]
    physics_name = "RAYNE_ARMOR" if args.character == "ninjaf" else stock_name
    models_to_bind = [(outfit, source)]
    if args.character == "zguest_f":
        physics = ballroom_skirt_physics(source, get(common, r"MODELS\RAYNE_DRESS.JUG"))
        models_to_bind = numbered_outfit_models(outfit, source, source_skb, select_ballroom_guest, BALLROOM_VARIANTS)
    else:
        physics = get(common, fr"MODELS\{physics_name}.JUG")
    if args.character in ("ferril", "fpunk_female"):
        physics = physics.replace(b"bone_left_breast", b"left_breast").replace(b"bone_right_breast", b"right_breast")
    if args.character == "fpunk_female":
        models_to_bind = numbered_outfit_models(outfit, source, source_skb, select_punk_variant, PUNK_VARIANTS)
    elif args.character == "zpunkf":
        models_to_bind = numbered_outfit_models(outfit, source, source_skb, select_zerenski_assassin, ZERENSKI_VARIANTS)
    args.out.mkdir(parents=True, exist_ok=True)
    assets = []
    for name, model_source in models_to_bind:
        model, skeleton = rebind(model_source, rayne, source_skb, rayne_skb, physics, args.character)
        assets.append((f"{name}.BFM", model))
        if secondary_prefixes and name == outfit:
            assets.append((f"{name}.SKB", skeleton))
    if args.character == "dhampir_f":
        assets.append((f"{outfit}.JUG", dampen_cape(physics)))
    elif args.character in ("ferril", "fpunk_female", "zguest_f"):
        assets.append((f"{outfit}.JUG", physics))
    for name, data in assets:
        path = args.out / name.upper()
        path.write_bytes(data)
        print(path)


if __name__ == "__main__":
    main()
