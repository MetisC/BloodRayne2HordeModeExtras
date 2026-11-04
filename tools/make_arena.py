#!/usr/bin/env python3
# make_arena.py — builds a wave-combat arena out of a stock story level.
#
# Recipe (validated on A1_MANSION_PART1_BATTLE_*):
#   - copy the level's .LVL
#   - point its script dependency at our generated .scb
#   - on the chosen CSpawnTrigger: enemiesDead -> flagon(WaveDoneIt),
#     maxNumberSpawn -> the per-variant enemy count, and
#     desiredNumberOfActiveEnemiesInArea -> a paced concurrency ladder
#   - write one minimal wave script shared by every variant of that arena:
#     warp the player inside the arena room, raise the room emitters,
#     poll WaveDoneIt, then slow-mo + chainToLevel(MENU_LEVEL.lvl)
#
# The generated .scb is the same token container the game ships:
#   dword(1) dword(numTokens) dword(bodyLen) + NUL-separated tokens.
#
# Usage:
#   python make_arena.py --src extracted/common_mod/WORLD --dst world \
#       --lvl A2_MEATPACKING.LVL --name chb_mp --wp wp_arenaD_rayne4 \
#       --trigger SpawnTrig_Wave_ArenaD \
#       --setup "raise(Emitters_ArenaD_On)" "raise(Emitters_Rooftops_Off)" \
#       [--killflag waveComplete] [--music music_wavefight2_loop.wav]

import argparse
import os
import re
import struct

COUNTS = (15, 25, 50, 100)
DESIRED = (3, 4, 6, 8)   # concurrent-enemy ladder per count tier


def build_scb(tokens):
    body = b"\x00".join(t.encode("cp1252") for t in tokens) + b"\x00"
    return struct.pack("<3I", 1, len(tokens), len(body)) + body


def wave_script(wp, setup, killflag, music):
    t = [
        "setLevelDescription(MissionDescription)",
        "flagOff(tutorials)",
        "setMusic()",
        "flagOff(%s)" % killflag,
        "fade(255,0,0,0,0)",
        "warpTo($,%s)" % wp,
    ]
    t += setup
    if music:
        t.append("SetMusic(%s)" % music)
    t += [
        "fade(0,0,0,0,1)",
        ":mainloop",
        "idle",
        "if (%s)" % killflag,
        "{",
        "idle",
        "setTimeFactor(0.2)",
        "flagOff(%s)" % killflag,
        "wait(2)",
        "setTimeFactor(1)",
        "idle",
        "chainToLevel(MENU_LEVEL.lvl)",
        "return",
        "}",
        "goto mainloop",
    ]
    return t


def make_lvl(src_text, lvl_base, scb_base, trigger, count, desired, killflag):
    d = src_text
    # point the script dependency at the shared generated scb
    d, n = re.subn(r"world\\[A-Za-z0-9_]+\.script",
                   lambda m: "world\\%s.script" % scb_base, d, count=1)
    assert n == 1, "script dep not found in %s" % lvl_base
    # retarget the chosen spawn trigger block
    pat = re.compile(r'(\{ CSpawnTrigger "%s".*?)(\} CSpawnTrigger)' %
                     re.escape(trigger), re.S)
    m = pat.search(d)
    assert m, "trigger %s not found in %s" % (trigger, lvl_base)
    blk = m.group(1)
    blk, n1 = re.subn(r'^\t\d+(\s*// maxNumberSpawn)',
                      '\t%d\\1' % count, blk, count=1, flags=re.M)
    blk, n2 = re.subn(r'^\t\d+(\s*// desiredNumberOfActiveEnemiesInArea)',
                      '\t%d\\1' % desired, blk, count=1, flags=re.M)
    blk, n3 = re.subn(r'\t"[^"]*"(\s*// enemiesDead)',
                      '\t"flagon(%s);"\\1' % killflag, blk, count=1)
    assert n1 == 1 and n3 == 1, \
        "trigger fields not matched in %s (%d %d %d)" % (lvl_base, n1, n2, n3)
    d = d[:m.start(1)] + blk + d[m.end(1):]
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--dst", required=True)
    ap.add_argument("--lvl", required=True, help="source story .LVL name")
    ap.add_argument("--name", required=True, help="output base, e.g. chb_mp")
    ap.add_argument("--wp", required=True, help="player warp waypoint inside arena")
    ap.add_argument("--trigger", required=True, help="CSpawnTrigger name")
    ap.add_argument("--setup", nargs="*", default=[],
                    help="extra script tokens after the warp")
    ap.add_argument("--killflag", default="WaveDoneIt")
    ap.add_argument("--music", default="music_wavefight2_loop.wav",
                    help="combat track; '' keeps the level ambient")
    a = ap.parse_args()

    lvl_base = os.path.basename(a.lvl)
    stem = lvl_base.rsplit(".", 1)[0]
    src = os.path.join(a.src, lvl_base)
    if not os.path.isfile(src):
        # try case-insensitive match inside src dir
        for f in os.listdir(a.src):
            if f.lower() == lvl_base.lower():
                src = os.path.join(a.src, f)
                break
    text = open(src, "rb").read().decode("cp1252")

    scb = build_scb(wave_script(a.wp, a.setup, a.killflag, a.music))
    out_scb = os.path.join(a.dst, a.name + ".scb")
    open(out_scb, "wb").write(scb)
    print("scb    ->", os.path.basename(out_scb))

    suffix = {15: "e", 25: "n", 50: "h", 100: "x"}
    for count, desired in zip(COUNTS, DESIRED):
        out_name = "%s_%s.lvl" % (a.name, suffix[count])
        lvl = make_lvl(text, lvl_base, a.name, a.trigger, count, desired,
                       a.killflag)
        out = os.path.join(a.dst, out_name)
        open(out, "wb").write(lvl.encode("cp1252"))
        print("lvl    ->", out_name, "(%d enemies, %d active)" %
              (count, desired))


if __name__ == "__main__":
    main()
