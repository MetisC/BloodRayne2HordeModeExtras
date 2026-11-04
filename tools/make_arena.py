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
import math
import os
import re
import struct

COUNTS = (15, 25, 50, 100)
DESIRED = (3, 4, 6, 8)   # concurrent-enemy ladder per count tier


def build_scb(tokens):
    body = b"\x00".join(t.encode("cp1252") for t in tokens) + b"\x00"
    return struct.pack("<3I", 1, len(tokens), len(body)) + body


def wave_script(wp, setup, arm, killflag, music, weapons=()):
    t = [
        "setLevelDescription(MissionDescription)",
        "flagOff(tutorials)",
        "setMusic()",
        "flagOff(%s)" % killflag,
        "fade(255,0,0,0,0)",
        "warpTo($,%s)" % wp,
    ]
    # Rayne's blades/bloodguns are standalone actors (carriedByActor=(none)):
    # they auto-equip on proximity, so warping only the hero would leave them
    # at the story spawn -- warp every hero weapon alongside, like story does
    t += ["warpTo(%s,%s)" % (w, wp) for w in weapons]
    t += setup
    # createActor instantiates the object, activateTrigger arms it -- the
    # native battle script (A1_MANSION_PART1_BATTLE) runs exactly this pair
    t += arm
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


TRIG_BLK = re.compile(r'(\{ CSpawnTrigger "([^"]+)".*?)(\} CSpawnTrigger)',
                      re.S)


def trig_fields(block):
    """pos, triggerSize, testRadius, emitter names from a trigger block."""
    pos = re.search(r'\n\t([^\n]+?)\s+// pos', block)
    size = re.search(r'\n\t([^\n]+?)\s+// triggerSize', block)
    rad = re.search(r'\n\t(\d+)\s+// testRadius', block)
    emitters = re.findall(r'\t"([^"]+)"\s*// text', block)
    return (pos.group(1) if pos else "0,0,0",
            size.group(1) if size else "50,50,50",
            rad.group(1) if rad else "200",
            [e for e in emitters if e != "(none)"])


def actor_pos(text, cls, name):
    m = re.search(r'\{ %s "%s".*?\n\t([^\n]+?)\s+// pos' %
                  (cls, re.escape(name)), text, re.S)
    return [float(x) for x in m.group(1).split(',')] if m else None


def make_lvl(src_text, lvl_base, scb_base, trigger, count, desired, killflag,
             inject_blk=None, inject_name=None, wp=None, keep_emitters=None,
             ring_r=None):
    d = src_text
    # point the script dependency at the shared generated scb
    d, n = re.subn(r"world\\[A-Za-z0-9_]+\.script",
                   lambda m: "world\\%s.script" % scb_base, d, count=1)
    assert n == 1, "script dep not found in %s" % lvl_base
    # the loader picks the .SCB by the level's ROOT NAME -- retarget it to our
    # generated scb or the stock story script would run instead of ours
    d, n = re.subn(r'(// Root name of level\r?\n)[^\r\n]+',
                   lambda m: m.group(1) + scb_base, d, count=1)
    assert n == 1, "root name not found in %s" % lvl_base

    if inject_blk is not None:
        # user-proven recipe: transplant the WORKING battle trigger object.
        # take the native trigger's zone/emitters, keep the template's brain
        pat = re.compile(r'\{ CSpawnTrigger "%s".*?\} CSpawnTrigger' %
                         re.escape(trigger), re.S)
        src_m = pat.search(d)
        assert src_m, "trigger %s not found in %s" % (trigger, lvl_base)
        pos, size, rad, emitters = trig_fields(src_m.group(0))
        blk = re.sub(r'([\{\}]) CSpawnTrigger "[^"]+"',
                     '\\g<1> CSpawnTrigger "%s"' % inject_name, inject_blk)
        blk = re.sub(r'\n\t[^\n]+?(\s*// pos)', '\n\t%s\\1' % pos, blk, count=1)
        blk = re.sub(r'\n\t[^\n]+?(\s*// triggerSize)',
                     '\n\t%s\\1' % size, blk, count=1)
        blk = re.sub(r'\n\t\d+(\s*// testRadius)', '\n\t%s\\1' % rad,
                     blk, count=1)
        blk, n1 = re.subn(r'^\t\d+(\s*// maxNumberSpawn)',
                          '\t%d\\1' % count, blk, count=1, flags=re.M)
        blk, n2 = re.subn(
            r'^\t\d+(\s*// desiredNumberOfActiveEnemiesInArea)',
            '\t%d\\1' % desired, blk, count=1, flags=re.M)
        blk, n3 = re.subn(r'\t"[^"]*"(\s*// enemiesDead)',
                          '\t"flagon(%s);"\\1' % killflag, blk, count=1)
        assert n1 == 1 and n3 == 1
        # retarget the emitter slots to the native spawn objects
        slots = emitters + ["(none)"] * (10 - len(emitters))
        it = iter(slots)
        blk = re.sub(r'\t"[^"]*"(\s*// text)',
                     lambda m: '\t"%s"%s' % (next(it), m.group(1)), blk)
        # register the object: actor count + name list + data block.
        # (the actorTypeList "CSpawnTrigger 10" is a FORMAT VERSION, not a
        # count -- bumping it makes the loader reject the level)
        d, an = re.subn(r'(// actor count\r?\n)(\d+)',
                        lambda m: m.group(1) + str(int(m.group(2)) + 1),
                        d, count=1)
        d = d.replace('// actor data list',
                      'CSpawnTrigger "%s"\n// actor data list' % inject_name, 1)
        # the actor data region ends at "// hero name" -- splat list and
        # cinemat sections follow; the block belongs as the last actor
        assert '// hero name' in d, "hero name marker not found"
        d = d.replace('// hero name', blk + '\n// hero name', 1)
        assert an == 1, "actor registration failed"
        # the borrowed native trigger becomes a dead shell below (neuter)
        trig_blk_start = None
    else:
        # retarget the chosen spawn trigger block
        pat = re.compile(r'(\{ CSpawnTrigger "%s".*?)(\} CSpawnTrigger)' %
                         re.escape(trigger), re.S)
        m = pat.search(d)
        assert m, "trigger %s not found in %s" % (trigger, lvl_base)
        blk = m.group(1)
        blk, n1 = re.subn(r'^\t\d+(\s*// maxNumberSpawn)',
                          '\t%d\\1' % count, blk, count=1, flags=re.M)
        blk, n2 = re.subn(
            r'^\t\d+(\s*// desiredNumberOfActiveEnemiesInArea)',
            '\t%d\\1' % desired, blk, count=1, flags=re.M)
        blk, n3 = re.subn(r'\t"[^"]*"(\s*// enemiesDead)',
                          '\t"flagon(%s);"\\1' % killflag, blk, count=1)
        blk, n4 = re.subn(r'^\t\d+(\s*// maxDelayTimer)',
                          '\t0\\1', blk, count=1, flags=re.M)
        # mustFace gates emission on the player looking at the trigger center
        # (story uses it to time cinematic reveals); an arena should start
        # emitting the instant the player is inside
        blk = re.sub(r'^\t\d+(\s*// mustFace)\r?$',
                     '\t0\\1', blk, count=1, flags=re.M)
        # rule 3: the horde-refill rule -- the stock SPAWN_* test arenas
        # (desired 3-7, max 250) all run type 4 + rule 3 for dense waves.
        # rule 2 rejects emitters seen in the current/previous frame, which
        # stalls the wave in open rooms where every point is visible
        blk = re.sub(r'^\t\d+(\s*// spawnEmitterActivationRule)',
                     '\t3\\1', blk, count=1, flags=re.M)
        assert n1 == 1 and n3 == 1, \
            "trigger fields not matched in %s (%d %d %d)" % (lvl_base, n1, n2, n3)
        if keep_emitters:
            # huge rooms scatter the wave over every emitter and mooks get
            # lost across the floor -- keep only the N nearest to the warp
            wp_pos = actor_pos(d, "CWayPoint", wp)
            assert wp_pos, "waypoint %s not found" % wp
            em = [e for e in
                  re.findall(r'\t"([^"]+)"\s*// text', blk)
                  if e != "(none)"]
            def dist(e):
                p = actor_pos(d, "CSpawn", e)
                return sum((p[i] - wp_pos[i]) ** 2 for i in range(3)) if p \
                    else 1e30
            em.sort(key=dist)
            kept = em[:keep_emitters]
            slots = kept + ["(none)"] * (10 - keep_emitters)
            it = iter(slots)
            blk = re.sub(r'\t"[^"]*"(\s*// text)',
                         lambda m: '\t"%s"%s' % (next(it), m.group(1)), blk)
        else:
            kept = wp_pos = None
        d = d[:m.start(1)] + blk + d[m.end(1):]
        if ring_r and kept:
            # the room's native points sit 45-110u away -- too far for a
            # horde. re-place the kept emitters on a tight ring around the
            # player's entry so the wave arrives in your face. (after the
            # splice above -- patching earlier would stale m's offsets)
            for i, e in enumerate(kept):
                a_ = 2 * math.pi * i / len(kept)
                epos = actor_pos(d, "CSpawn", e)
                np_ = "%d,%d,%d" % (
                    round(wp_pos[0] + ring_r * math.cos(a_)),
                    round(epos[1]),   # keep emitter Y (they drop in from ~20u
                    round(wp_pos[2] + ring_r * math.sin(a_)))  # above floor)
                d = re.sub(
                    r'(\{ CSpawn "%s".*?\n\t)[^\n]+?(\s*// pos)' %
                    re.escape(e),
                    lambda m: m.group(1) + np_ + m.group(2),
                    d, count=1, flags=re.S)

    keep = inject_name if inject_blk is not None else trigger
    # neuter every OTHER spawn trigger: disable it AND strip its emitter
    # list so nothing can be emitted even if another actor re-enables it
    # (level copy only, stock untouched)
    def neuter(mm):
        if mm.group(2) == keep:
            return mm.group(0)
        head = re.sub(r'^(\t)1(\s*// enabled)', r'\g<1>0\g<2>',
                      mm.group(1), count=1, flags=re.M)
        head = re.sub(r'\t"[^"(][^"]*"(\s*// text)', r'\t"(none)"\1', head)
        return head + mm.group(3)
    d = TRIG_BLK.sub(neuter, d)
    # disable stray CSpawn emitters not owned by our trigger: compatible
    # pre-existing characters inside the region can satisfy the wave quota
    # (desiredNumber) before a single spawn is emitted
    owned = set()
    if inject_blk is not None:
        owned = set(emitters)
    else:
        m2 = TRIG_BLK.search(d)
        while m2 and m2.group(2) != trigger:
            m2 = TRIG_BLK.search(d, m2.end())
        if m2:
            owned = set(e for e in
                        re.findall(r'\t"([^"]+)"\s*// text', m2.group(0))
                        if e != "(none)")
    def neuter_spawn(mm):
        if mm.group(1) in owned:
            return mm.group(0)
        return re.sub(r'^(\t)1(\s*// enabled)', r'\g<1>0\g<2>',
                      mm.group(0), count=1, flags=re.M)
    d = re.sub(r'\{ CSpawn "([^"]+)".*?\} CSpawn "[^"]*"',
               neuter_spawn, d, flags=re.S)
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
    ap.add_argument("--inject", metavar="LVL:TRIGGER",
                    help="transplant this working trigger object instead of "
                    "patching the native one (keeps its own name)")
    ap.add_argument("--keep-emitters", type=int, default=None,
                    help="keep only the N emitter points nearest to --wp")
    ap.add_argument("--ring", type=float, default=None,
                    help="re-place the kept emitters on a ring of this "
                    "radius around --wp")
    a = ap.parse_args()

    inject_blk = inject_name = None
    if a.inject:
        tlvl, tname = a.inject.split(":", 1)
        inject_name = tname
        tsrc = os.path.join(a.src, tlvl)
        if not os.path.isfile(tsrc):
            for f in os.listdir(a.src):
                if f.lower() == tlvl.lower():
                    tsrc = os.path.join(a.src, f)
                    break
        td = open(tsrc, "rb").read().decode("cp1252")
        tm = re.search(r'\{ CSpawnTrigger "%s".*?\} CSpawnTrigger "[^"]*"' %
                       re.escape(tname), td, re.S)
        assert tm, "template trigger %s not found in %s" % (tname, tlvl)
        inject_blk = tm.group(0)

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

    # battle-proven enable + arm pair; activateSpawnTrigger is the spawn
    # trigger's own command (activateTrigger belongs to a different class)
    tgt = inject_name if inject_name else a.trigger
    arm = ["createActor(%s)" % tgt, "activateSpawnTrigger(%s)" % tgt]
    # hero gear lives in the level as standalone weapon actors
    weapons = re.findall(r'\{ (?:CAnimatedMeleeWeapon|CMultigun) "([^"]+)"',
                         text)
    scb = build_scb(wave_script(a.wp, a.setup, arm, a.killflag, a.music,
                                weapons))
    out_scb = os.path.join(a.dst, a.name + ".scb")
    open(out_scb, "wb").write(scb)
    print("scb    ->", os.path.basename(out_scb))

    suffix = {15: "e", 25: "n", 50: "h", 100: "x"}
    for count, desired in zip(COUNTS, DESIRED):
        out_name = "%s_%s.lvl" % (a.name, suffix[count])
        lvl = make_lvl(text, lvl_base, a.name, a.trigger, count, desired,
                       a.killflag, inject_blk, inject_name, a.wp,
                       a.keep_emitters, a.ring)
        out = os.path.join(a.dst, out_name)
        open(out, "wb").write(lvl.encode("cp1252"))
        print("lvl    ->", out_name, "(%d enemies, %d active)" %
              (count, desired))


if __name__ == "__main__":
    main()
