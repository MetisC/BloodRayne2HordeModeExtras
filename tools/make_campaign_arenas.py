#!/usr/bin/env python3
"""Generate the campaign-derived arenas used by Challenge Mode.

The source maps are copied from common_orig. The generated SCBs replace their
story scripts; story triggers and static enemies are disabled, while the
environmental fire/fan actors and closed door collision remain in the level.
"""
import argparse
import os
import re

import make_arena
import make_modes


ARENAS = [
    {
        "base": "chb_fireplace",
        "lvl": "A1_MANSION_PART1B.LVL",
        "trigger": "SpawnTrigger_FireplaceWaveFight",
        "wp": "wp_raynebythefire",
        "recovery": (19, 12),
        "emitters": 4,
        "spawn_waypoints": {"Spawn_KillOMeter1": "wp_mookinvestigate1",
                            "Spawn_KillOMeter2": "wp_mookinvestigate2",
                            "Spawn_KillOMeter3": "wp_rayneleavesthevr",
                            "Spawn_KillOMeter4": "wp_raynebypicture"},
        "keep_triggers": ("Trigger_BodyBurning1",),
        "music": "music_wavefight2_loop.wav",
    },
    {
        "base": "chb_mansion_roof",
        "lvl": "A1_MANSION_ROOFARENA.LVL",
        "trigger": "SpawnTrigger0",
        "wp": "WP_Rayne_Dialog2",
        # Centre of the two stock player landings WP_End1 and WP_End2.
        "start": "5.131215,7.135875,87.7236",
        "start_orient": "0,0,0",
        "spawn_waypoints": {"Spawn2": "WP_Rayne_Dialog1", "Spawn3": "WP_Rayne_Dialog2"},
        "recovery": (29, 22),
        "emitters": 2,
        "keep_triggers": (),
        "music": "music_wavefight2_loop.wav",
    },
    {
        "base": "chb_bar",
        "lvl": "A2_MEATPACKING_1.LVL",
        "trigger": "SpawnTrig_Bar_Wave",
        "wp": "wp_bar_rayne_feed",
        "recovery": (19, 14),
        "spawn_waypoints": {"Sp_Bar_Wave1": "wp_LPunk_Bar1"},
        "emitters": 2,
        "keep_triggers": (),
        "music": "music_bar_loop.wav",
    },
    {
        "base": "chb_meatpacking_arena",
        "lvl": "A2_MEATPACKING_2.LVL",
        "trigger": "SpawnTrig_Wave_ArenaD",
        "wp": "wp_arenaD_Rayne3",
        "emitters": 2,
        "spawn_waypoints": {"Sp_Wave_ArenaD1": "wp_arenaD_rayne4",
                            "Sp_Wave_ArenaD2": "wp_arenaD_rayne1"},
        "keep_triggers": (),
        "music": "music_wavefight2_loop.wav",
    },
    {
        "base": "chb_shroud_roof",
        "lvl": "A3_SHROUDTOWER_ROOF.LVL",
        "trigger": "SpawnTrigger0",
        "wp": "WP_Beginning",
        # Facing the roof center, opposite of the stock waypoint direction.
        "start_orient": "0,0,0",
        "recovery": (29, 22),
        "disable_actors": ("WP_IntroCineWalkto1",),
        "emitters": 6,
        "spawn_positions": {"SpawnFPM3": "10,935.656,35",
                            "SpawnFPM4": "5,935.656,45",
                            "SpawnFPF2": "0,935.656,30"},
        "boundary_classes": "CMainCharacter|CLPunk",
        "keep_triggers": (),
        "music": "music_wavefight2_loop.wav",
    },
    {
        "base": "chb_insect_plaza",
        "lvl": "A4_PARK_1.LVL",
        "trigger": "SpawnTrig_Wave_Insect_Plaza",
        "wp": "wp_Insect_Plaza6",
        "emitters": 2,
        "spawn_waypoints": {"Sp_Insect_Plaza_Slegion1": "wp_Insect_Plaza5",
                            "Sp_Insect_Plaza_Slegion2": "wp_Insect_Plaza1"},
        "keep_triggers": (),
        "music": "music_wavefight2_loop.wav",
    },
    {
        "base": "chb_woodchipper",
        "lvl": "A4_PARK_2.LVL",
        "trigger": "SpawnTrig_Wave_Punk_Chipper_1",
        "wp": "wp_chipper_4",
        # The floor height and position come from the original golem emitter.
        "start": "1006.25,0.0468748,-211.5",
        "start_orient": "0,0,3.14159",
        "emitters": 4,
        "keep_triggers": (),
        "music": "music_wavefight2_loop.wav",
    },
]


def read_level(src, name):
    for filename in os.listdir(src):
        if filename.lower() == name.lower():
            with open(os.path.join(src, filename), "rb") as f:
                return f.read().decode("cp1252")
    raise FileNotFoundError(name)


def setup_tokens(src, lvl_name):
    stem = os.path.splitext(lvl_name)[0]
    raw = None
    for filename in os.listdir(src):
        if filename.lower() == (stem + ".scb").lower():
            with open(os.path.join(src, filename), "rb") as f:
                raw = f.read()
            break
    assert raw, "story script missing for " + stem
    tokens = raw[12:].decode("cp1252", errors="replace").split("\0")
    first_label = next((i for i, t in enumerate(tokens) if t.startswith(":")),
                       len(tokens))
    setup = []
    for token in tokens[:first_label]:
        if token.startswith(("enableSetGeometry(", "enableBreaker(",
                             "enablePathPoint(")):
            setup.append(token)
        elif "Fan" in token and token.startswith("attachActorToActor("):
            setup.append(token)
    return setup


def rayne_guns(level):
    return [
        name for name, block in re.findall(
            r'^\{ CMultigun "([^"]+)"(.*?)^\} CMultigun', level, re.M | re.S)
        if re.search(r'^\s*"\(none\)"\s*// carriedByActor', block, re.M)
        and re.search(r'^\s*0\s*// multiGunType', block, re.M)
    ]


def move_spawns_to_waypoints(level, positions):
    for emitter, waypoint in positions.items():
        pos = make_arena.actor_pos(level, "CWayPoint", waypoint)
        assert pos, waypoint
        level, count = re.subn(
            r'(^\{ CSpawn "' + re.escape(emitter) + r'"\s*\n\t)[^\r\n]+?(\s+// pos)',
            lambda m: m.group(1) + ",".join(map(str, pos)) + m.group(2),
            level, flags=re.M)
        assert count == 1, emitter
    return level


def configure_insect_pacing(level, trigger):
    """Start with a punk and a Slegion, then space out the Slegion."""
    weak = "Sp_Insect_Plaza_Slegion1"
    hard = "Sp_Insect_Plaza_Slegion2"
    weak_positions = [
        "303.59,0.1,-702.971",
        "387.445,0.1,-681.284",
        "300.134,0.1,-688.23",
        "301.825,0.1,-693.737",
        "301.825,0.1,-693.737",
        "303.59,0.1,-702.971",
        "387.445,0.1,-681.284",
        "300.134,0.1,-688.23",
        "301.825,0.1,-693.737",
    ]
    names = [weak] + ["Sp_Insect_Plaza_Punk%d" % i for i in range(1, 9)] + [hard]
    level = make_arena.actor_field(level, "CSpawn", weak, "pos", weak_positions[0])
    # Use the unarmed punk already spawned by this map in the campaign.
    for field in ("enemySpawnType", "enemyWeaponType"):
        donor = re.search(r'^\{ CSpawn "Spawn_Insect_Punk"[^}]*?^\t([^\r\n]+?)[ \t]+// ' +
                          field + r'\r?$', level, re.M)
        assert donor, field
        level = make_arena.actor_field(level, "CSpawn", weak, field, donor[1])
    blocks = []
    for name, position in zip(names[1:9], weak_positions[1:9]):
        source = re.search(
            r'\{ CSpawn "' + re.escape(weak) + r'".*?\} CSpawn "' +
            re.escape(weak) + r'"', level, re.S)
        assert source, "insect weak-emitter template missing"
        block = source.group(0).replace('"' + weak + '"', '"' + name + '"')
        block, count = re.subn(
            r'^(\t)[^\r\n]+?([ \t]+// pos)\r?$',
            r'\g<1>' + position + r'\2', block, count=1, flags=re.M)
        assert count == 1
        blocks.append(block)

    level, count = re.subn(
        r'(// actor count\r?\n)(\d+)',
        lambda m: m[1] + str(int(m[2]) + len(blocks)),
        level, count=1)
    assert count == 1
    level, count = re.subn(
        r'(// actor name list\r?\n)(.*?)(// actor data list\r?\n)',
        lambda m: m[1] + m[2] +
        "".join('CSpawn "%s"\n' % name for name in names[1:9]) + m[3],
        level, count=1, flags=re.S)
    assert count == 1
    level = level.replace("// hero name",
                          "\n".join(blocks) + "\n// hero name", 1)
    trigger_block = re.search(
        r'(\{ CSpawnTrigger "' + re.escape(trigger) +
        r'".*?\} CSpawnTrigger "' + re.escape(trigger) + r'")',
        level, re.S)
    assert trigger_block
    refs = iter([weak, hard] + names[1:9])
    updated, count = re.subn(
        r'^\t"[^"]*"([ \t]+// text)(\r?)$',
        lambda m: '\t"%s"%s%s' % (next(refs), m[1], m[2]),
        trigger_block.group(1), flags=re.M)
    assert count == 10
    return level[:trigger_block.start(1)] + updated + level[trigger_block.end(1):]


# World-space perimeter of each fight. These player-only barriers supplement
# doors in the copied maps, including outdoor exits with no door actor.
PERIMETERS = {
    "chb_fireplace": (-474, -377, -135, -330, -246),
    "chb_mansion_roof": (-53, 130, -5, 29, 227),
    "chb_bar": (-39, 23, 39.53, 539, 614),
    "chb_meatpacking_arena": (-183, -61, 0, 443, 532),
    "chb_shroud_roof": (-76, 74, 935.65, -71, 70),
    "chb_insect_plaza": (295, 482, 0, -773, -596),
    "chb_woodchipper": (969, 1053, 0, -331, -144),
}


def position_guns(level, names, hero_pos):
    hx, hy, hz = [float(v) for v in hero_pos.split(",")]
    for i, name in enumerate(names):
        pos = "%g,%g,%g" % (hx + (0.6 if i == 0 else -0.6), hy, hz)
        level, n = re.subn(
            r'(\{ CMultigun "' + re.escape(name) +
            r'".*?\n\t)[^\r\n]+?(\s*// pos)',
            lambda m: m.group(1) + pos + m.group(2), level,
            count=1, flags=re.S)
        assert n == 1, "multigun position not found: " + name
    # Keep any extra story pickups out of the arena and out of the warp list.
    extras = [n for n in re.findall(r'\{ CMultigun "([^"]+)"', level)
              if n not in names]
    for name in extras:
        level, n = re.subn(
            r'(\{ CMultigun "' + re.escape(name) +
            r'".*?\n\t)([^\r\n]+?)(\s*// pos)',
            lambda m: m.group(1) + "0,3000,0" + m.group(3), level,
            count=1, flags=re.S)
        assert n == 1
    return level


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="stock extracted WORLD")
    ap.add_argument("--dst", required=True, help="game WORLD output folder")
    a = ap.parse_args()
    os.makedirs(a.dst, exist_ok=True)

    template = read_level(a.src, "SPAWN.LVL")
    fall_template = read_level(a.src, "A3_SHROUDTOWER_ROOF.LVL")
    for cfg in ARENAS:
        original = read_level(a.src, cfg["lvl"])
        original = make_arena.strip_arena_cinematics(original)
        original, blade_names = make_arena.prepare_arena_player(original)
        hero = re.search(
            r'\{ CMainCharacter "[^"]+"\s*\r?\n\t([^\r\n]+?)\s*// pos',
            original, re.S)
        assert hero, "hero spawn missing in " + cfg["lvl"]
        hero_pos = hero.group(1)
        original = move_spawns_to_waypoints(original, cfg.get("spawn_waypoints", {}))
        for emitter, position in cfg.get("spawn_positions", {}).items():
            original = make_arena.actor_field(
                original, "CSpawn", emitter, "pos", position)
        for key, field in (("start", "pos"), ("start_orient", "orient")):
            if key in cfg:
                original = make_arena.actor_field(original, "CWayPoint", cfg["wp"], field, cfg[key])
        for name in cfg.get("disable_actors", ()):
            original, disabled = re.subn(
                r'(^\{ CMainCharacter "' + re.escape(name) +
                r'"[^}]*?^\t)[01](\s*// enabled)',
                r'\g<1>0\g<2>', original, count=1, flags=re.M)
            assert disabled == 1, name
        if cfg["base"] == "chb_fireplace":
            original = original.replace("; incCounter(fireplacekillometercount)", "")

        for count, default_desired in zip(make_arena.COUNTS, make_arena.CAMPAIGN_DESIRED):
            stem = "%s_%d" % (cfg["base"], count)
            desired = 2 if cfg["base"] == "chb_insect_plaza" else default_desired
            for guns in (False, True):
                outbase = stem + ("g" if guns else "")
                level = original
                weapon_names = blade_names.copy()
                has_guns = bool(re.search(r'^\{ CMultigun "', level, re.M))
                if guns:
                    if not has_guns:
                        level = make_modes.inject_guns(level, template)
                        level, _ = re.subn(r'^(\t?CMultigun +)5 *$', r'\g<1>6',
                                           level, count=1, flags=re.M)
                    gun_names = rayne_guns(level)
                    assert len(gun_names) == 2, (cfg["lvl"], gun_names)
                    level = position_guns(level, gun_names, hero_pos)
                    weapon_names += gun_names
                elif has_guns:
                    level = make_modes.nude_guns(level)

                clean = make_arena.make_lvl(
                    level, cfg["lvl"], outbase, cfg["trigger"], count,
                    desired, make_arena.ARENA_FINISH_FLAG, wp=cfg["wp"],
                    keep_emitters=cfg["emitters"], arena_cleanup=True,
                    keep_triggers=cfg["keep_triggers"], spawn_rule=0,
                    spawn_delay=2 if cfg["base"] == "chb_insect_plaza" else 1,
                    keep_trigger_position=True)
                if cfg["base"] == "chb_insect_plaza":
                    clean = configure_insect_pacing(clean, cfg["trigger"])
                bounds = PERIMETERS[cfg["base"]]
                clean = make_arena.cover_arena_wave(clean, cfg["trigger"], bounds)
                clean = make_arena.add_perimeter(
                    clean, bounds, cfg.get("boundary_classes", "CMainCharacter"))
                event_flags = []
                extra = []
                setup = setup_tokens(a.src, cfg["lvl"])
                if cfg["base"] == "chb_bar":
                    setup = ["flagOff(Fan_Fed_With_Body)"] + setup
                    event_flags = [("Fan_Fed_With_Body", "ArenaFanKill")]
                    extra = [":ArenaFanKill", "flagOff(Fan_Fed_With_Body)",
                             "raise(Fx_Fan_Activate)", "raise(Fx_Fan_Activate2)",
                             "raise(Snd_Fan_Kill1)", "return"]
                if "recovery" in cfg:
                    clean = make_arena.add_fall_recovery(
                        clean, fall_template, bounds, *cfg["recovery"])
                    setup.insert(0, "flagOff(ArenaPlayerFell)")
                    event_flags.insert(0, ("ArenaPlayerFell", "ArenaRecoverFall"))
                    extra += [":ArenaRecoverFall", "flagOff(ArenaPlayerFell)",
                              "warpTo($,%s)" % cfg["wp"], "return"]
                arm = ["createActor(%s)" % cfg["trigger"],
                       "activateSpawnTrigger(%s)" % cfg["trigger"]]
                script = make_arena.wave_script(
                    cfg["wp"], setup, arm, make_arena.ARENA_FINISH_FLAG, cfg["music"],
                    weapon_names, event_flags, guns)
                scb = make_arena.build_scb(script + extra)
                with open(os.path.join(a.dst, outbase + ".lvl"), "wb") as f:
                    f.write(clean.encode("cp1252"))
                with open(os.path.join(a.dst, outbase + ".scb"), "wb") as f:
                    f.write(scb)
                print("%-31s source=%-22s count=%3d guns=%s" %
                      (outbase, cfg["lvl"], count, "on" if guns else "off"))


if __name__ == "__main__":
    main()
