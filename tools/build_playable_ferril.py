#!/usr/bin/env python3
"""Build the challenge character from the stock Ferril rig and clips."""

import argparse
from dataclasses import replace
from pathlib import Path
import re
from struct import pack, pack_into, unpack_from

from build_ephemera import ATTACHMENT, Model, Pod3, name_at
from audit_playable_ferril import read_skb


ASSET = "br2_ferril_player"

MOVEMENT = {
    0: "idle", 1: "stand_ambient_1", 5: "gesture_a", 6: "standready",
    7: "walk_n", 8: "walk_n", 9: "walk_n", 10: "run_n", 11: "run_n",
    12: "run_n", 13: "run_n", 14: "run_n", 15: "idle", 16: "idle",
    17: "turn_left", 18: "turn_right", 19: "idle", 20: "standready",
    21: "walk_n", 22: "walk_s", 23: "standready", 24: "walk_n", 25: "walk_s",
    121: "bullet_block", 122: "block_harpoon", 153: "bullet_block", 154: "block_harpoon",
    199: "idle", 200: "idle", 212: "die", 213: "dead",
    255: "jump_25long_10high", 256: "jump_35long_16high", 257: "jump_25long_10high",
    258: "jump_25long_10high", 259: "standready", 260: "standready", 261: "jump_25long_10high",
    262: "jump_25long_10high", 263: "roll_left", 264: "jump_45long_16high",
    265: "jump_25long_10high", 266: "rail_slide_loop", 267: "wall_jump_off",
    268: "rail_slide_loop", 269: "rail_slide_loop", 270: "rail_swipe_right",
    271: "rail_swipe_left", 272: "rail_swipe_right", 273: "rail_slide_loop",
    274: "turn_left", 275: "turn_right", 276: "rail_slide_loop", 277: "rail_slide_loop",
    278: "rail_slide_loop", 279: "rail_slide_loop", 280: "rail_slide_loop", 281: "rail_slide_loop",
    282: "rail_to_rail_left", 285: "rail_to_rail_right",
    384: "tocombatidle", 385: "standready", 386: "tocombatidle", 387: "tocombatidle",
    388: "tocombatidle", 389: "tocombatidle", 390: "tocombatidle", 391: "tocombatidle",
    392: "turn_left", 393: "turn_right", 394: "jump_25long_10high", 395: "jump_25long_10high",
    396: "standready", 397: "roll_left", 398: "roll_left_attack", 399: "roll_left_attack",
    400: "roll_right", 401: "roll_right_attack", 402: "roll_right_attack",
    403: "jump_35long_16high", 404: "leap_attack_claw", 405: "leap_attack_kick",
    406: "jump_35long_16high", 407: "jump_35long_16high", 408: "jump_35long_16high", 409: "jump_35long_16high",
    410: "evade_back", 411: "evade_back", 412: "evade_back", 413: "evade_back",
    414: "claw_uber", 415: "kick_left", 421: "walk_w", 422: "walk_e",
    423: "walk_n", 424: "walk_s", 425: "tocombatidle", 426: "standready",
    427: "tocombatidle", 428: "turn_left", 429: "turn_right", 430: "jump_25long_10high",
    431: "jump_25long_10high", 432: "standready", 433: "roll_left", 434: "roll_left_attack",
    435: "roll_left_attack", 436: "roll_right", 437: "roll_right_attack", 438: "roll_right_attack",
    439: "jump_35long_16high", 440: "jump_35long_16high", 441: "jump_35long_16high", 442: "jump_35long_16high",
    443: "jump_35long_16high", 444: "evade_back", 445: "evade_back", 446: "evade_back",
    447: "evade_back", 448: "walk_w", 449: "walk_e", 450: "walk_n", 451: "walk_s",
    453: "corkscrew_combo", 454: "leap_attack_kick", 455: "phonemes", 456: "phonemes",
    457: "phonemes", 458: "phonemes", 465: "phonemes", 466: "jump_35long_16high",
    467: "tocombatidle", 468: "standready", 469: "gesture_b", 470: "standready",
    471: "standready", 472: "standready",
}
for start, names in (
    (155, ("hurt_soft_front", "hurt_soft_rear", "hurt_soft_left", "hurt_soft_right")),
    (159, ("hurt_strong_front", "hurt_strong_back", "hurt_strong_left", "hurt_strong_right")),
    (163, ("hurt_tackle_front", "hurt_tackle_back", "hurt_tackle_right", "hurt_tackle_left")),
):
    MOVEMENT.update(enumerate(names, start))
for index in range(201, 211):
    MOVEMENT[index] = "tocombatidle"

NATIVE_HIT_RANGES = ("lclaw2", "lclaw3", "rclaw1", "rclaw2", "rclaw3", "lkick3")
AIR_HOLD_STATES = {257, 258, 261, 262, 395, 431}
JUMPS = {255: "jump_up", 256: "jump_foward", 265: "jump_up", 394: "jump_up", 430: "jump_up"}
JUMP_TIMING = {"jump_up": (7, 18, 31), "jump_foward": (5, 14, 28)}
ATTACK_RATE = {
    "claw_left": 50, "claw_right": 50, "claw_uber": 46, "corkscrew_combo": 44,
    "kick_left": 48, "leap_attack_claw": 43, "leap_attack_kick": 46,
    "roll_left_attack": 42, "roll_right_attack": 42,
}
FINISHER_SLOTS = {40, 45, 47, 81, 86, 91, 111, 116, 119, 120, 127, 147, 152, 454}
SPECIAL_READY = {0, 1, *range(7, 15), *range(17, 26), 385, 426,
                 *range(421, 425), *range(448, 452)}
PLAYER_RANGES = {"allowturning", "combocontinue", "ragecontinue", "nogravity", "nosliding",
                 "check_hit_wall", "allow_recovery", "onground", "acro_trans", "check_release",
                 "check_pole_turn", "jumpTwistLand", "continueevade"}


def donor_for(index, state):
    if index in JUMPS:
        return JUMPS[index]
    if index in MOVEMENT:
        return MOVEMENT[index]
    if "-idle" in state.name and state.name.startswith("combo_"):
        return "standready"
    if "groundstrike" in state.name or index == 66:
        return "kick_left" if "_k_" in state.name or index == 66 else "claw_uber"
    if index in (46, 47):
        return "corkscrew_combo" if index == 46 else "kick_left"
    if re.search(r"(?:^|_)j[bk]", state.name):
        return "leap_attack_kick" if "jk" in state.name else "leap_attack_claw"
    if "_hold" in state.name and "combo_" in state.name:
        return "jump_25long_10high"
    attack = re.search(r"_([bk]{1,3})(?:_m(?:irror)?)?$", state.name)
    if state.name.startswith("combo_") and attack:
        chain = attack[1]
        if chain.startswith("k"):
            return "kick_left" if len(chain) < 3 else "leap_attack_kick"
        if len(chain) == 3:
            return "corkscrew_combo"
        if "_up_" in state.name or "_back_" in state.name:
            return "claw_uber" if len(chain) == 2 else "claw_left"
        return "claw_left" if len(chain) == 1 else "claw_right"
    if state.name.startswith("hurt_"):
        side = next((side for side in ("left", "right", "back", "front") if side in state.name), "front")
        return "hurt_tackle_" + side
    return None


def layout(data):
    bones, names, states, rates, _ = motion_layout(data)
    return bones, names, states, rates


def motion_layout(data):
    bones, states = read_skb(data)
    names_at = 8 + bones * 72
    count, = unpack_from("<I", data, names_at)
    names_at += 4
    names = [name_at(data, names_at + i * 16, 16) for i in range(count)]
    cursor = names_at + count * 16 + 4
    fps = []
    sounds = []
    for _ in states:
        fps.append(unpack_from("<f", data, cursor + 94)[0])
        cursor += 118
        count, = unpack_from("<I", data, cursor)
        cursor += 4
        samples = [data[cursor + i * 64:cursor + (i + 1) * 64] for i in range(count)]
        cursor += count * 64
        count, = unpack_from("<I", data, cursor)
        cursor += 4
        events = [unpack_from("<HHI", data, cursor + i * 8) for i in range(count)]
        cursor += count * 8
        sounds.append([(frame, samples[index]) for frame, kind, index in events
                       if kind == 1 and samples[index].lower().startswith(b"ferril_")])
        for size in (4, 12):
            count, = unpack_from("<I", data, cursor)
            cursor += 4 + count * size
    return data[:names_at - 4], names, states, fps, sounds


def fixed(value, size):
    encoded = value.encode("ascii")
    if len(encoded) >= size:
        raise ValueError(f"Field too long: {value}")
    return encoded.ljust(size, b"\0")


def build_skeleton(ferril, rayne, common):
    bones, _, original, rates, sounds = motion_layout(ferril)
    _, player_ranges, player, _ = layout(rayne)
    donors = {state.name: (state, rate, voice) for state, rate, voice in zip(original, rates, sounds)}
    jump, _, jump_voice = donors["jump_25long_10high"]
    for name, (takeoff, _, _) in JUMP_TIMING.items():
        clip = fr"Ferril\{name}.ani"
        data = common.read(common.by_name[fr"ANIMATIONS\{clip.upper()}"])
        version, frames = unpack_from("<II", data)
        if version != 3 or frames <= JUMP_TIMING[name][2] + 1:
            raise ValueError(f"Unexpected jump animation {clip}")
        donors[name] = (replace(jump, name=name, clip=clip, frames=frames, windows=()),
                        45.0, [(takeoff, sample) for _, sample in jump_voice])
    ranges = player_ranges + list(NATIVE_HIT_RANGES) + ["ferrilfinish"]
    if len(ranges) > 32 or len(player) != 478:
        raise ValueError("Unexpected player animation table")
    result = bytearray(bones + pack("<I", len(ranges)))
    result.extend(b"".join(fixed(name, 16) for name in ranges))
    result.extend(pack("<I", len(player)))

    for index, slot in enumerate(player):
        mapped = donor_for(index, slot)
        donor, rate, voice = donors[mapped or "standready"]
        rate = ATTACK_RATE.get(mapped, rate)
        if mapped == "run_n":
            rate *= 1.16
        if (mapped and mapped.startswith("hurt_tackle_")) or 201 <= index <= 210:
            rate *= 1.2
        short_recovery = "-idle" in slot.name or index in (8, 10, 15, 16, 19, 259, 260, 396, 432)
        frames = min(slot.frames, donor.frames) if short_recovery else donor.frames
        if "-idle" in slot.name and slot.name.startswith("combo_"):
            frames = min(frames, 9)
        if index in AIR_HOLD_STATES:
            frames = 2
        next_state = slot.next_state
        if not mapped and next_state < 0 and slot.name != "constraint":
            next_state = 0
        if 163 <= index <= 198:
            next_state = 0
        if index == 426:
            next_state = -2

        windows = []
        signals = []
        for name, start, end in slot.windows:
            if mapped and name in PLAYER_RANGES and name != "nogravity":
                scale = (frames - 1) / max(1, slot.frames - 1)
                if name == "combocontinue" and "-idle" in slot.name:
                    windows.append((name, 0.0, float(frames - 1)))
                else:
                    windows.append((name, start * scale, end * scale))
        for name, start, end in donor.windows:
            if mapped and name in NATIVE_HIT_RANGES:
                windows.append((name, start, end))
            if name == "ignoregravity" and index not in AIR_HOLD_STATES:
                windows.append(("nogravity", start, end))
        if index in SPECIAL_READY:
            # Let the native Feed/Harpoon input reach Ferril's ground special.
            windows.append(("allowharpoon", 0.0, float(frames - 1)))
        if index in JUMPS:
            takeoff, queue, descent = JUMP_TIMING[mapped]
            windows.append(("nogravity", 0.0, float(descent)))
            # Player motion events: takeoff, optional buffered attack, release to fall.
            signals.extend(((takeoff, 0, 1708), (queue, 0, 1201), (descent, 0, 1001)))
        if mapped == "corkscrew_combo" or index in FINISHER_SLOTS:
            strikes = [(start, end) for name, start, end in windows if name in NATIVE_HIT_RANGES]
            if strikes:
                windows.append(("ferrilfinish", *max(strikes)))

        samples = []
        for frame, sample in voice:
            if frame < frames:
                signals.append((frame, 1, len(samples)))
                samples.append(sample)

        result.extend(fixed(slot.name, 30) + fixed(donor.clip, 64))
        result.extend(pack("<fIfifII", rate, frames, 0.10, next_state, float(frames - 1), 0, len(samples)))
        result.extend(b"".join(samples))
        result.extend(pack("<I", len(signals)))
        for signal in sorted(signals):
            result.extend(pack("<HHI", *signal))
        result.extend(pack("<II", 0, len(windows)))
        for name, start, end in windows:
            result.extend(pack("<Iff", ranges.index(name), start, end))

    _, checked = read_skb(result)
    if [state.name for state in checked] != [state.name for state in player]:
        raise ValueError("Player slot names changed")
    return bytes(result)


def build_model(ferril, rayne):
    header_and_parts = bytearray(ferril.data[:ferril.attachments])
    records = bytearray(rayne.data[rayne.attachments:rayne.attachments + 4 * ATTACHMENT])
    for index in range(4):
        offset = index * ATTACHMENT + 24
        bone, = unpack_from("<i", records, offset)
        name = rayne.bones[bone].casefold()
        pack_into("<i", records, offset, next(i for i, n in enumerate(ferril.bones) if n.casefold() == name))
    pack_into("<I", header_and_parts, 20, 4)
    header_and_parts[32:112] = fixed(ASSET + ".skl", 80)
    result = header_and_parts + records + ferril.data[ferril.attachments:ferril.geometry]
    result.extend(bytes(-len(result) % 16))
    result.extend(ferril.data[ferril.geometry:])
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--game", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    common = Pod3(args.game / "COMMON.POD")
    models = Pod3(args.game / "W32MODEL.POD")
    ferril = common.read(common.by_name[r"DATA\FERRIL.SKB"])
    rayne = common.read(common.by_name[r"DATA\RAYNE.SKB"])
    skeleton = build_skeleton(ferril, rayne, common)
    model = build_model(Model.read(models.read(models.by_name[r"MODELS\FERRIL.BFM"]), ferril),
                        Model.read(models.read(models.by_name[r"MODELS\RAYNE.BFM"]), rayne))
    Model.read(model, skeleton)
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / (ASSET + ".SKB")).write_bytes(skeleton)
    (args.out / (ASSET + ".BFM")).write_bytes(model)
    print("Ferril player assets validated and written to", args.out)


if __name__ == "__main__":
    main()
