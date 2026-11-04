# BloodRayne2HordeModeExtras

Independent `dinput8.dll` proxy mod for **BloodRayne 2: Terminal Cut** (GOG).

No third-party proxy required: the DLL forwards `DirectInput8Create` to the
system `dinput8.dll` and coexists with other proxies (e.g. `binkw32.dll`).

## What it adds

- **Challenge Mode** entry on the main menu, before Extras:
  - **Arena** cycler — story levels retargeted as wave arenas:
    Mansion Battle, Mansion - Ballroom, Mansion - Roof, Meatpacking,
    Union Station - Abandoned Shopping Plaza,
    Wetworks - Chemical Processing, Kagan's Tower - Skybridge
  - **Enemies** cycler — 15 / 25 / 50 / 100 per arena
  - **Start Challenge** launches the selected combo; the menu shows the
    best time stored for it
  - Fixed **Boss:** rows at the end (Zerenski, Slezz, Ephemera,
    Unraveller) — the native story fights, no enemy cycling; killing the
    boss returns to the challenge menu
- **Extra Mode** entries inside the native Extras menu (story mode, museum,
  dev/testing levels).
- Global **Difficulty** option under Options — real gameplay scaling, not
  just labels: `Minion | Dhampir | Overlord | Unraveller | Custom`.
- In-challenge HUD: localized kill counter `Kills: N/M` plus a localized
  "Challenge Completed" message when the wave objective is met.
- All menu labels and HUD text are localized in the six game languages.

Campaign content and native door behavior are untouched: every
challenge-specific hook is gated to the standalone challenge levels.

## Requirements

| Tool | Why |
|---|---|
| BloodRayne 2: Terminal Cut | Target game (32-bit `rayne2.exe`, 2022 build) |
| MSVC x86 toolchain | Builds `dinput8.dll` (`build.bat` calls `vcvarsall x86`; default path assumes VS 2022 Community — edit `VCVARS` for other editions) |
| Python 3 | Runs `tools/make_world.py` (stdlib only) |
| "Challenge/extras" usermod files | Source for the arena/hub levels — see below |

## Environment setup

### 1. Build the proxy

Run `build.bat` inside an *x86 Native Tools* prompt (or anywhere — it
invokes `vcvarsall x86` itself). Produces `dinput8.dll`.

### 2. Generate `world\`

The arena/hub level files come from the "challenge/extras" usermod — they
ship inside its repacked `COMMON.POD`/`LANGUAGE.POD` and are **not** part of
a stock Terminal Cut install, so they are not redistributed here.

1. Extract the usermod `WORLD\` tree from its PODs (any POD3 tool works),
   plus the **stock** `LANGUAGE.POD` (needed for the base `MSGLIST.TXT`).
2. Run:

   ```
   python tools/make_world.py --src <extracted_common>\WORLD ^
       --src <extracted_language>\WORLD --src <stock_language>\WORLD ^
       --dst <game>\world
   ```

   Multiple `--src` dirs are searched in order; an already-deployed
   `world\` is also a valid source (all patches are idempotent). Missing
   inputs are listed at the end instead of failing halfway.

`make_world.py` copies the usermod files and applies the mod's patches:
blanked `display()` banners, the mansion `99 -> 100` spawn-target fix, the
horde extreme `250 -> 100` rebalance, the `NORM`/`EXTR` derived levels, the
`menu_level` rename, and the appended localization pairs.

### 3. Deploy

Into the game directory (next to `rayne2.exe`):

- `dinput8.dll` — built in step 1 (close `rayne2.exe` first)
- `br2mod.ini` — configuration
- `world\` — generated in step 2

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

[Diagnostics]
LoaderTrace=0
WatchGlassPointer=0
LegacyGuards=0
ChainTrace=0
DifficultyTrace=0     ; 1 = log every health/rage write to br2mod_diff.log
```

`Custom` multipliers are read from the ini each time the preset is
applied — select `Custom` in Options > Difficulty (or re-select it after
editing the ini). The chosen preset is written back to the ini so it
persists across runs.
