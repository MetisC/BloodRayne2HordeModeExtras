#!/usr/bin/env python3
"""make_release.py — build the end-user release zip for Nexus/GitHub.

Packages ONLY mod files: the proxy DLL, default config, generated world
levels, menu scene, per-language msglist and a README.txt. No stock game
files, no runtime files (br2mod_times.ini), no dev artifacts (.TXT deps).

Usage:
  python tools/make_release.py --game "H:\GOG\BloodRayne 2 Terminal Cut" ^
      --out <outdir> [--ver 1.0.0]
"""
import argparse, os, sys, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

GENERATED = (
    ["a1_mansion_part1_battle_%s" % n for n in
     ("100", "250", "15g", "25g", "50g", "100g", "250g")] +
    ["chb_hor_%s" % n for n in
     ("15", "25", "50", "100", "250",
      "15g", "25g", "50g", "100g", "250g")]
)
RETITLED = ["A1_MANSION_PART1_BATTLE_%s" % n for n in ("EASY", "NORMAL", "HARD")]
SCENE    = ["menu_level", "HUB5"]
LANGS    = ["DE", "ES", "FR", "IT", "RU"]

README_TXT = """BloodRayne 2: Terminal Cut - Challenge Mode & Extras (br2mod)
================================================================

WHAT IT IS
----------
Independent dinput8.dll proxy mod. Adds a Challenge Mode entry to the
main menu (Mansion Battle / Horde Mode, 15-250 enemies, optional guns,
best times), a global Difficulty preset selector under Options
(including a Custom preset driven by br2mod.ini), a global Skin
selector, and localized mod strings for all six game languages.
Campaign, Extras and stock files are untouched.

INSTALL
-------
1. Extract this zip into your game folder (next to rayne2.exe):
     - dinput8.dll
     - br2mod.ini
     - world\\  (whole folder)
2. If you already have a dinput8.dll (another proxy mod), back it up
   first - only one dinput8 proxy can be loaded.
3. Play. The Challenge Mode entry is on the main menu.

UNINSTALL
---------
Delete dinput8.dll and the files this zip added under world\\.
Your save games and POD archives are never touched. br2mod_times.ini
(best times) may be deleted to reset records.

QUE ES
------
Mod proxy dinput8.dll independiente. Anade un modo Desafio al menu
principal (Batalla en la Mansion / Modo Horda, 15-250 enemigos, armas
opcionales, mejores tiempos), un selector global de Dificultad en
Opciones (incluido un preset Custom en br2mod.ini), un selector global
de Skin, y cadenas del mod traducidas a los seis idiomas del juego.
La campana, los Extras y los ficheros originales quedan intactos.

INSTALACION
-----------
1. Descomprime este zip en la carpeta del juego (junto a rayne2.exe):
     - dinput8.dll
     - br2mod.ini
     - world\\  (carpeta completa)
2. Si ya tienes un dinput8.dll (otro mod proxy), haz copia antes:
   solo puede cargarse un proxy dinput8.
3. A jugar. El modo Desafio esta en el menu principal.

DESINSTALAR
-----------
Borra dinput8.dll y los ficheros que el zip anadio en world\\.
Las partidas guardadas y los POD originales nunca se tocan.
br2mod_times.ini (mejores tiempos) puede borrarse para reiniciar records.
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True, help="game root (with rayne2.exe)")
    ap.add_argument("--out", required=True, help="output dir for the zip")
    ap.add_argument("--ver", default="1.0.0")
    a = ap.parse_args()

    game = a.game
    items = []  # (src, arcname)
    items.append((os.path.join(ROOT, "dinput8.dll"), "dinput8.dll"))
    items.append((os.path.join(ROOT, "br2mod.ini"), "br2mod.ini"))

    world = os.path.join(game, "world")
    for base in GENERATED + RETITLED + SCENE:
        for kind in (("lvl", ".lvl", ".LVL"), ("scb", ".scb", ".SCB")):
            for ext in kind[1:]:
                src = os.path.join(world, base + ext)
                if os.path.isfile(src):
                    items.append((src, "world/" + base + ext))
                    break
            else:
                sys.exit("missing %s for %s" % (kind[0], base))
    for lang in LANGS:
        src = os.path.join(world, lang, "msglist.txt")
        if not os.path.isfile(src):
            sys.exit("missing " + src)
        items.append((src, "world/%s/msglist.txt" % lang))

    zpath = os.path.join(
        a.out, "BloodRayne2HordeModeExtras-v%s.zip" % a.ver)
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for src, arc in items:
            z.write(src, arc)
        z.writestr("README.txt", README_TXT)
    print("%d files -> %s" % (len(items) + 1, zpath))


if __name__ == "__main__":
    main()
