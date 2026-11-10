"""Build BR2_EPHEMERA_PLAYER / BR2_KESTRAL_PLAYER - playable boss versions of
Ephemera and Kestral for Challenge Mode.

Same recipe as build_playable_dhampirm.py: a skeleton graft (donor bones plus
the Rayne player bones the controller looks up by name) and a 478-slot move
table driven by the boss's own clips.

EPHEMERA (66 enemy states, 85 bones)
- Graft adds only the six Eye_* player bones (85+6=91, under the 120 cap);
  Rayne's hair/ribbon bones are never looked up by name and her own h_* hair
  plus JUG already covers physics, so grafting all 40 would exceed the cap for
  no benefit.
- Her BFM attachments are blade_L/blade_R; renamed to bladeL/bladeR so the
  native weapon mount finds them.
- No vertical-jump, knockdown, getup, rail, pole, wall, feed or gun clips:
  those slots fall back to a short standready. Vertical jump reuses the first
  frames of jumpforward, descent uses fall, landings use fall_land, and the
  getup/recovery slots reuse fall_land (her scramble-up clip).
- Shadow moves: ghost_float plays inshadow (her held shadow pose), dash_rayne
  plays entershadow. Real pool teleport needs CShadowPool entities and stays
  unmapped.
- locked_taunt plays gesture1.

KESTRAL (125 enemy states, 45 bones)
- Full graft: all 56 missing player bones (45+56=101 < 120). Her attachments
  are already bladeL/bladeR, native-compatible.
- She has the complete move set: vertical jump (jumpupstart/fall/land), the
  jump16 ninja flip for double_jump, four-directional evades WITH follow-up
  attacks (evade*blade / evade*kick), knockdowns, wall slams, getups and the
  entire rail set (slides, runs, railblade, dismounts).
- Pole/wall slots use the rayne\\*.ani clips her own stock SKB already binds
  (bind-by-name works on her dhampir rig, exactly as stock Kestral does).
- railblade (dual-blade spin) is the Triangle special; THROW is only a gesture
  without a projectile so locked_taunt plays it instead.
- locked_evade_*_b/_k play her evade*blade / evade*kick counters.

Both keep Rayne's timelines scaled onto the donor clips, translate the
donors' l/r blade/foot damage windows to native player ranges, and add donor
combat sfx on top of Rayne's scaled samples (the voice hook remaps rayne_vc_*
per character). die1 -> dead1 keeps the native game-over timer. Ephemera
uses a separate final pose like Rayne; other held clips use HOLD_RATE.
"""

import argparse
import re
import sys
from pathlib import Path
from struct import pack, pack_into, unpack_from

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "tools"))
import build_playable_ferril as F            # noqa: E402
import build_playable_dhampir as D           # noqa: E402
import build_playable_dhampirm as DM         # noqa: E402
from build_ephemera import SKB_BONE, Model   # noqa: E402
from pod3 import Pod3                        # noqa: E402


def dampen_boss_jug(physics):
    """Generic dampen for boss JUGs (hair/breast joints, any bone name):
    0.88 velocity retention, springs and constraints untouched."""
    lines = physics.splitlines()
    for i in range(5, 5 + int(lines[3]) * 2, 2):
        params = lines[i + 1].split(b",")
        if len(params) != 17:
            raise ValueError("Unexpected JUG joint " + lines[i].decode())
        params[1] = b"0.88"
        lines[i + 1] = b",".join(params)
    return b"\r\n".join(lines) + b"\r\n"

BS = chr(92)
fixed = F.fixed

ATTACK_RATE = 45.0
DEATH_RATE = 36.0
HOLD_RATE = 900.0
RATE_MIN, RATE_MAX = 24.0, 70.0
IDLE_CAP = 9
UNMAPPED_CAP = 12
HURT_CAP = 32
EPHEMERA_CLIPS = {46: r"br2_ephemera\flurry.ani", 212: r"br2_ephemera\death.ani",
                  213: r"br2_ephemera\dead.ani"}
KESTRAL_CLIPS = {46: r"br2_kestral\combo3.ani"}
NINJAM_FALL_FRAMES = 33
NINJAM_CLIPS = {212: r"br2_ninjam\death.ani", 213: r"br2_ninjam\dead.ani"}
KAGAN_REACTION_CLIPS = {f"hurt{side}": fr"br2_kagan\hurt{side}.ani"
                        for side in ("front", "back", "left", "right")}
KAGAN_CLIPS = {"shockwave": r"br2_kagan\shockwave.ani", **KAGAN_REACTION_CLIPS}

# Stock Kagan's sword pose relative to Bip01 R Hand during standready.
# The BFM stores a row-major affine matrix, with translation in column four.
KAGAN_RIGHT_SWORD_GRIP = (
    -0.62972533, 0.15653895, -0.76088209, 0.08048663,
    -0.57160225, 0.56990512, 0.59032112, -0.42920905,
    0.52603885, 0.80666208, -0.26940567, -0.11558100,
)
AIR_HOLD = {257, 258, 261, 262, 395, 431}
SPECIAL_READY = {0, 1, *range(7, 15), *range(17, 26), 385, 426,
                 *range(421, 425), *range(448, 452)}
DAMAGE_RANGES = {"lbladedamage", "rbladedamage", "bbladedamage",
                 "rfootdamage", "lfootdamage"}
KEEP_NOGRAVITY = re.compile(r"^(jump|double_jump|fall|locked_evade|combo_(locked_)?(forward_)?j[bk]|dismount_rail)")
HURT_FLIGHT = {"check_hit_wall", "nogravity"}
DECORATIVE = re.compile(r"^(hair_|ribbin_|ring_)")


def ephemera_donor(donor, index, slot):
    """Return (ephemera state | segment key, cap, rate mode) or None."""
    name = slot.name
    clip = slot.clip.replace("/", BS).split(BS)[-1].rsplit(".", 1)[0].lower()
    if clip == "home":
        return None
    if index == 46:
        return SEGMENTS_E["SPECIAL_BLADE"]
    if index == 47:
        return SEGMENTS_E["SPECIAL_KICK"]
    if index == 48:
        return None
    if name.startswith("combo_") and "-idle" in name:
        return ("standready", "idle", "idle")
    if "groundstrike" in name:
        return SEGMENTS_E["GROUND_K"] if "_k_" in name else SEGMENTS_E["GROUND_B"]
    if index == 66:
        return SEGMENTS_E["GROUND_K"]
    if re.search(r"(?:^|_)j[bk]", name):
        if name.endswith("_hold"):
            return ("fall", 2, "idle")
        if name.endswith(("_end", "_land")):
            return ("fall_land", "slot", "warp")
        return SEGMENTS_E["AIR_K"] if "jk" in name else SEGMENTS_E["AIR_B"]
    chain = re.search(r"_([bk]{1,3})(?:_m(?:irror)?)?$", name)
    if name.startswith("combo_") and chain:
        return SEGMENTS_E[CHAIN[(chain[1][0], len(chain[1]))]]
    if name.startswith("hurt_"):
        side = DM.hurt_side(name)
        if "tackle" in name:
            if re.search(r"(recov|wall|abort)", name):
                return ("standready", "slot", "idle")
            return (f"hurt{side}", 7 if "impact" in name else HURT_CAP, "warp")
        return (f"hurtlight{side}" if "fast" in name else f"hurt{side}", None, "warp")
    # The held-on-ground loops replay whatever clip they get: a one-shot
    # fall_land would strobe, standready holds the pose (same as dhampir_m).
    if name.startswith("getup_"):
        return ("standready", "slot", "idle")
    # recovery_* is a one-shot rise: fall_land reads as a settle, ends standing.
    if name.startswith(("recovery_onback", "recovery_onstomach")):
        return ("fall_land", "slot", "idle")
    if name.startswith(("pole_", "wall_", "gun_", "harpoon_", "bite_",
                        "locked_gun", "locked_evade_gun", "rail_", "dismount_rail",
                        "throw_rayne", "sync_throw", "grab_attack")):
        return None
    if name == "ghost_float":
        # inshadow's pose is anchored at y=-6.11 (inside the shadow pool);
        # a plain fall pose is the least-wrong float for a rage state.
        return ("fall", 2, "idle")
    if name == "dash_rayne":
        # entershadow sinks her -6 into the pool; evadefront is a real lunge.
        return ("evadefront", None, "warp")
    if name == "brute_grab":
        return ("constraint", "slot", "idle")
    if name == "locked_taunt":
        return ("gesture1", "slot", "idle")
    return CLIP_MAP_E.get(clip)


CLIP_MAP_E = {
    "stand_alert": ("standdefault", None, "idle"),
    "stand_ambient01": ("standambient1", None, "idle"),
    "constraint": ("constraint", None, "idle"),
    "walk_forward": ("walkn", None, "warp"), "walk_forward_gun": ("walkn", None, "warp"),
    "walk_forward_start": ("walkn", "slot", "warp"),
    "run_start": ("runreadyn", "slot", "warp"), "run_forward": ("runreadyn", None, "warp"),
    "run_forward_left": ("runreadyn", None, "warp"), "run_forward_right": ("runreadyn", None, "warp"),
    "run_forward_gun": ("runreadyn", None, "warp"),
    "run_stop_left": ("standready", "slot", "idle"), "run_stop_right": ("standready", "slot", "idle"),
    "stand_quickturn": ("turnshuffle", "slot", "warp"), "move_quickturn": ("runreadyn", "slot", "warp"),
    "move_quickstop": ("standready", "slot", "idle"),
    "locked_idle": ("standready", None, "idle"),
    "locked_move_forward": ("walkreadyn", None, "warp"), "locked_move_backward": ("walkreadys", None, "warp"),
    "locked_walk_left": ("walkreadyw", None, "warp"), "locked_walk_right": ("walkreadye", None, "warp"),
    "locked_turn_left": ("turnshuffle", None, "warp"), "locked_turn_right": ("turnshuffle", None, "warp"),
    "locked_parry_hold": ("parryf", 2, "idle"), "locked_parry": ("parryf", None, "warp"),
    "die1": ("death", None, "death"), "dead1": ("death", "slot", "idle"),
    "phonemes": ("phonemes", "slot", "idle"),
    "expression_blink": ("phonemes", "slot", "idle"), "expression_sneer": ("phonemes", "slot", "idle"),
    "expression_stretch": ("phonemes", "slot", "idle"), "expression_breathout": ("phonemes", "slot", "idle"),
    "rage_blade": ("SPECIAL_BLADE",), "rage_kick": ("SPECIAL_KICK",),
    "bloodrage_start": ("standready", "slot", "idle"), "bloodrage_end": ("standready", "slot", "idle"),
    "bloodfury": ("standready", "slot", "idle"), "bloodfury_end": ("standready", "slot", "idle"),
    "bloodstorm_loop": ("standready", "slot", "idle"), "bloodstorm_end": ("standready", "slot", "idle"),
    "jump": ("jumpforward", 20, "warp"), "jump_forward": ("jumpforward", None, "warp"),
    "double_jump": ("jumpback", None, "warp"), "locked_evade_jump": ("jumpforward", 20, "warp"),
    "jump_descent": ("fall", 2, "idle"), "jump_forward_descent": ("fall", 2, "idle"),
    "locked_evade_jump_descent": ("fall", 2, "idle"),
    "fall": ("fall", 2, "idle"), "fall_long": ("fall", 2, "idle"),
    "jump_land": ("fall_land", None, "warp"), "jump_forward_land": ("fall_land", None, "warp"),
    "locked_evade_jump_land": ("fall_land", None, "warp"),
    "jumpforward_roll": ("fall_land", None, "warp"), "jumpforward_hard": ("fall_land", None, "warp"),
    "getup_face_down_loop": ("fall_land", "slot", "idle"), "getup_back_down_loop": ("fall_land", "slot", "idle"),
    "feed_repel": ("evadeback", None, "warp"),
    "chainpulldown": ("chainpulldown", None, "warp"),
}
for _side in ("left", "right", "front", "back"):
    CLIP_MAP_E[f"locked_evade_{_side}"] = (f"evade{_side}", None, "warp")
    CLIP_MAP_E[f"locked_evade_{_side}_b"] = ("B1", None, "attack")
    CLIP_MAP_E[f"locked_evade_{_side}_k"] = ("K1", None, "attack")
for _suffix in ("j", "jj", "jjj"):
    CLIP_MAP_E[f"locked_evade_front_{_suffix}"] = ("K1", None, "attack")
    CLIP_MAP_E[f"locked_evade_back_{_suffix}"] = ("K1", None, "attack")
CLIP_MAP_E["locked_evade_front_twist"] = ("evadefront", None, "warp")
for _idle in ("stand_alert_to_locked_idle", "locked_idle_to_stand_alert", "locked_idle_m_to_stand_alert"):
    CLIP_MAP_E[_idle] = ("standready", "slot", "idle")

SEGMENTS_E = {
    "B1": ("combo1", 26, "attack"), "B2": ("combo2", 24, "attack"), "B3": ("combo3", 28, "attack"),
    "K1": ("combo4", 19, "attack"), "K2": ("combo4", 40, "attack"), "K3": ("combo6", 24, "attack"),
    "SPECIAL_BLADE": ("combo5", 52, "attack"), "SPECIAL_KICK": ("combo4", 40, "attack"),
    "AIR_B": ("combo2", 30, "attack"), "AIR_K": ("combo4", 50, "attack"),
    "GROUND_B": ("combo2", 30, "attack"), "GROUND_K": ("combo4", 50, "attack"),
}
CHAIN = {("b", 1): "B1", ("b", 2): "B2", ("b", 3): "B3", ("k", 1): "K1", ("k", 2): "K2", ("k", 3): "K3"}


def kestral_donor(donor, index, slot):
    name = slot.name
    clip = slot.clip.replace("/", BS).split(BS)[-1].rsplit(".", 1)[0].lower()
    if clip == "home":
        return None
    if index == 46:
        return ("combo3", None, "attack")
    if index == 47:
        return SEGMENTS_K["SPECIAL_KICK"]
    if index == 48:
        return None
    if name.startswith("combo_") and "-idle" in name:
        return ("standready", "idle", "idle")
    if "groundstrike" in name:
        return SEGMENTS_K["GROUND_K"] if "_k_" in name else SEGMENTS_K["GROUND_B"]
    if index == 66:
        return SEGMENTS_K["GROUND_K"]
    if re.search(r"(?:^|_)j[bk]", name):
        if name.endswith("_hold"):
            return ("jumpupfall", 2, "idle")
        if name.endswith(("_end", "_land")):
            return ("jumpupland", "slot", "warp")
        return SEGMENTS_K["AIR_K"] if "jk" in name else SEGMENTS_K["AIR_B"]
    chain = re.search(r"_([bk]{1,3})(?:_m(?:irror)?)?$", name)
    if name.startswith("combo_") and chain:
        return SEGMENTS_K[CHAIN[(chain[1][0], len(chain[1]))]]
    if name.startswith("hurt_"):
        side = DM.hurt_side(name)
        if "tackle" in name:
            if re.search(r"(abort|wall|recov)", name):
                return ("standready", "slot", "idle")
            return (f"hurt{side}", 7 if "impact" in name else HURT_CAP, "warp")
        return (f"hurtlight{side}" if "fast" in name else f"hurt{side}", None, "warp")
    # No lying-hold clip exists for the down loops, and the real getup clips
    # start on the floor: loops hold standready like dhampir_m; one-shot
    # recoveries play jumpupland (her land-settle, same role as fall_land).
    if name.startswith("getup_"):
        return ("standready", "slot", "idle")
    if name.startswith(("recovery_onback", "recovery_onstomach")):
        return ("jumpupland", "slot", "idle")
    if name.startswith(("gun_", "harpoon_", "bite_", "locked_gun", "locked_evade_gun",
                        "throw_rayne", "sync_throw", "grab_attack")):
        return None
    if name == "ghost_float":
        return ("jumpupfall", 2, "idle")
    if name == "dash_rayne":
        return ("evadefront", None, "warp")
    if name == "brute_grab":
        return ("constraint", "slot", "idle")
    if name == "locked_taunt":
        return ("throw", "slot", "warp")
    if name in RAIL_MAP_K:
        return RAIL_MAP_K[name]
    if name in POLE_MAP_K:
        return POLE_MAP_K[name]
    return CLIP_MAP_K.get(clip)


CLIP_MAP_K = dict(CLIP_MAP_E)
CLIP_MAP_K.update({
    "dead1": ("death", None, "hold"),
    "jump": ("jumpupstart", None, "warp"), "jump_forward": ("jump", None, "warp"),
    "double_jump": ("jump16", None, "warp"), "locked_evade_jump": ("jumpupstart", None, "warp"),
    "jump_descent": ("jumpupfall", 2, "idle"), "jump_forward_descent": ("jumpupfall", 2, "idle"),
    "locked_evade_jump_descent": ("jumpupfall", 2, "idle"),
    "fall": ("jumpupfall", 2, "idle"), "fall_long": ("jumpupfall", 2, "idle"),
    "jump_land": ("jumpupland", None, "warp"), "jump_forward_land": ("jumpupland", None, "warp"),
    "locked_evade_jump_land": ("jumpupland", None, "warp"),
    "jumpforward_roll": ("jumpupland", None, "warp"), "jumpforward_hard": ("jumpupland", None, "warp"),
    "getup_face_down_loop": ("getupfront", "slot", "warp"), "getup_back_down_loop": ("getupback", "slot", "warp"),
    "feed_repel": ("evadeback", None, "warp"),
    "chainpulldown": ("chainpulldown", None, "warp"),
    "run_forward_gun": ("runreadyn", None, "warp"),
    # Kestral has no phonemes clip: expression slots idle on her combat pose.
    "phonemes": ("standready", "slot", "idle"),
    "expression_blink": ("standready", "slot", "idle"), "expression_sneer": ("standready", "slot", "idle"),
    "expression_stretch": ("standready", "slot", "idle"), "expression_breathout": ("standready", "slot", "idle"),
})
for _side in ("left", "right", "front", "back"):
    CLIP_MAP_K[f"locked_evade_{_side}_b"] = (f"evade{_side}blade", None, "attack")
    CLIP_MAP_K[f"locked_evade_{_side}_k"] = (f"evade{_side}kick", None, "attack")
for _suffix in ("j", "jj", "jjj"):
    CLIP_MAP_K[f"locked_evade_front_{_suffix}"] = ("evadefrontkick", None, "attack")
    CLIP_MAP_K[f"locked_evade_back_{_suffix}"] = ("evadebackkick", None, "attack")

SEGMENTS_K = {
    "B1": ("combo1", 26, "attack"), "B2": ("combo2", 24, "attack"), "B3": ("combo6", 28, "attack"),
    "K1": ("combo4", 19, "attack"), "K2": ("combo4", 40, "attack"), "K3": ("combo5", 50, "attack"),
    "SPECIAL_BLADE": ("railblade", None, "attack"), "SPECIAL_KICK": ("combo5", 50, "attack"),
    "AIR_B": ("combo2", 30, "attack"), "AIR_K": ("evadefrontkick", None, "attack"),
    "GROUND_B": ("combo2", 30, "attack"), "GROUND_K": ("evadefrontkick", None, "attack"),
}

RAIL_MAP_K = {
    "rail_slide": ("railslide", None, "warp"), "rail_slide_off": ("rail_slide_off", None, "warp"),
    "rail_slide_down": ("railslidedown", None, "warp"), "rail_slide_up": ("railslideup", None, "warp"),
    "rail_blade": ("railblade", None, "attack"),
    "rail_blade_impact_left": ("railblade", "slot", "attack"), "rail_blade_impact_right": ("railblade", "slot", "attack"),
    "rail_stand": ("railstand", None, "idle"), "rail_stand_turn_l": ("railstandturn", None, "warp"),
    "rail_stand_turn_r": ("railstandturn", None, "warp"), "rail_idle": ("railstand", 2, "idle"),
    "rail_walk": ("railrun10", None, "warp"), "rail_run_10": ("railrun10", None, "warp"),
    "rail_run_20": ("railrun20", None, "warp"), "rail_run_30": ("railrun30", None, "warp"),
    "rail_run": ("railrun45", None, "warp"),
    "dismount_rail_left": ("dismountrailleft", None, "warp"),
    "dismount_rail_left_fall": ("dismountrailleftfall", None, "idle"),
    "dismount_rail_left_land": ("dismountrailleftland", None, "warp"),
    "dismount_rail_right": ("dismountrailright", None, "warp"),
    "dismount_rail_right_fall": ("dismountrailrightfall", None, "idle"),
    "dismount_rail_right_land": ("dismountrailrightland", None, "warp"),
    "dismount_rail_center": ("rail_slide_off", None, "warp"),
    "dismount_rail_to_vpole": ("rail_slide_off", None, "warp"),
    "dismount_rail_to_hpole": ("rail_slide_off", None, "warp"),
}

# Her stock SKB plays rayne\*.ani clips on the pole/wall states (they bind by
# name on her dhampir rig). Map the player's pole/wall slots to the same
# donor states so her pole play is exactly the stock behaviour.
POLE_MAP_K = {
    "pole_horz_grab": ("pole_horz_grab", None, "warp"),
    "pole_horz_grab_above": ("pole_horz_grab_above", None, "warp"),
    "pole_horz_perch": ("pole_horz_perch", None, "warp"),
    "pole_horz_idle": ("pole_horz_idle", None, "idle"),
    "pole_horz_salto_start": ("pole_horz_salto_start", None, "warp"),
    "pole_horz_salto": ("pole_horz_salto", None, "warp"),
    "pole_horz_salto_turn": ("pole_horz_salto_turn", None, "warp"),
    "pole_horz_salto_release": ("pole_horz_salto_release", None, "warp"),
    "pole_horz_salto_release_high": ("pole_horz_salto_release_high", None, "warp"),
    "pole_horz_salto_release_off": ("pole_horz_salto_release_off", None, "warp"),
    "pole_horz_turn": ("pole_horz_turn", None, "warp"),
    "pole_horz_hang_down": ("pole_horz_hang_down", None, "warp"),
    "pole_horz_hang_idle": ("pole_horz_hang_idle", None, "idle"),
    "pole_horz_hang_kick": ("pole_horz_hang_kick", None, "attack"),
    "pole_horz_hang_off": ("pole_horz_hang_off", None, "warp"),
    "pole_horz_hang_perch": ("pole_horz_hang_perch", None, "warp"),
    "pole_horz_off": ("pole_horz_off", None, "warp"),
    "wall_grab": ("wall_grab", None, "warp"),
    "wall_idle": ("wall_idle", 2, "idle"),
    "wall_jump": ("wall_jump", None, "warp"),
    "wall_jump_long": ("wall_jump_long", None, "warp"),
    "wall_jump_to_horz_pole": ("wall_jump_to_horz_pole", None, "warp"),
    "wall_jump_to_horz_long": ("wall_jump_to_horz_long", None, "warp"),
    "wall_jump_off": ("wall_jump_off", None, "warp"),
}


# KAGAN (60 enemy states, 67 bones)
# - Full graft: all missing player bones (67+42=109 < 120). No plain walk/run
#   clips exist: walkreadyn/runreadyn are his only locomotion. No front evade
#   either: locked_evade_front reuses his forward jump hop.
# - His parry set is parryhigh/middle/low (+impacts); locked_parry plays
#   parrymiddle, the frontal read.
# - locked_evade_*_b/_k counters use his closeinattack strikes; the jump-attack
#   chain uses jumpattack and the ground strikes use closeinattack01/03.
# - Triangle: shockwave (118f AoE slam). The cast/AoE clips carry no melee
#   windows (the boss AI spawned the damage entities), so EXTRA_HITS_KG
#   injects native blade windows - the animation stays his.
# - die1 -> death, dead1 -> deathidle: a real lying pose (y=-2.13) instead of
#   a frozen death frame. deathheadchop is Rayne's finisher ON him - unused.
# - Attachments: "sunbeam" (L Hand 31) becomes bladeL; "sword" (a bone -1 FX
#   marker) becomes bladeR patched onto R Hand 32. gunR stays unused.
# - harpoon_pull maps to chainpulldown (his only chain move).
#
# NINJA F/M (98/97 states, 40/34 bones)
# - Full graft (97/99 < 120). Rich own tables: walkn/s + runn/s + the full
#   walkready/runready strafe set, parryl/r/f + impacts, five hurt variants
#   per side, hurt*tackle ending near standing (-1.0), real getup clips
#   (start lying -3.1, unusable in the short-circuited chain), stumble,
#   feedrepel (a real repel kick), activatesword, throwstar (no projectile -
#   unused), backup as the only dodge.
# - No fall/fall_land clips: aerial holds and landings reuse jump/stumble.
# - No dedicated death clip. Ninja F uses the fatality-victim animation.
#   Ninja M uses the falling part of his frontal knockdown and holds its final pose.
# - locked_taunt plays activatesword (the blade ignition - signature).
# - Attachment: sword sits on R Hand (F:31, M:25) -> bladeR; a cloned record
#   is appended as bladeL on the other hand (F:30, M:24).
# - harpoon/feed/mounted/fatality and legless states stay unmapped: they are
#   victim anims, not moves the player can trigger.


def translate_hits_boss(donor, used, bare_blade="bbladedamage"):
    """Kagan/ninja damage windows as native player ranges.

    Their tables spell hits `bladedamageN` (no side) and `rhanddamageN`;
    the native ranges only know l/r/b bladedamage + l/r footdamage.
    Bare bladedamageN uses the configured blade; handdamage -> footdamage (the closest
    native range). Same l+r blade merge as DM.translate_hits."""
    hits = []
    for name, start, end in donor.windows:
        match = re.match(r"([lr])?(blade|foot|hand)damage\d$", name)
        if not match or start > used - 4:
            continue
        side, limb = match[1], match[2]
        if limb == "hand":
            rng = f"{side or 'r'}footdamage"
        elif side:
            rng = f"{side}{limb}damage"
        else:
            rng = bare_blade
        hits.append([rng, float(start), float(min(end, used - 1))])
    merged = []
    for hit in sorted(hits, key=lambda h: (h[1], h[0])):
        same = next((m for m in merged if m[0] == hit[0] and hit[1] <= m[2] + 3), None)
        if same:
            same[2] = max(same[2], hit[2])
            continue
        pair = next((m for m in merged if m[0] in ("lbladedamage", "rbladedamage")
                     and hit[0] in ("lbladedamage", "rbladedamage") and m[0] != hit[0]
                     and abs(m[1] - hit[1]) <= 3), None)
        if pair:
            pair[0], pair[1], pair[2] = "bbladedamage", min(pair[1], hit[1]), max(pair[2], hit[2])
        else:
            merged.append(hit)
    return [tuple(m) for m in merged]


def translate_single_sword_hits(donor, used):
    return translate_hits_boss(donor, used, bare_blade="rbladedamage")


EMPTY_JUG = b"Version\r\n13\r\nboneCount\r\n0\r\nboneList\r\ncollideBoneCount\r\n0\r\nboneLinkCount\r\n0\r\n"


def _bfm_cursor_end(data):
    """Walk the BFM the way Model.read does and return the offset where the
    pre-geometry alignment padding starts (end of the mesh descriptors)."""
    part_count, count, materials, attachments = unpack_from("<4I", data, 8)
    desc = 112 + 58 * part_count + 76 * attachments + 360 * materials + 44 * count
    cursor = desc + 4
    for _ in range(unpack_from("<I", data, desc)[0]):
        parts = unpack_from("<I", data, cursor + 8)[0]
        cursor += 12 + parts * 2
        cursor += 4 + unpack_from("<I", data, cursor)[0] * 2
        cursor += 24
    return cursor


def patch_bfm_attachments(model, ops):
    """Attachment record surgery beyond simple renames.

    Records are name(24) + bone index(4) + affine matrix(48) = 76 bytes.
    ops: ("rename", old, new) | ("bone", name, index) | ("append", name, src, index)
         | ("transform", name, matrix) - replace the local affine matrix

    An append inserts 76 bytes mid-file; the reader computes section offsets
    positionally so only the 16-aligned geometry contract can break: the dead
    padding before the mesh data is then grown/shrunk by the alignment delta."""
    if not ops:
        return model
    data = bytearray(model)
    part_count, attachments = unpack_from("<II", data, 8)[0], unpack_from("<I", data, 20)[0]
    attached = 112 + 58 * part_count

    def find(name):
        want = name.encode()
        return next((i for i in range(attachments)
                     if bytes(data[attached + i * 76:attached + i * 76 + 24]).split(b"\0", 1)[0] == want), None)

    for op in ops:
        if op[0] == "rename":
            i = find(op[1])
            if i is None:
                raise ValueError(f"attachment {op[1]} not found")
            data[attached + i * 76:attached + i * 76 + 24] = fixed(op[2], 24)
        elif op[0] == "bone":
            i = find(op[1])
            if i is None:
                raise ValueError(f"attachment {op[1]} not found")
            pack_into("<i", data, attached + i * 76 + 24, op[2])
        elif op[0] == "transform":
            i = find(op[1])
            if i is None:
                raise ValueError(f"attachment {op[1]} not found")
            pack_into("<12f", data, attached + i * 76 + 28, *op[2])
        elif op[0] == "append":
            src = find(op[2])
            if src is None:
                raise ValueError(f"attachment {op[2]} not found")
            rec = bytearray(data[attached + src * 76:attached + (src + 1) * 76])
            rec[:24] = fixed(op[1], 24)
            pack_into("<i", rec, 24, op[3])
            at = attached + attachments * 76
            data[at:at] = rec
            attachments += 1
            pack_into("<I", data, 20, attachments)
            # Re-align the geometry section: adjust the dead padding in front
            # of the mesh data by the alignment delta the insert introduced.
            cursor_end = _bfm_cursor_end(data)
            geom_actual = ((cursor_end - 76 + 15) & ~15) + 76
            pad_delta = (cursor_end + 15 & ~15) - geom_actual
            if pad_delta > 0:
                data[geom_actual:geom_actual] = b"\0" * pad_delta
            elif pad_delta < 0:
                del data[geom_actual + pad_delta:geom_actual]
    return bytes(data)


def kagan_donor(donor, index, slot):
    """Return (kagan state | segment key, cap, rate mode) or None."""
    name = slot.name
    clip = slot.clip.replace("/", BS).split(BS)[-1].rsplit(".", 1)[0].lower()
    if clip == "home":
        return None
    if index == 46:
        return SEGMENTS_KG["SPECIAL_BLADE"]
    if index == 47:
        return SEGMENTS_KG["SPECIAL_KICK"]
    if index == 48:
        return None
    if name.startswith("combo_") and "-idle" in name:
        return ("standready", "idle", "idle")
    if "groundstrike" in name:
        return SEGMENTS_KG["GROUND_K"] if "_k_" in name else SEGMENTS_KG["GROUND_B"]
    if index == 66:
        return SEGMENTS_KG["GROUND_K"]
    if re.search(r"(?:^|_)j[bk]", name):
        if name.endswith("_hold"):
            return ("fall", 2, "idle")
        if name.endswith(("_end", "_land")):
            return ("fall_land", "slot", "warp")
        return SEGMENTS_KG["AIR_K"] if "jk" in name else SEGMENTS_KG["AIR_B"]
    chain = re.search(r"_([bk]{1,3})(?:_m(?:irror)?)?$", name)
    if name.startswith("combo_") and chain:
        return SEGMENTS_KG[CHAIN[(chain[1][0], len(chain[1]))]]
    if name.startswith("hurt_"):
        side = DM.hurt_side(name)
        if "tackle" in name:
            if re.search(r"(abort|wall|recov)", name):
                return ("standready", "slot", "idle")
            return (f"hurt{side}", 7 if "impact" in name else HURT_CAP, "warp")
        # No hurtlight clips: fast and strong hurts both play hurt{side}.
        return (f"hurt{side}", None, "warp")
    # No lying/getup clips: loops hold standready, recoveries use fall_land.
    if name.startswith("getup_"):
        return ("standready", "slot", "idle")
    if name.startswith(("recovery_onback", "recovery_onstomach")):
        return ("fall_land", "slot", "idle")
    if name == "harpoon_pull":
        return ("chainpulldown", None, "warp")
    if name.startswith(("pole_", "wall_", "gun_", "harpoon_", "bite_",
                        "locked_gun", "locked_evade_gun", "rail_", "dismount_rail",
                        "throw_rayne", "sync_throw", "grab_attack")):
        return None
    if name == "ghost_float":
        return ("fall", 2, "idle")
    if name == "dash_rayne":
        return ("evadeback", None, "warp")
    if name == "brute_grab":
        return ("constraint", "slot", "idle")
    if name == "locked_taunt":
        return ("gesture1", "slot", "idle")
    return CLIP_MAP_KG.get(clip)


CLIP_MAP_KG = dict(CLIP_MAP_E)
CLIP_MAP_KG.update({
    # No standdefault/ambient set: his combat stance is his idle.
    "stand_alert": ("standready", None, "idle"),
    "stand_ambient01": ("standreadyidle1", None, "idle"),
    # No plain walk/run clips: walkreadyn/runreadyn are his only locomotion.
    "walk_forward": ("walkreadyn", None, "warp"), "walk_forward_gun": ("walkreadyn", None, "warp"),
    "walk_forward_start": ("walkreadyn", "slot", "warp"),
    # parrymiddle is the frontal read of his three parry heights.
    "locked_parry_hold": ("parrymiddle", 2, "idle"), "locked_parry": ("parrymiddle", None, "warp"),
    # His only jump is a forward hop; every descent/land reuses fall/fall_land.
    "jump": ("jump", 20, "warp"), "jump_forward": ("jump", None, "warp"),
    "double_jump": ("jump", None, "warp"), "locked_evade_jump": ("jump", 20, "warp"),
    # Real lying pose (y=-2.13) instead of a frozen death frame.
    "dead1": ("deathidle", "slot", "idle"),
})
for _side in ("left", "right", "back"):
    CLIP_MAP_KG[f"locked_evade_{_side}"] = (f"evade{_side}", None, "warp")
    CLIP_MAP_KG[f"locked_evade_{_side}_b"] = ("closeinattack01", 30, "attack")
    CLIP_MAP_KG[f"locked_evade_{_side}_k"] = ("closeinattack03", 40, "attack")
# No front evade exists: his forward jump hop covers locked_evade_front.
CLIP_MAP_KG["locked_evade_front"] = ("jump", 20, "warp")
CLIP_MAP_KG["locked_evade_front_b"] = ("closeinattack01", 30, "attack")
CLIP_MAP_KG["locked_evade_front_k"] = ("closeinattack03", 40, "attack")
CLIP_MAP_KG["locked_evade_front_twist"] = ("jump", 20, "warp")
for _suffix in ("j", "jj", "jjj"):
    CLIP_MAP_KG[f"locked_evade_front_{_suffix}"] = ("closeinattack02", 38, "attack")
    CLIP_MAP_KG[f"locked_evade_back_{_suffix}"] = ("closeinattack02", 38, "attack")

SEGMENTS_KG = {
    "B1": ("combo1", 18, "attack"), "B2": ("combo2", 18, "attack"), "B3": ("combo3", 32, "attack"),
    "K1": ("combo4", 15, "attack"), "K2": ("combo5", 17, "attack"), "K3": ("combo6", 46, "attack"),
    "SPECIAL_BLADE": ("shockwave", 64, "attack"), "SPECIAL_KICK": ("firesuperray", None, "attack"),
    "AIR_B": ("jumpattack", 40, "attack"), "AIR_K": ("closeinattack02", 38, "attack"),
    "GROUND_B": ("closeinattack01", 30, "attack"), "GROUND_K": ("closeinattack03", 41, "attack"),
}
EXTRA_HITS_KG = {
    # Keep the existing approximation for the separate circle-kick slot.
    "firesuperray": [("rbladedamage", 40.0, 100.0)],
}


def ninja_donor(donor, index, slot):
    """Return (ninja state | segment key, cap, rate mode) or None."""
    name = slot.name
    clip = slot.clip.replace("/", BS).split(BS)[-1].rsplit(".", 1)[0].lower()
    if clip == "home":
        return None
    if index == 46:
        return SEGMENTS_N["SPECIAL_BLADE"]
    if index == 47:
        return SEGMENTS_N["SPECIAL_KICK"]
    if index == 48:
        return None
    if name.startswith("combo_") and "-idle" in name:
        return ("standready", "idle", "idle")
    if "groundstrike" in name:
        return SEGMENTS_N["GROUND_K"] if "_k_" in name else SEGMENTS_N["GROUND_B"]
    if index == 66:
        return SEGMENTS_N["GROUND_K"]
    if re.search(r"(?:^|_)j[bk]", name):
        if name.endswith("_hold"):
            return ("jump", 2, "idle")
        if name.endswith(("_end", "_land")):
            return ("stumble", "slot", "warp")
        return SEGMENTS_N["AIR_K"] if "jk" in name else SEGMENTS_N["AIR_B"]
    chain = re.search(r"_([bk]{1,3})(?:_m(?:irror)?)?$", name)
    if name.startswith("combo_") and chain:
        return SEGMENTS_N[CHAIN[(chain[1][0], len(chain[1]))]]
    if name.startswith("hurt_"):
        side = DM.hurt_side(name)
        if "tackle" in name:
            if re.search(r"(abort|wall|recov)", name):
                return ("standready", "slot", "idle")
            # hurt*tackle ends near standing (y~-1.0) - safe in the short chain.
            return (f"hurt{side}tackle", 20 if "impact" in name else None, "warp")
        # No hurtlight clips: fast hurts play the snappier variant 1.
        return (f"hurt{side}1" if "fast" in name else f"hurt{side}", None, "warp")
    # The real getup clips start lying (-3.1): unusable in the short chain.
    if name.startswith("getup_"):
        return ("standready", "slot", "idle")
    if name.startswith(("recovery_onback", "recovery_onstomach")):
        return ("stumble", "slot", "idle")
    if name.startswith(("pole_", "wall_", "gun_", "harpoon_", "bite_",
                        "locked_gun", "locked_evade_gun", "rail_", "dismount_rail",
                        "throw_rayne", "sync_throw", "grab_attack")):
        return None
    if name == "ghost_float":
        return ("jump", 2, "idle")
    if name == "dash_rayne":
        return ("backup", None, "warp")
    if name == "brute_grab":
        return ("constraint", "slot", "idle")
    if name == "locked_taunt":
        # The blade ignition: their signature flourish.
        return ("activatesword", "slot", "idle")
    return CLIP_MAP_N.get(clip)


def ninjam_donor(donor, index, slot):
    if index == 212:
        return ("hurtfronttackle", NINJAM_FALL_FRAMES, "death")
    if index == 213:
        return ("hurtfronttackle", 2, "idle")
    return ninja_donor(donor, index, slot)


CLIP_MAP_N = dict(CLIP_MAP_E)
CLIP_MAP_N.update({
    "stand_alert": ("standdefault", None, "idle"),
    "stand_ambient01": ("standambient1", None, "idle"),
    "walk_forward": ("walkn", None, "warp"), "walk_forward_gun": ("walkn", None, "warp"),
    "walk_forward_start": ("walkn", "slot", "warp"),
    "run_start": ("runn", "slot", "warp"), "run_forward": ("runn", None, "warp"),
    "run_forward_left": ("runn", None, "warp"), "run_forward_right": ("runn", None, "warp"),
    "run_forward_gun": ("runn", None, "warp"),
    "locked_parry_hold": ("parryf", 2, "idle"), "locked_parry": ("parryf", None, "warp"),
    # One jump clip only: every air state reuses it (there are no fall/land clips).
    "jump": ("jump", 20, "warp"), "jump_forward": ("jump", None, "warp"),
    "double_jump": ("jump", None, "warp"), "locked_evade_jump": ("jump", 20, "warp"),
    "jump_descent": ("jump", 2, "idle"), "jump_forward_descent": ("jump", 2, "idle"),
    "locked_evade_jump_descent": ("jump", 2, "idle"),
    "fall": ("jump", 2, "idle"), "fall_long": ("jump", 2, "idle"),
    "jump_land": ("stumble", None, "warp"), "jump_forward_land": ("stumble", None, "warp"),
    "locked_evade_jump_land": ("stumble", None, "warp"),
    "jumpforward_roll": ("stumble", None, "warp"), "jumpforward_hard": ("stumble", None, "warp"),
    # Ninja F uses the stock reaction to the blade fatality.
    "die1": ("altStandBladeFatalityBack", None, "death"),
    "dead1": ("altStandBladeFatalityBack", None, "hold"),
    "feed_repel": ("feedrepel", None, "warp"),
    "phonemes": ("standready", "slot", "idle"),
    "expression_blink": ("standready", "slot", "idle"), "expression_sneer": ("standready", "slot", "idle"),
    "expression_stretch": ("standready", "slot", "idle"), "expression_breathout": ("standready", "slot", "idle"),
})
for _side in ("left", "right", "front", "back"):
    CLIP_MAP_N[f"locked_evade_{_side}"] = ("backup", None, "warp")
    CLIP_MAP_N[f"locked_evade_{_side}_b"] = ("combo1", 17, "attack")
    CLIP_MAP_N[f"locked_evade_{_side}_k"] = ("combo4", 16, "attack")
for _suffix in ("j", "jj", "jjj"):
    CLIP_MAP_N[f"locked_evade_front_{_suffix}"] = ("combo4", 16, "attack")
    CLIP_MAP_N[f"locked_evade_back_{_suffix}"] = ("combo4", 16, "attack")
CLIP_MAP_N["locked_evade_front_twist"] = ("backup", None, "warp")

SEGMENTS_N = {
    "B1": ("combo1", 17, "attack"), "B2": ("combo2", 15, "attack"), "B3": ("combo3", 44, "attack"),
    "K1": ("combo4", 16, "attack"), "K2": ("combo4", 25, "attack"), "K3": ("combo4", 48, "attack"),
    "SPECIAL_BLADE": ("closeinshoulder", None, "attack"), "SPECIAL_KICK": ("combo4", None, "attack"),
    "AIR_B": ("combo1", 17, "attack"), "AIR_K": ("combo4", 16, "attack"),
    "GROUND_B": ("combo1", 17, "attack"), "GROUND_K": ("combo4", 16, "attack"),
}


def build_states(donor_skb, rayne_skb, donor_for, segmap, audit=None, donor_sfx=False,
                 hits=DM.translate_hits, hit_names=("combo", "evade", "railblade"),
                 extra_hits=None, clip_overrides=None, death_rate=DEATH_RATE):
    _, ranges, player, rayne_rates = F.layout(rayne_skb)
    _, _, donor_states, donor_rates, _ = F.motion_layout(donor_skb)
    if any(state.name == "closeinshoulder" for state in donor_states):
        ranges.append("shoulderhit")
    donors = {s.name: (s, r) for s, r in zip(donor_states, donor_rates)}
    extras = DM.read_player_slots(rayne_skb)
    donor_extras = DM.read_player_slots(donor_skb) if donor_sfx else None
    donor_extra_by_name = {s.name: e for s, e in zip(donor_states, donor_extras)} if donor_sfx else {}
    out = bytearray(pack("<I", len(ranges)))
    out.extend(b"".join(fixed(n, 16) for n in ranges))
    out.extend(pack("<I", len(player)))
    mapped_count = 0
    for index, slot in enumerate(player):
        entry = donor_for(donors, index, slot)
        if entry and entry[0] in segmap:
            entry = segmap[entry[0]]
        mapped = entry is not None
        if mapped:
            mapped_count += 1
            state_name, cap, mode = entry
            d, donor_rate = donors[state_name]
            used = d.frames
            if cap == "slot" or cap == "idle":
                used = min(used, IDLE_CAP if cap == "idle" else slot.frames)
            elif cap:
                used = min(used, cap)
            if index in AIR_HOLD:
                used = 2
            if mode == "attack":
                rate = ATTACK_RATE
            elif mode == "death":
                rate = death_rate
            elif mode == "hold":
                rate = HOLD_RATE
            elif mode == "idle":
                rate = donor_rate
            else:
                rate = min(RATE_MAX, max(RATE_MIN, used * rayne_rates[index] / max(1, slot.frames)))
        else:
            d, donor_rate = donors["standready"]
            used, rate, mode = min(slot.frames, UNMAPPED_CAP), 30.0, "idle"
        next_state = slot.next_state
        if not mapped and next_state < 0 and slot.name != "constraint":
            next_state = 0
        if 163 <= index <= 198:
            next_state = 0
        if index == 426:
            next_state = -2

        wave = d.name == "shockwave"
        shoulder = index == 46 and d.name == "closeinshoulder"
        scale = (used - 1) / max(1, slot.frames - 1)
        hurt = slot.name.startswith("hurt_")
        windows = []
        if mapped:
            for name, start, end in slot.windows:
                if name not in F.PLAYER_RANGES:
                    continue
                if name == "nogravity" and not KEEP_NOGRAVITY.match(slot.name):
                    continue
                if hurt and name in HURT_FLIGHT:
                    continue
                if name == "combocontinue" and "-idle" in slot.name:
                    windows.append((name, 0.0, float(used - 1)))
                else:
                    windows.append((name, start * scale, min(end * scale, used - 1)))
            if shoulder:
                windows.append(("shoulderhit", 23.0, 26.0))
            elif d.name.startswith(hit_names):
                windows.extend(hits(d, used))
            if extra_hits:
                windows.extend((n, s, min(e, used - 1)) for n, s, e in extra_hits.get(d.name, ())
                               if s <= used - 4)
            # Donor windows already spelled like a native player range pass
            # straight through (Kestral's pole_kick l/rfootdamage has no digit).
            for n, s, e in d.windows:
                if n in DAMAGE_RANGES and s <= used - 4:
                    windows.append((n, float(s), float(min(e, used - 1))))
            if d.name.startswith(("dismountrail",)):
                windows.extend((n, s, min(e, used - 1)) for n, s, e in d.windows if n == "nogravity")
        if index in SPECIAL_READY:
            windows.append(("allowharpoon", 0.0, float(used - 1)))
        if clip_overrides and clip_overrides.get(index) == EPHEMERA_CLIPS[46]:
            windows = [(name, 10.0, 16.0) if name == "rbladedamage" and start == 12.0
                        else (name, start, end) for name, start, end in windows]
            windows.append(("allowturning", 0.0, 12.0))
        if clip_overrides and clip_overrides.get(index) == KESTRAL_CLIPS[46]:
            # Keep the wind-up and the gap between the kicks and slashes steerable.
            windows.extend((("allowturning", 0.0, 9.0), ("allowturning", 30.0, 49.0)))

        samples, signals = [], DM.strike_id_events(windows)
        custom_strikes = bool(signals)
        if wave:
            # Stock shockwave audio cues the ground impact on this same frame.
            signals.append((37, 0, 0x7F00))
        if mapped:
            rayne_samples, rayne_events = extras[index]
            for frame, kind, ident in (() if wave or shoulder else rayne_events):
                at = min(used - 1, round(frame * scale))
                if kind == 1:
                    signals.append((at, 1, len(samples)))
                    samples.append(rayne_samples[ident])
                elif not (custom_strikes and kind == 0 and ident in (1204, 1205, 1301, 1302, 1315)):
                    signals.append((at, kind, ident))
            # The boss's own combat barks land at her clip's native moments.
            d_samples, d_events = donor_extra_by_name.get(d.name, ([], []))
            d_scale = (used - 1) / max(1, d.frames - 1)
            for frame, kind, ident in d_events:
                if kind != 1:
                    continue
                if wave and frame >= used:
                    continue
                at = frame if wave else min(used - 1, round(frame * d_scale))
                signals.append((at, 1, len(samples)))
                samples.append(d_samples[ident])
        clip = clip_overrides.get(index, clip_overrides.get(d.name, d.clip)) if clip_overrides else d.clip
        out.extend(fixed(slot.name, 30) + fixed(clip, 64))
        out.extend(pack("<fIfifII", rate, used, 0.10, next_state, float(used - 1), 0, len(samples)))
        out.extend(b"".join(samples))
        out.extend(pack("<I", len(signals)))
        for signal in sorted(signals):
            out.extend(pack("<HHI", *signal))
        out.extend(pack("<II", 0, len(windows)))
        for name, start, end in windows:
            out.extend(pack("<Iff", ranges.index(name), start, end))
        if audit is not None:
            audit.append(f"{index:3} {slot.name:32} -> "
                         + (f"{d.name:18} frames={used:3} rate={rate:5.1f} next={next_state:3} "
                            f"win={[(n, round(s), round(e)) for n, s, e in windows if n in DAMAGE_RANGES or n in ('nogravity', 'allowharpoon')]}"
                            if mapped else f"(unmapped) standready frames={used}"))
    return bytes(out), mapped_count


def ani_tracks(data):
    """Read the packed v3 tracks in the order used by CAnimation::load."""
    version, frames, count, extra_tracks, raw_size = unpack_from("<5I", data)
    assert version == 3
    descriptor = 20 + raw_size
    cursor = 20
    tracks = []
    strides = (16, 16, 4, 4, 4, 6, 6, 6, 8)
    for i in range(count):
        name, kind, keys = unpack_from("<24sII", data, descriptor + i * 32)
        stride = strides[kind]
        tracks.append((name, kind, keys, stride, cursor))
        cursor += (keys * stride + 3) & ~3
    roots, rotations = unpack_from("<II", data, descriptor + count * 32)
    assert rotations == 0 and cursor + roots * 16 <= descriptor
    assert descriptor + count * 32 + 8 + extra_tracks * 60 == len(data)
    return tracks, cursor, roots


def scale_ani_travel(data, scale):
    _, offset, roots = ani_tracks(data)
    for i in range(roots):
        key = offset + i * 16
        _, x, y, z = unpack_from("<I3f", data, key)
        pack_into("<3f", data, key + 4, x * scale, y, z * scale)


def ephemera_clips(common):
    death = bytearray(common.read(common.by_name[r"ANIMATIONS\EPHEMERA\DEATH.ANI"]))
    flurry = bytearray(common.read(common.by_name[r"ANIMATIONS\EPHEMERA\COMBO5.ANI"]))

    # The boss's clip raises the pelvis for its first 31 frames. Replace that
    # arc with a descent that meets the original impact pose on frame 51.
    tracks, _, _ = ani_tracks(death)
    _, _, keys, stride, offset = next(track for track in tracks
        if track[0].split(b"\0", 1)[0] == b"Bip01 Pelvis" and track[1] == 0)
    pelvis = [unpack_from("<I3f", death, offset + i * stride) for i in range(keys)]
    landing_frame = 51
    start_y = pelvis[0][2]
    landing_y = next(y for frame, _, y, _ in pelvis if frame == landing_frame)
    for i, (frame, _, y, _) in enumerate(pelvis):
        if frame >= landing_frame:
            break
        cap = start_y + (landing_y - start_y) * (frame / landing_frame) ** 1.65
        pack_into("<f", death, offset + i * stride + 8, min(y, cap))

    # A separate two-frame pose, like Rayne's DEAD1.ANI. Keep the final key
    # of each bone and omit the accumulated root motion of the fall.
    raw, descriptors = bytearray(), bytearray()
    for name, kind, keys, stride, offset in tracks:
        assert kind in (0, 2, 3, 8) and keys > 0
        last_key = offset + (keys - 1) * stride
        frame_size = 4 if kind == 0 else 2
        raw.extend(bytes(frame_size) + death[last_key + frame_size:last_key + stride])
        raw.extend(bytes(-len(raw) % 4))
        descriptors.extend(pack("<24sII", name, kind, 1))
    dead = pack("<5I", 3, 2, len(tracks), 0, len(raw)) + raw + descriptors + bytes(8)
    ani_tracks(dead)

    # Keep the animated swings intact and make the advance cover melee range.
    scale_ani_travel(flurry, 0.35)
    return {EPHEMERA_CLIPS[46]: flurry, EPHEMERA_CLIPS[212]: death, EPHEMERA_CLIPS[213]: dead}


def kestral_clips(common):
    combo = bytearray(common.read(common.by_name[r"ANIMATIONS\KESTRAL\COMBO3.ANI"]))
    scale_ani_travel(combo, 0.60)
    return {KESTRAL_CLIPS[46]: combo}


def ninjam_clips(common):
    source = common.read(common.by_name[r"ANIMATIONS\NINJAM\HURTFRONTTACKLE.ANI"])
    tracks, root_offset, root_count = ani_tracks(source)
    assert unpack_from("<I", source, 12)[0] == 0
    fall_raw, fall_tracks = bytearray(), bytearray()
    dead_raw, dead_tracks = bytearray(), bytearray()
    for name, kind, count, stride, offset in tracks:
        frame_format = "<I" if kind == 0 else "<H"
        frame_size = 4 if kind == 0 else 2
        kept = 0
        for key in range(offset, offset + count * stride, stride):
            if unpack_from(frame_format, source, key)[0] >= NINJAM_FALL_FRAMES:
                break
            kept += 1
        if not kept:
            continue
        end = offset + kept * stride
        fall_raw.extend(source[offset:end])
        fall_raw.extend(bytes(-len(fall_raw) % 4))
        fall_tracks.extend(pack("<24sII", name, kind, kept))
        dead_raw.extend(bytes(frame_size) + source[end - stride + frame_size:end])
        dead_raw.extend(bytes(-len(dead_raw) % 4))
        dead_tracks.extend(pack("<24sII", name, kind, 1))

    roots = 0
    for i in range(root_count):
        frame, x, y, z = unpack_from("<I3f", source, root_offset + 16 * i)
        if frame >= NINJAM_FALL_FRAMES:
            break
        # Keep a short recoil instead of the boss AI's full slide.
        fall_raw.extend(pack("<I3f", frame, x * 0.35, y, z * 0.35))
        roots += 1
    count = len(fall_tracks) // 32
    fall = (pack("<5I", 3, NINJAM_FALL_FRAMES, count, 0, len(fall_raw))
            + fall_raw + fall_tracks + pack("<II", roots, 0))
    dead = (pack("<5I", 3, 2, count, 0, len(dead_raw))
            + dead_raw + dead_tracks + bytes(8))
    ani_tracks(fall)
    ani_tracks(dead)
    return {NINJAM_CLIPS[212]: fall, NINJAM_CLIPS[213]: dead}


def kagan_clips(common):
    clips = {}
    for name, clip in KAGAN_CLIPS.items():
        data = bytearray(common.read(common.by_name[fr"ANIMATIONS\KAGAN\{name.upper()}.ANI"]))
        _, offset, roots = ani_tracks(data)
        keys = [unpack_from("<I3f", data, offset + 16 * i) for i in range(roots)]
        if name == "shockwave":
            # Keep the ground strike at frame 37, then make the first leap a
            # short hop. Back on the ground at 62 before the state exits.
            hop = {53: (50, 1.0), 66: (62, 0.0)}
            assert {53, 66} <= {frame for frame, *_ in keys}
            for i, (frame, _, _, _) in enumerate(keys):
                at, height = hop.get(frame, (frame, 0.0))
                pack_into("<I3f", data, offset + 16 * i, at, 0.0, height, 0.0)
        else:
            travel = max(((x * x + z * z) ** 0.5 for _, x, _, z in keys), default=0.0)
            if travel > 0.9:
                scale_ani_travel(data, 0.9 / travel)
        clips[clip] = data
    return clips


CHARS = {
    "ephemera": dict(
        asset="br2_ephemera_player",
        donor_skb=r"DATA\EPHEMERA.SKB", donor_bfm=r"MODELS\EPHEMERA.BFM",
        donor_jug=r"MODELS\EPHEMERA.JUG", donor_sbs=r"MODELS\EPHEMERA.SBS",
        donor_for=ephemera_donor, segmap=SEGMENTS_E,
        graft_skip=lambda n: bool(DECORATIVE.match(n)),
        attach_patch=[("rename", "blade_L", "bladeL"), ("rename", "blade_R", "bladeR")],
        donor_sfx=True,
        clip_overrides=EPHEMERA_CLIPS, build_clips=ephemera_clips, death_rate=55.0,
    ),
    "kestral": dict(
        asset="br2_kestral_player",
        donor_skb=r"DATA\KESTRAL.SKB", donor_bfm=r"MODELS\KESTRAL.BFM",
        donor_jug=r"MODELS\KESTRAL.JUG", donor_sbs=r"MODELS\KESTRAL.SBS",
        donor_for=kestral_donor, segmap=SEGMENTS_K,
        graft_skip=None,
        donor_sfx=True,
        clip_overrides=KESTRAL_CLIPS, build_clips=kestral_clips,
    ),
    "kagan": dict(
        asset="br2_kagan_player",
        donor_skb=r"DATA\KAGAN.SKB", donor_bfm=r"MODELS\KAGAN.BFM",
        donor_jug=None, donor_sbs=r"MODELS\KAGAN.SBS",
        donor_for=kagan_donor, segmap=SEGMENTS_KG,
        graft_skip=None,
        attach_patch=[("rename", "sunbeam", "bladeL"),
                      ("rename", "sword", "bladeR"),
                      ("bone", "bladeR", 32),
                      ("transform", "bladeR", KAGAN_RIGHT_SWORD_GRIP)],
        donor_sfx=True,
        hits=translate_single_sword_hits, hit_names=("combo", "closeinattack", "jumpattack"),
        extra_hits=EXTRA_HITS_KG,
        clip_overrides=KAGAN_CLIPS, build_clips=kagan_clips,
    ),
    "ninjaf": dict(
        asset="br2_ninjaf_player",
        donor_skb=r"DATA\NINJAF.SKB", donor_bfm=r"MODELS\NINJAF.BFM",
        donor_jug=None, donor_sbs=r"MODELS\NINJAF.SBS",
        donor_for=ninja_donor, segmap=SEGMENTS_N,
        graft_skip=None,
        attach_patch=[("rename", "sword", "bladeR"),
                      ("append", "bladeL", "bladeR", 30)],
        donor_sfx=True,
        hits=translate_single_sword_hits, hit_names=("combo", "closein", "feedrepel"),
    ),
    "ninjam": dict(
        asset="br2_ninjam_player",
        donor_skb=r"DATA\NINJAM.SKB", donor_bfm=r"MODELS\NINJAM.BFM",
        donor_jug=None, donor_sbs=r"MODELS\NINJAM.SBS",
        donor_for=ninjam_donor, segmap=SEGMENTS_N,
        clip_overrides=NINJAM_CLIPS, build_clips=ninjam_clips,
        graft_skip=None,
        attach_patch=[("rename", "sword", "bladeR"),
                      ("append", "bladeL", "bladeR", 24)],
        donor_sfx=True,
        hits=translate_single_sword_hits, hit_names=("combo", "closein", "feedrepel"),
    ),
}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("character", choices=sorted(CHARS))
    p.add_argument("--game", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--audit", type=Path, help="write the slot -> clip mapping report here")
    args = p.parse_args()
    cfg = CHARS[args.character]
    asset = cfg["asset"]

    common = Pod3(args.game / "COMMON.POD")
    models = Pod3(args.game / "W32MODEL.POD")
    d_skb = common.read(common.by_name[cfg["donor_skb"]])
    r_skb = common.read(common.by_name[r"DATA\RAYNE.SKB"])
    clips = cfg["build_clips"](common) if "build_clips" in cfg else {}
    grafted, names = D.build_skeleton(d_skb, r_skb, skip=cfg["graft_skip"])
    report = []
    table, mapped = build_states(d_skb, r_skb, cfg["donor_for"], cfg["segmap"],
                                 report, donor_sfx=cfg["donor_sfx"],
                                 hits=cfg.get("hits", DM.translate_hits),
                                 hit_names=cfg.get("hit_names", ("combo", "evade", "railblade")),
                                 extra_hits=cfg.get("extra_hits"), clip_overrides=cfg.get("clip_overrides"),
                                 death_rate=cfg.get("death_rate", DEATH_RATE))
    skeleton = grafted[:8 + SKB_BONE * len(names)] + table
    checked = DM.check_table(skeleton, r_skb, common, clips)
    dbfm = Model.read(models.read(models.by_name[cfg["donor_bfm"]]), d_skb)
    rbfm = Model.read(models.read(models.by_name[r"MODELS\RAYNE.BFM"]), r_skb)
    model = D.build_model(dbfm, rbfm, names, asset)
    assert model[32:112].split(b"\0", 1)[0] == (asset + ".skl").encode(), \
        "BFM must reference its own skeleton"
    model = patch_bfm_attachments(model, cfg.get("attach_patch"))
    player_model = Model.read(model, skeleton)
    count = len(player_model.bones)
    children = unpack_from(f"<{count}i", model, player_model.rig + 40 * count)
    stock_children = unpack_from(f"<{len(dbfm.bones)}i", dbfm.data, dbfm.rig + 40 * len(dbfm.bones))
    if children[:len(stock_children)] != stock_children:
        raise ValueError("Donor stock rig changed")
    args.out.mkdir(parents=True, exist_ok=True)
    for clip, data in clips.items():
        path = args.out / "animations" / Path(clip)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    (args.out / (asset + ".skb")).write_bytes(skeleton)
    (args.out / (asset + ".bfm")).write_bytes(model)
    if cfg["donor_jug"]:
        jug = dampen_boss_jug(common.read(common.by_name[cfg["donor_jug"]]))
    else:
        jug = EMPTY_JUG
    (args.out / (asset + ".jug")).write_bytes(jug)
    if cfg["donor_sbs"]:
        (args.out / (asset + ".sbs")).write_bytes(common.read(common.by_name[cfg["donor_sbs"]]))
    if args.audit:
        args.audit.write_text("\n".join(report) + "\n", encoding="utf-8")
    print(f"{args.character} assets validated: {len(names)} bones, {len(checked)} states, {mapped} mapped")


if __name__ == "__main__":
    main()
