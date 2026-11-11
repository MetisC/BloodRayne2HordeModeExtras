#!/usr/bin/env python3
"""make_release.py — build the end-user release zip for Nexus/GitHub.

Packages the proxy DLL, default config, generated levels, challenge
character and appearance assets, menu scene, localization and two text guides. Keeps
stock archives, player records and developer artifacts out of the zip.

Usage:
  python tools/make_release.py --game "H:\GOG\BloodRayne 2 Terminal Cut" ^
      --out <outdir> [--ver 1.0.0]
"""
import argparse, os, re, sys, zipfile
from pathlib import Path

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

GENERATED = (
    ["a1_mansion_part1_battle_%s" % n for n in
     ("100", "250", "15g", "25g", "50g", "100g", "250g")] +
    ["chb_hor_%s" % n for n in
     ("15", "25", "50", "100", "250",
      "15g", "25g", "50g", "100g", "250g")] +
    ["%s_%s" % (arena, count) for arena in
     ("chb_fireplace", "chb_bar", "chb_meatpacking_arena", "chb_insect_plaza",
      "chb_mansion_roof", "chb_shroud_roof", "chb_woodchipper")
     for count in ("15", "25", "50", "100", "250",
                   "15g", "25g", "50g", "100g", "250g")]
)
RETITLED = ["A1_MANSION_PART1_BATTLE_%s" % n for n in ("EASY", "NORMAL", "HARD")]
SCENE    = ["menu_level", "HUB5"]
LANGS    = ["DE", "ES", "FR", "IT", "RU"]

CHALLENGE_MODELS = (
    "br2_ferril_player", "br2_dhampir_player", "br2_dhampir_m_player",
    "br2_ephemera_player", "br2_kestral_player", "br2_kagan_player",
    "br2_ninjaf_player", "br2_ninjam_player",
)
CHALLENGE_WEAPONS = (
    "br2_dhampir_blades", "br2_dhampir_scimitar", "br2_kagan_sword", "br2_ninja_blade",
)
CHALLENGE_ANIMATIONS = (
    "br2_ephemera/dead", "br2_ephemera/death", "br2_ephemera/flurry",
    "br2_kestral/combo3", "br2_kagan/shockwave",
    "br2_ninjam/death", "br2_ninjam/dead",
    "br2_kagan/hurtfront", "br2_kagan/hurtback", "br2_kagan/hurtleft", "br2_kagan/hurtright",
)

README_TXT = """BloodRayne 2: Terminal Cut - Challenge Mode & Extras (br2mod)
================================================================

WHAT IT IS
----------
Independent dinput8.dll proxy mod. Adds a Challenge Mode entry to the
main menu (eight campaign arenas plus Horde Mode, 15-250 enemies, optional guns,
best times), a global Difficulty preset selector under Options
(including a Custom preset driven by br2mod.ini), a global Skin
selector, and localized mod strings for all six game languages.
Challenge runs offer Base or Maximum health and rage capacity.
The Character option selects the fighter for a challenge run.
Fighters have individual damage and resistance multipliers in Challenge
Mode, applied before difficulty. Updated fighters use new record sections.
Photo Mode is available from Pause or via F12 / L3+R3 in gameplay.
Enhanced combat adds a timed parry and follow-up, hitstop, recovery
protection, a combo counter, style ranks from D to V, and cinematic
effects for feeding executions and dismemberment.
If an armed enemy repels a frontal feeding attempt, tap Parry just
before the follow-up to roll away and get up facing the enemy.
Options > Combat Effects shows five switches when Combat is set to Enhanced.
See COMBAT_SETTINGS.txt for a quick setup and the full INI reference.
To Dodge Offset, dodge during a melee chain and tap blade or kick
during the dodge or just after it. The follow-up earns style if it hits.
Twisted Wind briefly blocks enemy melee at the start of its active kick.
During challenge results, Esc and Start are disabled until the automatic
return to the main menu.
In Enhanced challenges, best time and peak style are separate records.
Normal challenges track best time.

INSTALL
-------
1. Extract this zip into your game folder (next to rayne2.exe):
     - dinput8.dll
     - br2mod.ini
     - br2mod_combat.ini
     - world\\  (whole folder)
     - data\\ and models\\  (challenge characters, equipment and appearances)
     - animations\\  (challenge character moves)
2. If you already have a dinput8.dll (another proxy mod), back it up
   first - only one dinput8 proxy can be loaded.
3. If upgrading, keep br2mod.ini to retain your selections. The new
   br2mod_combat.ini contains the current defaults. Reapply custom
   combat settings there if needed.
4. Play. The Challenge Mode entry is on the main menu.
   In Options, set Combat to Enhanced to use the combat changes.
   Save edits to br2mod_combat.ini to tune them while the game runs.

UNINSTALL
---------
Delete dinput8.dll and the files this zip added under world\\, data\\, models\\ and animations\\.
Your save games and POD archives are never touched. br2mod_times.ini
(challenge records) may be deleted to reset them.

QUE ES
------
Mod proxy dinput8.dll independiente. Anade un modo Desafio al menu
principal (ocho arenas de campa\u00f1a y Modo Horda, 15-250 enemigos, armas
opcionales, mejores tiempos), un selector global de Dificultad en
Opciones (incluido un preset Custom en br2mod.ini), un selector global
de Skin, y cadenas del mod traducidas a los seis idiomas del juego.
En Desafio puedes elegir capacidad de vida y furia Base o Maxima.
La opcion Personaje selecciona quien lucha en la arena.
Cada luchador tiene ajustes de dano y resistencia en Desafio, antes de
la dificultad. Los luchadores ajustados usan secciones nuevas de records.
Modo Foto se abre desde Pausa o con F12 / L3+R3 en partida.
Combate Mejorado anade parry y replica, hitstop, proteccion tras
recibir dano, contador de combos, rangos de estilo de D a V y
efectos de camara en ejecuciones y mutilaciones.
Si un enemigo armado rechaza un mordisco frontal, pulsa Parry antes
del segundo golpe para rodar hacia atras y levantarte de cara al enemigo.
Opciones > Efectos de combate muestra cinco interruptores con Combate
Mejorado. Consulta COMBAT_SETTINGS.txt para la guia y referencia del INI.
Para continuar un combo con Dodge Offset, esquiva en mitad de la cadena
y pulsa cuchilla o patada durante la esquiva o al salir. El golpe suma
estilo si conecta. Viento retorcido bloquea brevemente el cuerpo a cuerpo
enemigo al inicio de la patada. Durante los resultados, Esc y Start quedan
bloqueados hasta la vuelta automatica al menu principal.
En Desafio con Mejorado hay records separados de tiempo y estilo.
En Normal se guarda el mejor tiempo.

INSTALACION
-----------
1. Descomprime este zip en la carpeta del juego (junto a rayne2.exe):
     - dinput8.dll
     - br2mod.ini
     - br2mod_combat.ini
     - world\\  (carpeta completa)
     - data\\ y models\\  (personajes de Desafio, equipo y apariencias)
     - animations\\  (movimientos de los personajes de Desafio)
2. Si ya tienes un dinput8.dll (otro mod proxy), haz copia antes:
   solo puede cargarse un proxy dinput8.
3. Si actualizas, conserva br2mod.ini para mantener tus selecciones.
   El nuevo br2mod_combat.ini incluye los valores actuales. Vuelve
   a aplicar ahi tus ajustes de combate personalizados si los usas.
4. A jugar. El modo Desafio esta en el menu principal.
   En Opciones, selecciona Combate Mejorado para activar los cambios.
   Guarda br2mod_combat.ini para ajustarlos con el juego abierto.

DESINSTALAR
-----------
Borra dinput8.dll y los ficheros que el zip anadio en world\\, data\\, models\\ y animations\\.
Las partidas guardadas y los POD originales nunca se tocan.
br2mod_times.ini puede borrarse para reiniciar los records de Desafio.
"""


def combat_reference():
    """Keep the text guide in sync with the shipped INI and parser ranges."""
    root = Path(ROOT)
    settings = (root / "enhanced_settings.inl").read_text(encoding="utf-8")
    bounds = {
        name: (lo.strip().removesuffix("f"), hi.strip().removesuffix("f"))
        for name, lo, hi in re.findall(
            r"\bX\((?:int|float),\s*(\w+),[^,]+,\s*([^,]+),\s*([^\n)]+)\)",
            settings
        )
        if name != "trace"
    }
    lines = [
        "BloodRayne 2 - Enhanced Combat Settings",
        "=======================================",
        "",
        "QUICK SETUP",
        "In Options, set Combat to Enhanced and open Combat Effects.",
        "Turn off Finisher Cinematics and Impact Pause to remove extra finish",
        "slowdown, zoom and hitstop while keeping the combat mechanics enabled.",
        "",
        "Finisher Cinematics covers combo finishers, feeding executions,",
        "dismemberment and special strikes on live grounded targets. Native",
        "time powers, victory slowdown and parry slowdown are separate.",
        "Impact Pause controls hitstop on attacks dealt and received.",
        "Parry controls the parry, the riposte and the back roll escape when an",
        "armed enemy repels a frontal feeding attempt. Press Parry just before",
        "the enemy's follow-up to escape. A held button does not count.",
        "Dodge Offset controls the ability to resume a combo after dodging.",
        "Style and Combo hides their in-game display. Scoring and results continue.",
        "",
        "Menu choices are saved in br2mod.ini. An off switch masks its group.",
        "Turning it back on restores the values in br2mod_combat.ini.",
        "The menu switches share the same challenge records.",
        "",
        "ADVANCED INI REFERENCE",
        "Edit br2mod_combat.ini next to rayne2.exe. The mod reloads edits while",
        "the game runs. The ranges below are the individual parser limits.",
        "Time settings use milliseconds unless a comment specifies otherwise.",
        "0/1 switches use 0 for off and 1 for on.",
        "",
    ]
    seen = set()
    for line in (root / "br2mod_combat.ini").read_text(encoding="utf-8").splitlines():
        if line.startswith(";"):
            lines.append(line[1:].lstrip())
        elif match := re.fullmatch(r"(\w+)\s*=\s*(\S+)", line):
            name, default = match.groups()
            lo, hi = bounds[name]
            lines.append(f"  {name} = {default}  (range {lo} to {hi})")
            seen.add(name)
        else:
            lines.append(line)
    if seen != bounds.keys():
        raise ValueError(f"Missing shipped combat settings: {sorted(bounds.keys() - seen)}")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", required=True, help="game root (with rayne2.exe)")
    ap.add_argument("--out", required=True, help="output dir for the zip")
    ap.add_argument("--ver", default="1.0.0")
    a = ap.parse_args()

    game = a.game
    items = []  # (src, arcname)
    dll = os.path.join(ROOT, "dinput8.dll")
    with open(dll, "rb") as binary:
        if b"BR2-COMBAT-TEST" in binary.read():
            sys.exit("Refusing to package a combat test DLL. Run build.bat without arguments.")
    items.append((dll, "dinput8.dll"))
    items.append((os.path.join(ROOT, "br2mod.ini"), "br2mod.ini"))
    items.append((os.path.join(ROOT, "br2mod_combat.ini"), "br2mod_combat.ini"))

    assets = [f"{folder}/{name}.{ext}"
              for name in CHALLENGE_MODELS
              for folder, ext in (("data", "skb"), ("models", "bfm"), ("models", "jug"))
              if not (name == "br2_ferril_player" and ext == "jug")]
    assets.extend(f"models/{name}.smb" for name in CHALLENGE_WEAPONS)
    assets.extend(f"animations/{name}.ani" for name in CHALLENGE_ANIMATIONS)
    source = (Path(ROOT) / "br2mod.cpp").read_text(encoding="utf-8")
    outfits = source.split("static constexpr ModOutfit ModOutfits[] = {", 1)[1].split("\n};", 1)[0]
    appearance_files = re.findall(r'"((?:models|data)\\\\[^"]+)"', outfits)
    if not appearance_files:
        sys.exit("No appearance assets found in ModOutfits")
    assets.extend(dict.fromkeys(name.replace("\\\\", "/") for name in appearance_files))
    for asset in assets:
        src = os.path.join(game, asset)
        if not os.path.isfile(src):
            sys.exit("missing challenge character asset " + asset)
        items.append((src, asset))

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
        z.writestr("COMBAT_SETTINGS.txt", combat_reference())
    print("%d files -> %s" % (len(items) + 2, zpath))


if __name__ == "__main__":
    main()
