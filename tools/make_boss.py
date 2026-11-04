#!/usr/bin/env python3
# make_boss.py — packages a stock story boss fight as a fixed challenge.
#
#   - copies the boss level's .LVL under a mod-owned name (never the stock
#     name: a loose file with the story name would hijack the campaign)
#   - repoints its script dependency to a regenerated .scb
#   - the .scb is the story script verbatim except every chainToLevel/
#     chainTolevel target becomes menu_level.lvl, so killing the boss
#     returns to the challenge menu instead of continuing the campaign
#
# Usage:
#   python make_boss.py --src extracted/common_mod/WORLD --dst world \
#       --lvl A1_MANSION_ROOFARENA.LVL --scb A1_MANSION_ROOFARENA.SCB \
#       --name chb_boss_zerenski

import argparse
import os
import re
import struct


def repack_scb(raw, retarget):
    body = raw[12:]
    toks = body.split(b"\x00")
    out = []
    for t in toks:
        m = re.match(rb"(chainTo[Ll]evel)\(([^)]+\.lvl)\)", t)
        if m:
            t = m.group(1) + b"(" + retarget.encode() + b")"
        out.append(t)
    nb = b"\x00".join(out)
    return struct.pack("<3I", 1, len([x for x in out if x]), len(nb)) + nb


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--dst", required=True)
    ap.add_argument("--lvl", required=True)
    ap.add_argument("--scb", default=None, help="defaults to <lvl base>.scb")
    ap.add_argument("--name", required=True)
    ap.add_argument("--retarget", default="menu_level.lvl")
    a = ap.parse_args()

    lvl_base = os.path.basename(a.lvl)
    scb_base = a.scb or (lvl_base.rsplit(".", 1)[0] + ".SCB")

    def find(base):
        p = os.path.join(a.src, base)
        if os.path.isfile(p):
            return p
        for f in os.listdir(a.src):
            if f.lower() == base.lower():
                return os.path.join(a.src, f)
        raise SystemExit("not found: " + base)

    lvl = open(find(lvl_base), "rb").read().decode("cp1252")
    scb = open(find(scb_base), "rb").read()

    lvl, n = re.subn(r"world\\[A-Za-z0-9_]+\.script",
                     lambda m: "world\\%s.script" % a.name, lvl, count=1)
    assert n == 1, "script dep not found in " + lvl_base
    out_lvl = os.path.join(a.dst, a.name + ".lvl")
    open(out_lvl, "wb").write(lvl.encode("cp1252"))
    print("lvl ->", os.path.basename(out_lvl))

    out_scb = os.path.join(a.dst, a.name + ".scb")
    open(out_scb, "wb").write(repack_scb(scb, a.retarget))
    print("scb ->", os.path.basename(out_scb))


if __name__ == "__main__":
    main()
