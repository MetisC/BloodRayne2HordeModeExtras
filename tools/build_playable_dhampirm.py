"""Build BR2_DHAMPIR_M_PLAYER - playable male dhampir for Challenge Mode.

Skeleton graft + BFM come from build_playable_dhampir.py (73 enemy bones plus
the 42 Rayne player bones the controller looks up by name).

The 478-slot move table follows the Ferril recipe: every Rayne player slot is
driven by the male dhampir's own clips (dhampir_m\\*.ani, an enemy rig with no
strafe/jump-forward/wall/pole/gun/bite player clips).  Rules:

- A slot with no suitable male clip stays unmapped: a short standready that
  returns to state 0, so the controller never plays a motion he does not have.
- Mapped slots keep Rayne's whole timeline (windows, script events, sound
  events) scaled onto the donor clip, so jumps, evades and combo windows fire
  the native logic at the same relative moments.  Locomotion/hurt/evade clips
  are time-warped to Rayne's slot duration, so reactions are as quick as hers.
- The male's attack clips are enemy multi-hit chains.  Each Rayne b/bb/bbb and
  k/kk/kkk step plays a truncated prefix of one of them, and his own damage
  ranges (l/r bladedamage1-3, foot damage) are translated to the native player
  ranges at the male's own strike frames, so hitstop, slow motion and
  dismemberment keep running through the native weapon/foot hit path.
- Hurt slots only use in-place clips (no tackle/knockdown flight).
- Death keeps Rayne's chain die1 (212) -> dead1 (213): the engine only counts the
  post-death timer, and so shows the game-over menu, while the state is 213.  He
  has no still "dead" clip, so dead1 replays the death clip at HOLD_RATE and stays
  on its last frame (next -1), which is the lying pose.
- Slot 46 (combo_circle_blade, reached by the Triangle special hook) plays his
  blade flurry; he cannot feed, so every bite/harpoon slot is unmapped.
"""

import argparse
import re
import sys
from pathlib import Path
from struct import pack, unpack_from

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "tools"))
import build_playable_ferril as F            # noqa: E402
import build_playable_dhampir as D           # noqa: E402
from build_ephemera import SKB_BONE, Model   # noqa: E402
from pod3 import Pod3                        # noqa: E402

ASSET = "br2_dhampir_m_player"
BS = chr(92)
fixed = F.fixed

ATTACK_RATE = 45.0
DEATH_RATE = 36.0
HOLD_RATE = 900.0
RATE_MIN, RATE_MAX = 24.0, 70.0
IDLE_CAP = 9
UNMAPPED_CAP = 12
HURT_CAP = 32
AIR_HOLD = {257, 258, 261, 262, 395, 431}
SPECIAL_READY = {0, 1, *range(7, 15), *range(17, 26), 385, 426, *range(421, 425), *range(448, 452)}
DAMAGE_RANGES = {"lbladedamage", "rbladedamage", "bbladedamage", "rfootdamage", "lfootdamage",
                 "fire_left_gun", "fire_right_gun"}
KEEP_NOGRAVITY = re.compile(r"^(jump|double_jump|fall|locked_evade|combo_(locked_)?(forward_)?j[bk]|dismount_rail)")
HURT_FLIGHT = {"check_hit_wall", "nogravity"}

# (male state, frame cap: None | int | "slot", rate mode: "warp" | "idle" | "attack")
SEGMENTS = {
    "B1": ("combo1", 26, "attack"), "B2": ("combo2", 18, "attack"), "B3": ("combo6", 62, "attack"),
    "K1": ("combo4", 19, "attack"), "K2": ("combo4", 27, "attack"), "K3": ("combo4", 80, "attack"),
    "SPECIAL_BLADE": ("combo5", 52, "attack"), "SPECIAL_KICK": ("combo4", 58, "attack"),
    "AIR_B": ("evadefrontblade", 24, "attack"), "AIR_K": ("evadefrontkick", None, "attack"),
    "GROUND_B": ("evadefrontblade", 30, "attack"), "GROUND_K": ("evadefrontkick", None, "attack"),
}
CHAIN = {("b", 1): "B1", ("b", 2): "B2", ("b", 3): "B3", ("k", 1): "K1", ("k", 2): "K2", ("k", 3): "K3"}

CLIP_MAP = {
    "stand_alert": ("standdefault", None, "idle"), "stand_ambient01": ("standambient1", None, "idle"),
    "locked_taunt": ("standreadyidle1", "slot", "idle"), "constraint": ("constraint", None, "idle"),
    "walk_forward": ("walkn", None, "warp"), "walk_forward_gun": ("walkn", None, "warp"),
    "walk_forward_start": ("walkn", "slot", "warp"), "run_start": ("runreadyn", "slot", "warp"),
    "run_forward": ("runreadyn", None, "warp"), "run_forward_left": ("runreadyn", None, "warp"),
    "run_forward_right": ("runreadyn", None, "warp"), "run_forward_gun": ("runreadyn", None, "warp"),
    "run_stop_left": ("standready", "slot", "idle"), "run_stop_right": ("standready", "slot", "idle"),
    "stand_quickturn": ("turnshuffle", "slot", "warp"), "move_quickturn": ("runreadyn", "slot", "warp"),
    "move_quickstop": ("standready", "slot", "idle"),
    "locked_idle": ("standready", None, "idle"),
    "locked_move_forward": ("walkreadyn", None, "warp"), "locked_move_backward": ("walkreadys", None, "warp"),
    "locked_walk_left": ("walkreadyw", None, "warp"), "locked_walk_right": ("walkreadye", None, "warp"),
    "locked_turn_left": ("turnshuffle", None, "warp"), "locked_turn_right": ("turnshuffle", None, "warp"),
    "locked_parry_hold": ("parryf", 2, "idle"), "locked_parry": ("parryf", None, "warp"),
    "die1": ("death", None, "death"), "dead1": ("death", None, "hold"),
    "phonemes": ("phonemes", "slot", "idle"), "expression_blink": ("phonemes", "slot", "idle"),
    "expression_sneer": ("phonemes", "slot", "idle"), "expression_stretch": ("phonemes", "slot", "idle"),
    "expression_breathout": ("phonemes", "slot", "idle"),
    "rage_blade": ("SPECIAL_BLADE",), "rage_kick": ("SPECIAL_KICK",),
    "bloodrage_start": ("standready", "slot", "idle"), "bloodrage_end": ("standready", "slot", "idle"),
    "bloodfury": ("standready", "slot", "idle"), "bloodfury_end": ("standready", "slot", "idle"),
    "bloodstorm_loop": ("standready", "slot", "idle"), "bloodstorm_end": ("standready", "slot", "idle"),
    "jump": ("jumpupstart", None, "warp"), "jump_forward": ("jumpupstart", None, "warp"),
    "double_jump": ("jumpupstart", None, "warp"), "locked_evade_jump": ("jumpupstart", None, "warp"),
    "jump_descent": ("jumpupfall", 2, "idle"), "jump_forward_descent": ("jumpupfall", 2, "idle"),
    "locked_evade_jump_descent": ("jumpupfall", 2, "idle"),
    "fall": ("jumpupfall", 2, "idle"), "fall_long": ("jumpupfall", 2, "idle"),
    "jump_land": ("jumpupland", None, "warp"), "jump_forward_land": ("jumpupland", None, "warp"),
    "locked_evade_jump_land": ("jumpupland", None, "warp"),
    "jumpforward_roll": ("jumpupland", None, "warp"), "jumpforward_hard": ("jumpupland", None, "warp"),
    "getup_face_down_loop": ("standready", "slot", "idle"), "getup_back_down_loop": ("standready", "slot", "idle"),
}
for _side, _name in (("left", "left"), ("right", "right"), ("front", "front"), ("back", "back")):
    CLIP_MAP[f"locked_evade_{_side}"] = (f"evade{_name}", None, "warp")
    CLIP_MAP[f"locked_evade_{_side}_b"] = (f"evade{_name}blade", None, "attack")
    CLIP_MAP[f"locked_evade_{_side}_k"] = (f"evade{_name}kick", None, "attack")
for _suffix in ("j", "jj", "jjj"):
    CLIP_MAP[f"locked_evade_front_{_suffix}"] = ("evadefrontkick", None, "attack")
    CLIP_MAP[f"locked_evade_back_{_suffix}"] = ("evadebackkick", None, "attack")
CLIP_MAP["locked_evade_front_twist"] = ("evadefront", None, "warp")
for _idle in ("stand_alert_to_locked_idle", "locked_idle_to_stand_alert", "locked_idle_m_to_stand_alert"):
    CLIP_MAP[_idle] = ("standready", "slot", "idle")

RAIL_MAP = {
    "rail_slide": ("railslide", None, "warp"), "rail_slide_off": ("rail_slide_off", None, "warp"),
    "rail_slide_down": ("railslidedown", None, "warp"), "rail_slide_up": ("railslideup", None, "warp"),
    "rail_blade": ("railblade", None, "attack"),
    "rail_blade_impact_left": ("railblade", "slot", "attack"), "rail_blade_impact_right": ("railblade", "slot", "attack"),
    "rail_stand": ("railstand", None, "idle"), "rail_stand_turn_l": ("railstandturn", None, "warp"),
    "rail_stand_turn_r": ("railstandturn", None, "warp"), "rail_idle": ("railstand", 2, "idle"),
    "rail_walk": ("railrun10", None, "warp"), "rail_run_10": ("railrun10", None, "warp"),
    "rail_run_20": ("railrun20", None, "warp"), "rail_run_30": ("railrun30", None, "warp"),
    "rail_run": ("railrun45", None, "warp"),
    "dismount_rail_left": ("dismountrailleft", None, "warp"), "dismount_rail_left_fall": ("dismountrailleftfall", None, "idle"),
    "dismount_rail_left_land": ("dismountrailleftland", None, "warp"),
    "dismount_rail_right": ("dismountrailright", None, "warp"), "dismount_rail_right_fall": ("dismountrailrightfall", None, "idle"),
    "dismount_rail_right_land": ("dismountrailrightland", None, "warp"),
    "dismount_rail_center": ("rail_slide_off", None, "warp"), "dismount_rail_to_vpole": ("rail_slide_off", None, "warp"),
    "dismount_rail_to_hpole": ("rail_slide_off", None, "warp"),
}


def hurt_side(name):
    return next((s for s in ("left", "right", "back", "front") if s in name), "front")


def donor_for(index, slot):
    """Return (male state | SEGMENTS key, cap, rate mode) or None for an unmapped slot."""
    name = slot.name
    clip = slot.clip.replace("/", BS).split(BS)[-1].rsplit(".", 1)[0].lower()
    if clip == "home":
        return None
    if index == 46:
        return SEGMENTS["SPECIAL_BLADE"]
    if index == 47:
        return SEGMENTS["SPECIAL_KICK"]
    if index == 48:
        return None
    if name.startswith("combo_") and "-idle" in name:
        return ("standready", "idle", "idle")
    if "groundstrike" in name:
        return SEGMENTS["GROUND_K"] if "_k_" in name else SEGMENTS["GROUND_B"]
    if index == 66:
        return SEGMENTS["GROUND_K"]
    if re.search(r"(?:^|_)j[bk]", name):
        if name.endswith("_hold"):
            return ("jumpupfall", 2, "idle")
        if name.endswith(("_end", "_land")):
            return ("jumpupland", "slot", "warp")
        return SEGMENTS["AIR_K"] if "jk" in name else SEGMENTS["AIR_B"]
    chain = re.search(r"_([bk]{1,3})(?:_m(?:irror)?)?$", name)
    if name.startswith("combo_") and chain:
        return SEGMENTS[CHAIN[(chain[1][0], len(chain[1]))]]
    if name.startswith("hurt_"):
        side = hurt_side(name)
        if "tackle_" in name:
            if re.search(r"(recov|wall|abort)", name):
                return ("standready", "slot", "idle")
            return (f"hurt{side}", 7 if "impact" in name else HURT_CAP, "warp")
        return (f"hurtlight{side}" if "fast" in name else f"hurt{side}", None, "warp")
    if name.startswith("recovery_onback") or name.startswith("recovery_onstomach"):
        return ("standready", "slot", "idle")
    if name in RAIL_MAP:
        return RAIL_MAP[name]
    return CLIP_MAP.get(clip)


def read_player_slots(skb):
    """Rayne's player table including the sample list and script events of every slot."""
    bones, states = F.read_skb(skb)
    at = 8 + bones * 72
    count, = unpack_from("<I", skb, at)
    at += 4 + count * 16 + 4
    extra = []
    for _ in states:
        at += 118
        n, = unpack_from("<I", skb, at)
        at += 4
        samples = [skb[at + i * 64:at + (i + 1) * 64] for i in range(n)]
        at += n * 64
        n, = unpack_from("<I", skb, at)
        at += 4
        events = [unpack_from("<HHI", skb, at + i * 8) for i in range(n)]
        at += n * 8
        for size in (4, 12):
            n, = unpack_from("<I", skb, at)
            at += 4 + n * size
        extra.append((samples, events))
    return extra


def translate_hits(donor, used):
    """The male's l/r blade/foot damage ranges as native player ranges, within the first `used` frames."""
    hits = []
    for name, start, end in donor.windows:
        match = re.match(r"([lr])(blade|foot)damage\d$", name)
        if not match or start > used - 4:
            continue
        hits.append([f"{match[1]}{match[2]}damage", float(start), float(min(end, used - 1))])
    merged = []
    for hit in sorted(hits, key=lambda h: (h[1], h[0])):
        pair = next((m for m in merged if m[0].endswith("bladedamage") and hit[0].endswith("bladedamage")
                     and m[0] != hit[0] and abs(m[1] - hit[1]) <= 3 and m[0] != "bbladedamage"), None)
        if pair:
            pair[0], pair[1], pair[2] = "bbladedamage", min(pair[1], hit[1]), max(pair[2], hit[2])
        else:
            merged.append(hit)
    return [tuple(m) for m in merged]


def strike_id_events(windows):
    """Use the native move events to rearm each distinct blade or kick strike."""
    ends = {}
    blades = {}
    events = []
    limbs = {"lbladedamage": ("left",), "rbladedamage": ("right",),
             "bbladedamage": ("left", "right"),
             "lfootdamage": ("left foot",), "rfootdamage": ("right foot",)}
    for name, start, end in sorted(windows, key=lambda window: window[1]):
        for limb in limbs.get(name, ()):
            if start > ends.get(limb, -1):
                frame = int(start)
                if limb in ("left", "right"):
                    blades.setdefault(frame, set()).add(limb)
                else:
                    events.append((frame, 0, 1204 if limb == "left foot" else 1205))
            ends[limb] = max(end, ends.get(limb, -1))
    for frame, sides in blades.items():
        event = 1315 if len(sides) == 2 else 1301 if "left" in sides else 1302
        events.append((frame, 0, event))
    return events


def build_states(male_skb, rayne_skb, audit=None):
    _, ranges, player, rayne_rates = F.layout(rayne_skb)
    _, _, male_states, male_rates, _ = F.motion_layout(male_skb)
    male = {s.name: (s, r) for s, r in zip(male_states, male_rates)}
    extras = read_player_slots(rayne_skb)
    out = bytearray(pack("<I", len(ranges)))
    out.extend(b"".join(fixed(n, 16) for n in ranges))
    out.extend(pack("<I", len(player)))
    mapped_count = 0
    for index, slot in enumerate(player):
        entry = donor_for(index, slot)
        if entry and len(entry) == 1:
            entry = SEGMENTS[entry[0]]
        mapped = entry is not None
        if mapped:
            mapped_count += 1
            state_name, cap, mode = entry
            donor, donor_rate = male[state_name]
            used = donor.frames
            if cap == "slot" or cap == "idle":
                used = min(used, IDLE_CAP if cap == "idle" else slot.frames)
            elif cap:
                used = min(used, cap)
            if index in AIR_HOLD:
                used = 2
            if mode == "attack":
                rate = ATTACK_RATE
            elif mode == "death":
                rate = DEATH_RATE
            elif mode == "hold":
                rate = HOLD_RATE
            elif mode == "idle":
                rate = donor_rate
            else:
                rate = min(RATE_MAX, max(RATE_MIN, used * rayne_rates[index] / max(1, slot.frames)))
        else:
            donor, donor_rate = male["standready"]
            used, rate, mode = min(slot.frames, UNMAPPED_CAP), 30.0, "idle"
        next_state = slot.next_state
        if not mapped and next_state < 0 and slot.name != "constraint":
            next_state = 0
        if 163 <= index <= 198:
            next_state = 0
        if index == 426:
            next_state = -2

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
            if donor.name.startswith(("combo", "evade", "railblade")):
                windows.extend(translate_hits(donor, used))
            if donor.name.startswith(("dismountrail",)):
                windows.extend((n, s, min(e, used - 1)) for n, s, e in donor.windows if n == "nogravity")
        if index in SPECIAL_READY:
            windows.append(("allowharpoon", 0.0, float(used - 1)))

        samples, signals = [], strike_id_events(windows)
        custom_strikes = bool(signals)
        if mapped:
            rayne_samples, rayne_events = extras[index]
            for frame, kind, ident in rayne_events:
                at = min(used - 1, round(frame * scale))
                if kind == 1:
                    signals.append((at, 1, len(samples)))
                    samples.append(rayne_samples[ident])
                elif not (custom_strikes and kind == 0 and ident in (1204, 1205, 1301, 1302, 1315)):
                    signals.append((at, kind, ident))
        out.extend(fixed(slot.name, 30) + fixed(donor.clip, 64))
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
                         + (f"{donor.name:18} frames={used:3} rate={rate:5.1f} next={next_state:3} "
                            f"win={[(n, round(s), round(e)) for n, s, e in windows if n in DAMAGE_RANGES or n in ('nogravity', 'allowharpoon')]}"
                            if mapped else f"(unmapped) standready frames={used}"))
    return bytes(out), mapped_count


def check_table(skeleton, rayne_skb, common, loose_clips=()):
    bones, checked = F.read_skb(skeleton)
    _, ranges, player, _ = F.layout(rayne_skb)
    assert [s.name for s in checked] == [s.name for s in player], "slot names changed"
    for state in checked:
        assert 1 <= state.frames, state.name
        assert state.next_state < len(checked), state.name
        for name, start, end in state.windows:
            assert 0 <= start <= end <= state.frames - 1 + 1e-3, (state.name, name, start, end, state.frames)
        clip = state.clip.replace("/", BS).upper()
        assert state.clip in loose_clips or common.by_name.get("ANIMATIONS" + BS + clip) is not None, \
            (state.name, state.clip)
    return checked


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--game", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--audit", type=Path, help="write the slot -> clip mapping report here")
    args = p.parse_args()
    common = Pod3(args.game / "COMMON.POD")
    models = Pod3(args.game / "W32MODEL.POD")
    d_skb = common.read(common.by_name["DATA" + BS + "DHAMPIR_M.SKB"])
    r_skb = common.read(common.by_name["DATA" + BS + "RAYNE.SKB"])
    grafted, names = D.build_skeleton(d_skb, r_skb)
    report = []
    table, mapped = build_states(d_skb, r_skb, report)
    skeleton = grafted[:8 + SKB_BONE * len(names)] + table
    checked = check_table(skeleton, r_skb, common)
    dbfm = Model.read(models.read(models.by_name["MODELS" + BS + "DHAMPIR.BFM"]), d_skb)
    rbfm = Model.read(models.read(models.by_name["MODELS" + BS + "RAYNE.BFM"]), r_skb)
    model = D.build_model(dbfm, rbfm, names, ASSET)
    assert model[32:112].split(b"\0", 1)[0] == (ASSET + ".skl").encode(), "BFM must reference its own skeleton"
    player_model = Model.read(model, skeleton)
    count = len(player_model.bones)
    children = unpack_from(f"<{count}i", model, player_model.rig + 40 * count)
    stock_children = unpack_from(f"<{len(dbfm.bones)}i", dbfm.data, dbfm.rig + 40 * len(dbfm.bones))
    if children[:len(stock_children)] != stock_children:
        raise ValueError("Dhampir's stock rig changed")
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / (ASSET + ".skb")).write_bytes(skeleton)
    (args.out / (ASSET + ".bfm")).write_bytes(model)
    jug = common.read(common.by_name["MODELS" + BS + "DHAMPIR.JUG"])
    (args.out / (ASSET + ".jug")).write_bytes(dampen_male_cape(jug))
    if args.audit:
        args.audit.write_text("\n".join(report) + "\n", encoding="utf-8")
    print(f"Dhampir-M assets validated: {len(names)} bones, {len(checked)} states, {mapped} mapped")


def dampen_male_cape(physics):
    """dampen_cape() for any cape-bone count (male 20, female 25): restoring spring
    on free ends, 0.88 velocity retention; pinned (0) tips stay pinned."""
    lines = physics.splitlines()
    for i in range(5, 5 + int(lines[3]) * 2, 2):
        params = lines[i + 1].split(b",")
        if not lines[i].startswith(b"cape_") or len(params) != 17:
            raise ValueError("Unexpected cape joint " + lines[i].decode())
        params[1] = b"0.88"
        lines[i + 1] = b",".join(params)
    return b"\r\n".join(lines) + b"\r\n"


if __name__ == "__main__":
    main()
