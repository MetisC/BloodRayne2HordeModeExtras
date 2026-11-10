"""Build the standalone Dhampir player assets for Challenge Mode.

CMainCharacter::setup looks up a hardcoded list of player bones
(bone_Slerp_left/right weapon mounts, Jaw/Lip/Tounge/Brow/Eye face bones,
breasts) that the stock DHAMPIR_F skeleton (63 bones, enemy rig) lacks,
failing fatally.  This grafts every Rayne bone missing from DHAMPIR_F onto
the end of her bone table (indices 0-62 untouched, so her mesh weights,
attachments, move table and cape JUG physics all keep working), then emits
a matching .BFM with the bone count bumped and rig entries appended.

Clips bind to bones by name, so the appended bones animate from Rayne's
clip tracks exactly as authored.
"""

import argparse
from pathlib import Path
from struct import pack_into, unpack_from

import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "tools"))
from build_ephemera import SKB_BONE, Model, name_at, dampen_cape, RIG_ENTRY  # noqa: E402
from pod3 import Pod3  # noqa: E402

ASSET = "br2_dhampir_player"
BS = chr(92)


def fixed(value, size):
    encoded = value.encode("ascii")
    assert len(encoded) < size
    return encoded.ljust(size, b"\0")


def bone_names(skb):
    _, count = unpack_from("<2I", skb, 0)
    return [name_at(skb, 8 + SKB_BONE * i, 24) for i in range(count)]


def rec_at(skb, i, field):
    return skb, 8 + SKB_BONE * i + field


def build_skeleton(dhampir, rayne):
    d_names = bone_names(dhampir)
    r_names = bone_names(rayne)
    missing = [n for n in r_names if n not in d_names]
    if not missing:
        raise ValueError("dhampir skeleton already complete")
    new_names = d_names + missing
    bones_end = 8 + SKB_BONE * len(d_names)
    result = bytearray(dhampir[:bones_end])
    pack_into("<I", result, 4, len(new_names))
    for name in missing:
        rec = bytearray(rayne[8 + SKB_BONE * r_names.index(name):
                              8 + SKB_BONE * (r_names.index(name) + 1)])
        result.extend(rec)
    # Remap parent (+28) and mirror (+32) index fields for the appended
    # records; both hold indices into the rayne table
    for i, name in enumerate(missing):
        new_i = len(d_names) + i
        for field in (28, 32):
            old, = unpack_from("<i", *rec_at(rayne, r_names.index(name), field))
            n = new_names.index(r_names[old]) if old >= 0 else -1
            pack_into("<i", result, 8 + SKB_BONE * new_i + field, n)
    result.extend(dhampir[bones_end:])
    # Parents point at earlier bones and mirrored bones resolve.
    for i in range(len(new_names)):
        p, = unpack_from("<i", result, 36 + SKB_BONE * i)
        assert -1 <= p < i, f"bad parent for {new_names[i]} -> {p}"
        s, = unpack_from("<i", result, 40 + SKB_BONE * i)
        assert -1 <= s < len(new_names), f"bad mirror for {new_names[i]}"
    return bytes(result), new_names


def build_model(dbfm, rbfm, new_names):
    count = len(dbfm.bones)
    extra = len(new_names) - count
    if extra <= 0:
        raise ValueError("no bones to add")
    donor_count = len(rbfm.bones)
    donor_indices = {name: i for i, name in enumerate(rbfm.bones)}
    new_indices = {name: i for i, name in enumerate(new_names)}
    result = bytearray(dbfm.data[:dbfm.rig])
    # BFM stores positions, orientations, flags and orient-child indices in
    # parallel arrays. Expand each array before moving to the next one.
    offset = 0
    for stride in (12, 24, 4, 4):
        start = dbfm.rig + offset * count
        result.extend(dbfm.data[start:start + stride * count])
        for name in new_names[count:]:
            start = rbfm.rig + offset * donor_count + stride * donor_indices[name]
            field = rbfm.data[start:start + stride]
            if offset == 40:
                child, = unpack_from("<i", field)
                if child >= 0:
                    child = new_indices[rbfm.bones[child]]
                result.extend(child.to_bytes(4, "little", signed=True))
            else:
                result.extend(field)
        offset += stride

    descriptors_end = dbfm.meshes[-1][0] + 4
    result.extend(dbfm.data[dbfm.descriptors:descriptors_end])
    result.extend(bytes(-len(result) % 16))
    result.extend(dbfm.data[dbfm.geometry:])
    pack_into("<I", result, 12, count + extra)
    # every mesh packet embeds the skeleton bone count; Model.meshes stores
    # the offset of that field for each packet
    for offset, _, _ in dbfm.meshes:
        pack_into("<I", result, offset + RIG_ENTRY * extra, count + extra)
    result[32:112] = fixed(ASSET + ".skl", 80)
    return bytes(result)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--game", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    args = p.parse_args()
    common = Pod3(args.game / "COMMON.POD")
    models = Pod3(args.game / "W32MODEL.POD")
    d_skb = common.read(common.by_name["DATA" + BS + "DHAMPIR_F.SKB"])
    r_skb = common.read(common.by_name["DATA" + BS + "RAYNE.SKB"])
    skeleton, names = build_skeleton(d_skb, r_skb)
    dbfm = Model.read(models.read(models.by_name["MODELS" + BS + "DHAMPIR_F.BFM"]), d_skb)
    rbfm = Model.read(models.read(models.by_name["MODELS" + BS + "RAYNE.BFM"]), r_skb)
    model = build_model(dbfm, rbfm, names)
    player = Model.read(model, skeleton)
    count = len(player.bones)
    children = unpack_from(f"<{count}i", model, player.rig + 40 * count)
    stock_children = unpack_from(f"<{len(dbfm.bones)}i", dbfm.data, dbfm.rig + 40 * len(dbfm.bones))
    if children[:len(stock_children)] != stock_children or player.origins[:len(dbfm.bones)] != dbfm.origins:
        raise ValueError("Dhampir's stock rig changed")
    for i, child in enumerate(children):
        if child != -1 and not (0 <= child < count and
                                unpack_from("<i", skeleton, 8 + SKB_BONE * child + 28)[0] == i):
            raise ValueError(f"Invalid orient child for {player.bones[i]}")
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / (ASSET + ".skb")).write_bytes(skeleton)
    (args.out / (ASSET + ".bfm")).write_bytes(model)
    # Same cape fix as the appearance skin: dampened dhampir_f.jug physics.
    jug = common.read(common.by_name["MODELS" + BS + "DHAMPIR_F.JUG"])
    (args.out / (ASSET + ".jug")).write_bytes(dampen_cape(jug))
    print("Dhampir player assets validated:", len(names), "bones ->", args.out)


if __name__ == "__main__":
    main()
