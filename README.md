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

Original game files are never modified.
