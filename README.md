# BloodRayne2HordeModeExtras

Independent `dinput8.dll` proxy mod for **BloodRayne 2: Terminal Cut** (GOG).

Adds a **Challenge Mode** entry to the main menu (Horde arenas, Mansion
battles, Infinite Survival), extra level entries inside the native Extras
menu, and a global **Difficulty** option under Options.

The DLL is self-contained: it forwards `DirectInput8Create` to the system
`dinput8.dll` and coexists with other proxies (e.g. `binkw32.dll`).

## Build

Requires MSVC x86 tools. Run `build.bat` inside an *x86 Native Tools*
prompt (or anywhere — it invokes `vcvarsall x86` itself). Produces
`dinput8.dll`.

## Install

1. Copy `dinput8.dll` and `br2mod.ini` next to `rayne2.exe`.
2. The mod also expects its loose level/resource files under `world\`
   next to the executable. Those files are derived from the game itself
   and are **not redistributable** — the repository contains the code only.

### Generating `world\`

The arena/hub level files come from the "challenge/extras" usermod that
ships them inside its repacked `COMMON.POD`/`LANGUAGE.POD` — they are not
part of a stock Terminal Cut install. To reproduce the deployed `world\`:

1. Extract the usermod `WORLD\` tree from its PODs (any POD3 tool works)
   plus the **stock** `LANGUAGE.POD` (needed for the base `MSGLIST.TXT`).
2. Run:

   ```
   python tools/make_world.py --src <extracted_common>\WORLD ^
       --src <extracted_language>\WORLD --src <stock_language>\WORLD ^
       --dst <game>\world
   ```

   Multiple `--src` dirs are searched in order; an already-deployed
   `world\` is also a valid source (all patches are idempotent). Missing
   inputs are reported instead of failing halfway.

Original game files are never modified.
