#!/usr/bin/env python3
"""Build the 'Estándar (PS2)' outfit: clone MODELS\\RAYNE.BFM and repoint the
body texture names at the additive RAYNE_PS2* set.

The game itself ships the official PS2-skin textures inside W32ART3.POD under
ART\\PS2\\ (32-bit, 512x1024 - the artwork the 2006 'PS2 Hidden Skin' mod
recreated). We deploy them as loose ART\\RAYNE_PS2*.TEX files; the engine
loads disk files before the PODs.

Usage:
    build_rayne_ps2.py --pod W32MODEL.POD --art-pod W32ART3.POD \
        --models-out <game>/models --art-out <game>/art

Everything stays additive: stock PODs, BFM and TEX files are never touched.
"""
import argparse
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
POD3 = os.path.join(HERE, "..", "..", "..", "tools", "pod3.py")

# RAYNE.BFM texture-name slots (fixed ~48-byte buffers, NUL-terminated).
# Verified offsets inside stock RAYNE.BFM (621,574 bytes, W32MODEL.POD).
NAME_SLOTS = [
    (0x814, "RAYNE.tif", "RAYNE_PS2.tif"),
    (0x85C, "RAYNE_bumpmap.tif", "RAYNE_PS2_bumpmap.tif"),
    (0x8A4, "RAYNE_glossmap.tif", "RAYNE_PS2_glossmap.tif"),
]
NAME_FIELD = 0x48  # record stride / buffer capacity seen in the BFM


def pod_extract(pod, outdir, pattern):
    subprocess.run([sys.executable, POD3, "extract", pod, outdir, pattern],
                   check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pod", required=True, help="path to W32MODEL.POD")
    ap.add_argument("--art-pod", required=True, help="path to W32ART3.POD")
    ap.add_argument("--stock-art-pod", required=True,
                    help="path to W32ART.POD (stock bump map source)")
    ap.add_argument("--models-out", required=True, help="game models\\ dir")
    ap.add_argument("--art-out", required=True, help="game art\\ dir")
    ap.add_argument("--work", default=os.path.join(HERE, "rayne_ps2_work"),
                    help="scratch dir for POD extracts")
    args = ap.parse_args()

    os.makedirs(args.work, exist_ok=True)
    pod_extract(args.pod, args.work, "RAYNE.BFM")
    pod_extract(args.art_pod, args.work, "PS2")
    # The PS2 set has no bump map: reuse the stock one (same mesh, same
    # normals) under the new name so the skin owns its own file.
    pod_extract(args.stock_art_pod, args.work, "RAYNE_BUMPMAP.TEX")

    src = os.path.join(args.work, "MODELS", "RAYNE.BFM")
    data = bytearray(open(src, "rb").read())

    for offset, old, new in NAME_SLOTS:
        old_b = old.encode("ascii")
        new_b = new.encode("ascii")
        assert data[offset:offset + len(old_b)] == old_b, \
            f"expected {old!r} at {offset:#x}, found {data[offset:offset+32]!r}"
        assert len(new_b) < NAME_FIELD, f"{new!r} exceeds name buffer"
        data[offset:offset + NAME_FIELD] = new_b + b"\x00" * (NAME_FIELD - len(new_b))

    os.makedirs(args.models_out, exist_ok=True)
    dst = os.path.join(args.models_out, "BR2_RAYNE_PS2.BFM")
    open(dst, "wb").write(data)
    print(f"wrote {dst} ({len(data)} bytes)")

    os.makedirs(args.art_out, exist_ok=True)
    for src_name, dst_name, subdir in [
        ("RAYNE.TEX", "RAYNE_PS2.TEX", "PS2"),
        ("RAYNE_GLOSSMAP.TEX", "RAYNE_PS2_GLOSSMAP.TEX", "PS2"),
        ("RAYNE_BUMPMAP.TEX", "RAYNE_PS2_BUMPMAP.TEX", ""),  # stock bump
    ]:
        src_file = os.path.join(args.work, "ART", *([subdir] if subdir else []),
                                src_name)
        dst_file = os.path.join(args.art_out, dst_name)
        if not os.path.exists(src_file):
            print(f"skip {dst_name}: {src_name} not in POD")
            continue
        open(dst_file, "wb").write(open(src_file, "rb").read())
        print(f"wrote {dst_file}")


if __name__ == "__main__":
    main()
