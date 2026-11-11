# BloodRayne2HordeModeExtras

Independent `dinput8.dll` proxy mod for **BloodRayne 2: Terminal Cut** (GOG).

No third-party proxy required: the DLL forwards `DirectInput8Create` to the
system `dinput8.dll` and coexists with other proxies (e.g. `d3d8.dll` /
Gunsucked).

## What it adds

- **Challenge Mode** entry on the main menu:
  - Nine arena types in campaign order: **Mansion Battle**, **Mansion Fireplace**,
    **Mansion Rooftop**, **Meatpacking Bar**, **Meatpacking Arena**,
    **Shroud Tower Roof**, **Insect Plaza**, **Woodchipper**, and **Horde Mode**
  - **Enemies** cycler — 15 / 25 / 50 / 100 / 250
  - **Guns** toggle — Yes/No, persisted in the ini
  - **Health and Rage** toggle — Base (1,000 each) or Maximum (2,000 each),
    persisted in the ini. The arena starts with full health and empty rage
    on every load or restart. Capacity stays fixed and Carnage rank-ups
    give no refills in these arenas.
  - **Best time** per arena, count, guns setting, difficulty and capacity,
    shown on the launch row and stored in `br2mod_times.ini`
  - Localized `!New Record!` banner when a run beats the stored time
- Global **Difficulty** under Options: `Minion | Dhampir | Overlord |
  Unraveller | Custom` — real gameplay scaling; `Custom` reads live
  multipliers from `br2mod.ini`
- Global **Combat** under Options offers Normal and Enhanced. New installs
  default to Enhanced with hitstop on. Enhanced adds
  parry on Lock-On, hitstop, recovery protection, a hit counter and a style
  grade. It also adds effects for combo endings and parry follow-ups, combo
  continuation after a directional evade, and a brief camera and time pulse
  at the impact of feeding fatalities that use blades, kicks or throws.
  The feeding effect is disabled during the native time powers.
- Edit `br2mod_combat.ini` while the game is open. Its `[Enhanced]` settings
  are reloaded automatically and include a switch for each mechanic.
  Normal uses the original combat rules. Challenge time records have
  separate Normal and Enhanced profiles.
- **Enhanced Gun Combat** under Combat Effects increases blood ammunition
  and enables automatic pickup. Rayne can carry two of each conventional
  type. A new copy fills a free hand, right first, or goes into the inventory.
  Further copies refill equipped weapons first, then stored copies, and
  disappear. The arena must have Guns enabled in Challenge Mode.

| Action | Keyboard | Xbox | DualSense |
| --- | --- | --- | --- |
| Open the wheel | Hold F7 | Hold RT | Hold R2 |
| Highlight an entry | Left / Right | Right stick or D-pad left / right | Right stick or D-pad left / right |
| Equip | F9 | Y | Triangle |
| Stow | F6 | X | Square |
| Drop | F8 | Click right stick | R3 |

The wheel lists all conventional types and the six blood gun modes. Grey
entries are unavailable. The number is the inventory count. Amber marks the
right hand and blue marks the left. Releasing the wheel button closes it.
Equipping a new type with both hands occupied replaces the right weapon.
Equipping a second copy replaces the weapon in the opposite hand. Stow and
drop act on the left copy first when both hands carry the same type.

Blood guns provide two copies after their native unlock. Both use the same
mode and share a reserve. Each shot spends half the cost of a pair. The
HUD counts paired shots with both equipped and individual shots with one.
Blood guns can be stowed. Conventional weapons can be dropped and picked up
again after stepping away. An empty discard despawns after five gameplay
seconds by default. Weapons are local to the current level.

Fire uses the original binding. If the controller had fire on RT/R2, the
arsenal uses the former cycle-weapons button for fire while enabled.
Arsenal tuning lives in `br2mod_combat.ini`.

- Global **Appearance/Skin** under Options: `Default` keeps the native /
  Gunsucked outfit behaviour; any other entry forces that skin everywhere
- In-challenge HUD: localized kill counter `Kills: N/M` and a localized
  `Challenge Completed` + run time on completion; the game clock is the
  scaled gameplay delta, so slow motion counts as world time
- All mod strings localized for the six game languages
- Native campaign, Extras content (museum, dev/testing levels) and door
  behaviour are untouched — every challenge hook is gated to the mod levels
- Stability fix: guards a native engine race where a tracked target actor is
  destroyed mid-frame (the stock "Suicide Run" arena crash)

## Requirements

| Tool | Why |
|---|---|
| BloodRayne 2: Terminal Cut | Target game (32-bit `rayne2.exe`, 2022 build) |
| MSVC x86 toolchain | Builds `dinput8.dll` (`build.bat` calls `vcvarsall x86`; default path assumes VS 2022 Community — edit `VCVARS` for other editions) |
| Python 3 (stdlib only) | `tools/pod3.py`, `tools/make_modes.py`, `tools/make_campaign_arenas.py`, `tools/build_msglist.py` |

## Environment setup

### 1. Build the proxy

Run `build.bat` inside an *x86 Native Tools* prompt (or anywhere — it
invokes `vcvarsall x86` itself). Produces `dinput8.dll`.

### 2. Extract the game PODs

`tools/pod3.py` reads the Terminal Reality POD3 archives:

```
python tools/pod3.py extract <game>\COMMON.POD    extracted\common
python tools/pod3.py extract <game>\LANGUAGE.POD  extracted\language
```

### 3. Generate `world\`

```
python tools/make_modes.py  --src extracted\common\WORLD   --dst <game>\world
python tools/make_campaign_arenas.py --src extracted\common\WORLD --dst <game>\world
python tools/build_msglist.py --src extracted\language\WORLD --dst <game>\world
```

`make_modes.py` copies the native arena levels and applies the mod patches:
`maxNumberSpawn` per enemy count, injected multiguns for the Guns variants,
`enableGunMode` + `warpTo` script edits, blanked `display( )` banners, and
script/root-name retargeting (`a1_mansion_part1_battle_*` keeps the native
party-dress rule; `chb_hor_*` for horde). `build_msglist.py` writes an
augmented `world\<LANG>\msglist.txt` per language with the mod's strings.
`make_campaign_arenas.py` builds five-count, guns-on/off copies of seven story
rooms. It replaces the story SCBs, disables story triggers and static hostiles,
closes doors, adds player barriers, and keeps the fireplace and bar-fan hazards.

### 4. Deploy

Into the game directory (next to `rayne2.exe`):

- `dinput8.dll` — built in step 1 (close `rayne2.exe` first)
- `br2mod.ini` — configuration
- `world\` — generated in step 3

Original game files are never modified: the proxy swaps the file handlers
so loose files take priority over the PODs, only when the game is still in
its default POD-first order.

## Configuration (`br2mod.ini`)

```ini
[Difficulty]
preset=Dhampir        ; Minion | Dhampir | Overlord | Unraveller | Custom

[Custom]
dmgIn=0.1             ; damage Rayne receives   (1.0 = normal)
dmgOut=10.0           ; damage Rayne deals      (1.0 = normal)
cost=0.5              ; rage cost of powers     (1.0 = normal)

[Appearance]
skin=default          ; "default" = native outfit system untouched

[Challenge]
count=25              ; 15 | 25 | 50 | 100 | 250
guns=0                ; 0 = No | 1 = Yes
maxBars=0             ; 0 = Base | 1 = Maximum
```

`Custom` multipliers are read from the ini each time the preset is
applied — select `Custom` in Options > Difficulty (or re-select it after
editing the ini). Chosen menu values are written back so they persist.

`br2mod_times.ini` stores new records under
`[Times_v2_<Difficulty>_BarsBase]` and `[Times_v2_<Difficulty>_BarsMax]`.
Enhanced combat uses the same sections with an `_Enhanced` suffix.
The previous sections are kept as an archive and excluded from comparisons.
