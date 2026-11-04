#!/usr/bin/env python3
# make_modes.py — builds the challenge-mode level variants.
#
# Two arenas, five enemy counts, guns on/off:
#   - Mansion Battle: stock a1_mansion_part1_battle_{easy,normal,hard}
#     (15/25/50, no guns) ship as-is. Generated: 100/250 from EXTREME
#     (max patch) and all five "+g" variants (multigun actors injected).
#   - Horde Mode: stock spawn_{easy,normal,hard,extreme} (15/25/50/250,
#     guns included) ship as-is. Generated: 100 from EXTREME and all five
#     no-guns variants (multigun actors relocated out of reach).
#
# Every generated file is a renamed copy: the script dependency and the
# level ROOT NAME are retargeted to the output base so the loader picks
# our .scb (a verbatim copy of the source one -- these scripts already
# finish the wave and chainToLevel(MENU_LEVEL.lvl) by themselves).
#
# Usage: python make_modes.py --src extracted/common_mod/WORLD --dst world
import argparse
import os
import re

# (out_base, source_level_stem, maxNumberSpawn or None, gun_op)
# gun_op: None = leave weapons, "inject" = add multiguns, "nude" = move them,
#         "horde_guns"/"horde_nude" = horde: hero spawns stock (weapon
#         proximity-equip only works there), then the script warpTo()s him
#         and the weapons into the arena centre -- same pattern the mansion
#         battle uses with wp_harpooncine1. Rival wave triggers disabled.
VARIANTS = [
    # --- Mansion Battle: stock counts shipped as retitled loose copies so
    #     the banner reads the localizable "Mansion Battle" key (the old
    #     equal-length blanker left display(   ) spaces). Same-name outputs.
    ("A1_MANSION_PART1_BATTLE_EASY",   "A1_MANSION_PART1_BATTLE_EASY",   None, None),
    ("A1_MANSION_PART1_BATTLE_NORMAL", "A1_MANSION_PART1_BATTLE_NORMAL", None, None),
    ("A1_MANSION_PART1_BATTLE_HARD",   "A1_MANSION_PART1_BATTLE_HARD",   None, None),
    # --- Mansion Battle: extra counts from EXTREME (native 99) ---
    ("a1_mansion_part1_battle_100",  "A1_MANSION_PART1_BATTLE_EXTREME", 100, None),
    ("a1_mansion_part1_battle_250",  "A1_MANSION_PART1_BATTLE_EXTREME", 250, None),
    # --- Mansion Battle: guns=yes variants (battle has no multiguns) ---
    ("a1_mansion_part1_battle_15g",  "A1_MANSION_PART1_BATTLE_EASY",    None, "inject"),
    ("a1_mansion_part1_battle_25g",  "A1_MANSION_PART1_BATTLE_NORMAL",  None, "inject"),
    ("a1_mansion_part1_battle_50g",  "A1_MANSION_PART1_BATTLE_HARD",    None, "inject"),
    ("a1_mansion_part1_battle_100g", "A1_MANSION_PART1_BATTLE_EXTREME", 100,  "inject"),
    ("a1_mansion_part1_battle_250g", "A1_MANSION_PART1_BATTLE_EXTREME", 250,  "inject"),
    # --- Horde Mode: all variants are copies (arena start + counter fix) ---
    ("chb_hor_15",   "SPAWN_EASY",                      None, "horde_nude"),
    ("chb_hor_25",   "SPAWN_NORMAL",                    None, "horde_nude"),
    ("chb_hor_50",   "SPAWN_HARD",                      None, "horde_nude"),
    ("chb_hor_100",  "SPAWN_EXTREME",                   100,  "horde_nude"),
    ("chb_hor_250",  "SPAWN_EXTREME",                   None, "horde_nude"),
    ("chb_hor_15g",  "SPAWN_EASY",                      None, "horde_guns"),
    ("chb_hor_25g",  "SPAWN_NORMAL",                    None, "horde_guns"),
    ("chb_hor_50g",  "SPAWN_HARD",                      None, "horde_guns"),
    ("chb_hor_100g", "SPAWN_EXTREME",                   100,  "horde_guns"),
    ("chb_hor_250g", "SPAWN_EXTREME",                   None, "horde_guns"),
]

WAVE_TRIGGERS = ["ST_Adv_Combat_Tute_Help", "SpawnTrigger_Both"]
GUN_POS = "-182,-145,-261"   # wp_harpooncine1: battle script warp destination
# Horde: hero+weapons keep the stock sub-room spawn (-48,16,76) so the native
# proximity equip fires; the script then warps them to this waypoint in the
# middle of the arena (floor level there is ~y=12).
HORDE_WP = "-28,12,4"
HORDE_WARP = ["MainCharacter0", "AnimatedMeleeWeapon0", "AnimatedMeleeWeapon1"]
HORDE_RIVALS = ("SpawnTrigger_Melee", "SpawnTrigger_Gun")


def find(src_dir, stem):
    for ext in (".LVL", ".lvl", ".SCB", ".scb"):
        p = os.path.join(src_dir, stem + ext)
        if os.path.isfile(p):
            return p
    for f in os.listdir(src_dir):
        if f.lower() == (stem + ".lvl").lower() or \
           f.lower() == (stem + ".scb").lower():
            return os.path.join(src_dir, f)
    raise SystemExit("not found: " + stem)


def patch_max(lvl, count):
    for trig in WAVE_TRIGGERS:
        pat = re.compile(r'(\{ CSpawnTrigger "%s".*?)(\} CSpawnTrigger)' %
                         re.escape(trig), re.S)
        m = pat.search(lvl)
        if not m:
            continue
        blk, n = re.subn(r'^\t\d+(\s*// maxNumberSpawn)',
                         '\t%d\\1' % count, m.group(1), count=1, flags=re.M)
        assert n == 1, "maxNumberSpawn not found in " + trig
        return lvl[:m.start(1)] + blk + lvl[m.end(1):], trig
    raise SystemExit("no wave trigger found for max patch")


def nude_guns(lvl):
    # multiguns auto-equip on proximity; park them far above the arena
    def up(mm):
        pos = mm.group(2)
        x, y, z = [float(v) for v in pos.split(",")]
        return mm.group(1) + "%g,%g,%g" % (x, y + 3000.0, z) + mm.group(3)
    lvl, n = re.subn(
        r'(\{ CMultigun "[^"]+".*?\n\t)([^\n]+?)(\s*// pos)', up,
        lvl, flags=re.S)
    assert n >= 2, "expected 2+ multiguns to relocate, got %d" % n
    return lvl


def neuter_rivals(lvl):
    # All three wave triggers ship enabled=1 with the same WaveDoneIt event;
    # three live objectives == ambiguous == no kill counter. The script's own
    # gosub Both deletes the losers anyway, so disabling them changes nothing
    # but un-ambiguates the objective from the first frame.
    for trig in HORDE_RIVALS:
        pat = re.compile(r'(\{ CSpawnTrigger "%s".*?\n\t)1(\s*// enabled)'
                         % re.escape(trig), re.S)
        lvl, n = pat.subn(r'\g<1>0\g<2>', lvl, count=1)
        assert n == 1, "enabled line not found in " + trig
    return lvl


def add_waypoint(lvl, name, pos):
    # CWayPoint is already declared in the typelist (version 3); add an
    # instance at the arena centre so the script warpTo() has a target.
    block = ('{ CWayPoint "%s"\n\t%s    // pos\n\t0,0,0    // orient\n'
             '\t1    // enabled\n\t""    // group\n\t0    // attachCount\n'
             '} CWayPoint "%s"') % (name, pos, name)
    lvl, n = re.subn(r'(// actor count\r?\n)(\d+)',
                     lambda m: m.group(1) + str(int(m.group(2)) + 1),
                     lvl, count=1)
    assert n == 1, "actor count not found"
    lvl = lvl.replace("// actor data list",
                      'CWayPoint "%s"\n// actor data list' % name, 1)
    assert "// hero name" in lvl
    lvl = lvl.replace("// hero name", block + "\n// hero name", 1)
    return lvl


def horde_setup(lvl, guns):
    # Nothing moves in the LVL: the native spawn proximity-equips the weapons
    # (proven stock behaviour). The script warp does the arena relocation.
    lvl = neuter_rivals(lvl)
    lvl = add_waypoint(lvl, "wp_chb_horde", HORDE_WP)
    if not guns:
        lvl = nude_guns(lvl)
    return lvl


def mansion_gun_warp(scb):
    # The mansion levels run with gun mode DISABLED (Zerenski's infiltration
    # party; stock story scripts call enableGunMode(single) when Rayne gets
    # her guns back in PART4/5). Without it the proximity pickup never fires.
    # Also warpTo the guns with the hero (stock weapon-warp pattern): if the
    # spawn equip already ran they ride along, if not they land on him.
    toks = scb[12:].split(b"\x00")
    ins = [b"enableGunMode(single)",
           b"warpTo(Multigun0, wp_harpooncine1)",
           b"warpTo(Multigun1, wp_harpooncine1)"]
    i = toks.index(b"warpTo(0MainCharacter0, wp_harpooncine1)")
    out = toks[:i + 1] + ins + toks[i + 1:]
    nb = b"\x00".join(out)
    return scb[:4] + (len([t for t in out if t])).to_bytes(4, "little") + \
        len(nb).to_bytes(4, "little") + nb


def horde_warp(scb, guns):
    # Insert "warpTo(<actor>, wp_chb_horde)" right after the init "gosub Both"
    # -- i.e. before :mainloop. Weapons are warped too (stock pattern): if the
    # proximity equip already ran they ride along anyway, if not they land on
    # the hero and equip on contact. Parked multiguns are never warped.
    toks = scb[12:].split(b"\x00")
    names = HORDE_WARP + (["Multigun0", "Multigun1"] if guns else [])
    ins = [b"warpTo(%s, wp_chb_horde)" % n.encode() for n in names]
    i = toks.index(b"gosub Both")
    out = toks[:i + 1] + ins + toks[i + 1:]
    nb = b"\x00".join(out)
    return scb[:4] + (len([t for t in out if t])).to_bytes(4, "little") + \
        len(nb).to_bytes(4, "little") + nb


def inject_guns(lvl, tmpl_lvl):
    # clone the template's CMultigun blocks at the HERO SPAWN. Proximity pickup
    # is evaluated at spawn (the horde recipe): guns placed at the script's
    # later warp destination were never picked up, and a half-resolved
    # carriedByActor broke the party-dress outfit. The script also warpTo()s
    # the guns to wp_harpooncine1 alongside the hero as a belt-and-suspenders.
    # The battle typelist declares CMultigun version 6 (vs SPAWN's 5): v6 has
    # the extra trailing property multiGunType, without which the loader aborts
    # ("Error loading actor property").
    hero = re.search(r'\{ CMainCharacter "[^"]+"\s*\r?\n\t([-\d.,e+]+)\s*// pos',
                     lvl, re.S)
    assert hero, "hero pos not found"
    hx, hy, hz = [float(v) for v in hero.group(1).split(",")]
    blocks = re.findall(r'\{ CMultigun "[^"]+".*?\} CMultigun "[^"]*"',
                        tmpl_lvl, re.S)
    assert len(blocks) >= 2, "multigun template blocks not found"
    add = []
    for i, b in enumerate(blocks[:2]):
        off = 0.6 if i == 0 else -0.6
        np_ = "%g,%g,%g" % (hx + off, hy, hz)
        b = re.sub(r'\n\t[^\n]+?(\s*// pos)', '\n\t%s\\1' % np_, b, count=1)
        if "multiGunType" not in b:
            b = re.sub(r'(\n\t)(1000\s*// ammoCount)',
                       '\\1\\2\n\t0    // multiGunType', b, count=1)
            assert "multiGunType" in b, "ammoCount line not found"
        add.append(b)
    lvl, n = re.subn(r'(// actor count\r?\n)(\d+)',
                     lambda m: m.group(1) + str(int(m.group(2)) + len(add)),
                     lvl, count=1)
    assert n == 1, "actor count not found"
    names = "".join('CMultigun "Multigun%d"\n' % i for i in range(len(add)))
    lvl = lvl.replace("// actor data list", names + "// actor data list", 1)
    assert "// hero name" in lvl
    lvl = lvl.replace("// hero name",
                      "\n".join(add) + "\n// hero name", 1)
    return lvl


def retitle(scb):
    # Blank every display() banner except "Challenge Completed": the title
    # token sits in the HUD text slot and would flash "Mansion Battle" /
    # "Horde Mode" before our kill counter takes ownership.
    keep = b"Challenge Completed"
    body = scb[12:]
    toks = body.split(b"\x00")
    out = [t if (not t.startswith(b"display(") or keep in t)
           else b"display( )" for t in toks]
    nb = b"\x00".join(out)
    return scb[:4] + (len([t for t in out if t])).to_bytes(4, "little") + \
        len(nb).to_bytes(4, "little") + nb


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True)
    ap.add_argument("--dst", required=True)
    a = ap.parse_args()

    tmpl = open(find(a.src, "SPAWN"), "rb").read().decode("cp1252")

    for out, stem, maxn, guns in VARIANTS:
        lvl = open(find(a.src, stem), "rb").read().decode("cp1252")
        raw = None
        for f in os.listdir(a.src):
            if f.lower() == (stem + ".scb").lower():
                raw = open(os.path.join(a.src, f), "rb").read()
                break
        assert raw, "scb not found for " + stem

        # script dep + root name -> our base, or the stock script would run
        lvl, n = re.subn(r"world\\[A-Za-z0-9_]+\.script",
                         lambda m: "world\\%s.script" % out, lvl, count=1)
        assert n == 1, "script dep not found in " + stem
        lvl, n = re.subn(r'(// Root name of level\r?\n)[^\r\n]+',
                         lambda m: m.group(1) + out, lvl, count=1)
        assert n == 1, "root name not found in " + stem

        raw = retitle(raw)
        if maxn:
            lvl, trig = patch_max(lvl, maxn)
        else:
            trig = None
        if guns == "inject":
            lvl = inject_guns(lvl, tmpl)
            raw = mansion_gun_warp(raw)
        elif guns == "nude":
            lvl = nude_guns(lvl)
        elif guns == "horde_guns":
            lvl = horde_setup(lvl, guns=True)
            raw = horde_warp(raw, guns=True)
        elif guns == "horde_nude":
            lvl = horde_setup(lvl, guns=False)
            raw = horde_warp(raw, guns=False)

        open(os.path.join(a.dst, out + ".lvl"), "wb") \
            .write(lvl.encode("cp1252"))
        open(os.path.join(a.dst, out + ".scb"), "wb").write(raw)
        print("%-14s <- %-38s max=%-4s guns=%s" %
              (out, stem, maxn or "-", guns or "-"))


if __name__ == "__main__":
    main()
