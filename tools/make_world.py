#!/usr/bin/env python3
# make_world.py — regenerates the mod's loose world\ files from the source
# level files of the "challenge/extras" usermod (they are NOT shipped in this
# repository: they are third-party game data owned by their authors).
#
# What it does, per output file:
#   copy     byte-identical usermod file
#   rename   MENU.LVL/SCB -> menu_level.lvl/scb (the game asks for
#            world\menu_level.lvl; the usermod's minimal menu level is used)
#   derive   BATTLE_NORM/EXTR = copy of NORMAL/EXTREME with the internal
#            "NORMAL"->"NORM" / "EXTREME"->"EXTR" rename (same pipeline the
#            mod originally used, verified byte-identical)
#   patch    equal-length byte replacements:
#              - SCB: blanked display() title banners + display(done) +
#                display(Challenge Completed) — the mod HUD owns the slot
#              - LVL: BATTLE_EXTR "maxNumberSpawn" 99 -> 100 (usermod bug;
#                the wave really ends at 100 kills)
#   msglist  stock MSGLIST.TXT + the mod's appended translation pairs
#
# Usage:
#   python tools/make_world.py --src <dir> [--src <dir2> ...] --dst <world dir>
#
# Each --src is searched for every input (first match wins). A src may be:
#   - a WORLD/ dir from an extracted usermod POD      (WORLD\SPAWN_EASY.LVL)
#   - the parent of a WORLD/ dir                      (...\extracted\common_mod)
#   - an already deployed game world\ dir             (world\SPAWN_EASY.LVL)
# For msglist you also want the STOCK LANGUAGE.POD extraction (language_orig).
#
# Missing inputs are listed at the end instead of aborting halfway.
# All patches are idempotent: pointing --src at an already patched world\
# reproduces it exactly.

import os
import re
import sys
import argparse

# ---------------------------------------------------------------- manifest

ARENAS = ["SPAWN_EASY", "SPAWN_NORMAL", "SPAWN_HARD", "SPAWN_EXTREME",
          "SPAWN_INFINITE"]
BATTLES = ["A1_MANSION_PART1_BATTLE_EASY", "A1_MANSION_PART1_BATTLE_NORMAL",
           "A1_MANSION_PART1_BATTLE_HARD", "A1_MANSION_PART1_BATTLE_EXTREME"]
EXTRA = ["A1_MANSION_PART1O", "A1_MANSION_PART1_STORYMODE12"]
HUBS = ["HUB1", "HUB2", "HUB3", "HUB4", "HUB5"]

COPY = []
for n in ARENAS + BATTLES + EXTRA + HUBS:
    COPY += [n + ".LVL", n + ".SCB"]

# deployed name -> list of acceptable source names (first found wins; the
# deployed name itself keeps regeneration idempotent on a live world\)
RENAME = {
    "menu_level.lvl": ["MENU.LVL", "menu_level.lvl"],
    "menu_level.scb": ["MENU.SCB", "menu_level.scb"],
}

DERIVE = {  # output base -> (source base, old, new)
    "A1_MANSION_PART1_BATTLE_NORM": ("A1_MANSION_PART1_BATTLE_NORMAL",
                                     b"NORMAL", b"NORM"),
    "A1_MANSION_PART1_BATTLE_EXTR": ("A1_MANSION_PART1_BATTLE_EXTREME",
                                     b"EXTREME", b"EXTR"),
}

# SCB equal-length replacements (script strings must keep their declared
# length, so every replacement is space-padded to the same size)
SCB_DISP = [b"Horde Mode - Easy", b"Horde Mode - Normal", b"Horde Mode - Hard",
            b"Horde Mode - Extreme", b"Mansion Battle - Easy",
            b"Mansion Battle - Normal", b"Mansion Battle - Hard",
            b"Infinite Battle"]
SCB_CALLS = [(b"display(done)", b"display(    )"),
             (b"display(Challenge Completed)", b"display(                   )")]

# LVL patches: the mansion 100-kill tier shipped misconfigured at 99
# (usermod bug; EXTR is the file hub4 actually loads), and the horde
# extreme arena was rebalanced 250 -> 100 to match the menu label.
LVL_PATCH = {
    "A1_MANSION_PART1_BATTLE_EXTR.LVL":
        [(b"\t99    // maxNumberSpawn", b"\t100    // maxNumberSpawn")],
    "SPAWN_EXTREME.LVL":
        [(b"\t250    // maxNumberSpawn", b"\t100    // maxNumberSpawn")],
}

# files the mod pipeline rewrote in text mode (CRLF collapsed to LF)
LINEFEED = {"SPAWN_EXTREME.LVL"}

# language dirs: copy every usermod .TXT matching these prefixes
LANG_PREFIXES = ("SPAWN_", "A1_MANSION_PART1O", "A1_MANSION_PART1_BATTLE_")
LANGS = ["DE", "EN", "ES", "FR", "IT", "RU"]

# {English: {lang: translation}} appended to stock MSGLIST.TXT.
# The loader sorts the table (binary search in Localize), so appending is fine.
T = {
 "Challenge Mode":      {"ES":"Modo desafío","DE":"Herausforderung","FR":"Mode défi","IT":"Modalità sfida","RU":"Режим испытаний"},
 "Horde - 15 Enemies":  {"ES":"Horda - 15 enemigos","DE":"Horde - 15 Gegner","FR":"Horde - 15 ennemis","IT":"Orda - 15 nemici","RU":"Орда - 15 врагов"},
 "Horde - 25 Enemies":  {"ES":"Horda - 25 enemigos","DE":"Horde - 25 Gegner","FR":"Horde - 25 ennemis","IT":"Orda - 25 nemici","RU":"Орда - 25 врагов"},
 "Horde - 50 Enemies":  {"ES":"Horda - 50 enemigos","DE":"Horde - 50 Gegner","FR":"Horde - 50 ennemis","IT":"Orda - 50 nemici","RU":"Орда - 50 врагов"},
 "Horde - 100 Enemies": {"ES":"Horda - 100 enemigos","DE":"Horde - 100 Gegner","FR":"Horde - 100 ennemis","IT":"Orda - 100 nemici","RU":"Орда - 100 врагов"},
 "Mansion Battle - 15 Enemies":{"ES":"Batalla de la mansión - 15 enemigos","DE":"Villen-Kampf - 15 Gegner","FR":"Bataille du manoir - 15 ennemis","IT":"Battaglia nella villa - 15 nemici","RU":"Битва в особняке - 15 врагов"},
 "Mansion Battle - 25 Enemies":{"ES":"Batalla de la mansión - 25 enemigos","DE":"Villen-Kampf - 25 Gegner","FR":"Bataille du manoir - 25 ennemis","IT":"Battaglia nella villa - 25 nemici","RU":"Битва в особняке - 25 врагов"},
 "Mansion Battle - 50 Enemies":{"ES":"Batalla de la mansión - 50 enemigos","DE":"Villen-Kampf - 50 Gegner","FR":"Bataille du manoir - 50 ennemis","IT":"Battaglia nella villa - 50 nemici","RU":"Битва в особняке - 50 врагов"},
 "Mansion Battle - 100 Enemies":{"ES":"Batalla de la mansión - 100 enemigos","DE":"Villen-Kampf - 100 Gegner","FR":"Bataille du manoir - 100 ennemis","IT":"Battaglia nella villa - 100 nemici","RU":"Битва в особняке - 100 врагов"},
 "Survival - Infinite": {"ES":"Supervivencia - Infinita","DE":"Überleben - Unendlich","FR":"Survie - Infinie","IT":"Sopravvivenza - Infinita","RU":"Выживание - Бесконечное"},
 "Back to Main Menu":   {"ES":"Volver al menú principal","DE":"Zurück zum Hauptmenü","FR":"Retour au menu principal","IT":"Torna al menu principale","RU":"Назад в главное меню"},
 "Story Mode":          {"ES":"Modo historia","DE":"Story-Modus","FR":"Mode histoire","IT":"Modalità storia","RU":"Сюжетный режим"},
 "Combat Mode":         {"ES":"Modo combate","DE":"Kampfmodus","FR":"Mode combat","IT":"Modalità combattimento","RU":"Боевой режим"},
 "Character Museum":    {"ES":"Museo de personajes","DE":"Charakter-Museum","FR":"Musée des personnages","IT":"Museo dei personaggi","RU":"Музей персонажей"},
 "Suicide Run":         {"ES":"Carrera suicida","DE":"Selbstmord-Lauf","FR":"Course suicide","IT":"Corsa suicida","RU":"Самоубийственный забег"},
 "Testing Level":       {"ES":"Nivel de pruebas","DE":"Test-Level","FR":"Niveau de test","IT":"Livello di test","RU":"Тестовый уровень"},
 "Difficulty":          {"ES":"Dificultad","DE":"Schwierigkeit","FR":"Difficulté","IT":"Difficoltà","RU":"Сложность"},
 "Unraveller":          {"ES":"Desmembrador","DE":"Filetierer","FR":"Le Corrupteur","IT":"Unraveller","RU":"Вонюк"},
 "Custom":              {"ES":"Personalizado","DE":"Benutzerdefiniert","FR":"Personnalisé","IT":"Personalizzato","RU":"Пользовательский"},
 "Kills":               {"ES":"Bajas","DE":"Abschüsse","FR":"Victimes","IT":"Uccisioni","RU":"Убийств"},
 "Challenge Completed": {"ES":"Desafío completado","DE":"Herausforderung gemeistert","FR":"Défi terminé","IT":"Sfida completata","RU":"Испытание пройдено"},
 # --- opciones globales ---
 "Skin":                {"ES":"Apariencia","DE":"Aussehen","FR":"Apparence","IT":"Aspetto","RU":"Облик"},
 "Default":             {"ES":"Por defecto","DE":"Standard","FR":"Par défaut","IT":"Predefinito","RU":"По умолчанию"},
}
ENC = {"ES":"cp1252","DE":"cp1252","FR":"cp1252","IT":"cp1252","RU":"cp1251"}

# the usermod msglist left this key untranslated; the mod fills it.
MSGLIST_FIXES = {
    "use tab to show menu": {
        "DE": "Menü mit TAB anzeigen",
        "ES": "usa el tabulador para mostrar el menú",
        "FR": "Tab pour afficher le menu.",
        "IT": "usa TAB per mostrare il menu",
        "RU": "используй клавишу 'Tab', чтобы показать меню",
    },
}

# ---------------------------------------------------------------- helpers

def find_src(srcs, name):
    """name is a WORLD-relative path like 'SPAWN_EASY.LVL' or 'ES/FOO.TXT'."""
    for s in srcs:
        for cand in (os.path.join(s, *name.split("/")),
                     os.path.join(s, "WORLD", *name.split("/")),
                     os.path.join(s, "world", *name.split("/"))):
            if os.path.isfile(cand):
                return cand
    return None

def repl_eq(data, pairs):
    for old, new in pairs:
        assert len(old) == len(new)
        data = data.replace(old, new)
    return data

def patch_scb(data):
    data = repl_eq(data, SCB_CALLS)
    for s in SCB_DISP:
        data = repl_eq(data, [(s, b" " * len(s))])
    return data

def main():
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--src", action="append", required=True,
                    help="input dir (repeatable): extracted usermod WORLD, "
                         "deployed world\\, or stock LANGUAGE extraction")
    ap.add_argument("--dst", required=True, help="output world dir")
    a = ap.parse_args()

    missing, written = [], []
    def emit(dst_rel, data, how):
        p = os.path.join(a.dst, *dst_rel.split("/"))
        os.makedirs(os.path.dirname(p) or ".", exist_ok=True)
        old = open(p, "rb").read() if os.path.exists(p) else None
        if old != data:
            open(p, "wb").write(data)
        written.append((how, dst_rel))

    def load(name, how_if_missing):
        f = find_src(a.src, name)
        if not f:
            missing.append(name)
            return None
        return open(f, "rb").read()

    # root level files
    for name in COPY:
        d = load(name, "copy")
        if d is None: continue
        if name.upper().endswith(".SCB"):
            d = patch_scb(d)
        for o, n in LVL_PATCH.get(name, []):
            d = d.replace(o, n)
        if name in LINEFEED:
            d = d.replace(b"\r\n", b"\n")
        emit(name, d, "copy")

    # menu_level <- MENU.* (or the deployed name when src == dst)
    for out, srcs in RENAME.items():
        for src in srcs:
            d = load(src, "rename")
            if d is not None:
                emit(out, d, "rename %s -> %s" % (src, out))
                break

    # derived tiers (rename inside, then SCB blanks + LVL patches)
    for outbase, (srcbase, old, new) in DERIVE.items():
        for ext in ("LVL", "SCB"):
            out, src = outbase + "." + ext, srcbase + "." + ext
            d = load(src, "derive")
            if d is None: continue
            d = d.replace(old, new)
            if ext == "SCB":
                d = patch_scb(d)
            for o, n in LVL_PATCH.get(out, []):
                d = d.replace(o, n)
            emit(out, d, "derive %s -> %s" % (src, out))

    # per-language usermod .TXT + stock msglist + our pairs
    for lang in LANGS:
        low = lang.lower()
        for s in a.src:
            for sub in (lang, os.path.join("WORLD", lang),
                        os.path.join("world", lang)):
                d = os.path.join(s, sub)
                if os.path.isdir(d):
                    for f in sorted(os.listdir(d)):
                        if f.upper().endswith(".TXT") and \
                           f.upper().startswith(LANG_PREFIXES):
                            emit(lang + "/" + f,
                                 open(os.path.join(d, f), "rb").read(),
                                 "copy-lang")
        if lang == "EN":
            continue  # English is the key language: no EN msglist exists
        m = load(lang + "/MSGLIST.TXT", "msglist")
        if m is None:
            m = load(lang + "/msglist.txt", "msglist")
        if m is not None:
            if not m.endswith(b"\r\n"):
                m += b"\r\n"
            for en, trs in MSGLIST_FIXES.items():
                m = m.replace(('"%s", ""' % en).encode(ENC[lang]),
                              ('"%s", "%s"' % (en, trs[lang])).encode(ENC[lang]))
            for en, trs in T.items():
                key = ('"%s",' % en).encode("cp1252")
                if key not in m:   # idempotent: skip pairs already present
                    m += ('"%s", "%s"\r\n' % (en, trs[lang])).encode(ENC[lang])
            emit(low + "/msglist.txt", m, "msglist")

    for how, rel in written:
        print("%-28s %s" % (how, rel))
    if missing:
        print("\nMISSING (%d):" % len(missing))
        for m in sorted(set(missing)):
            print("  ", m)
        print("\nPoint --src at a dir containing them (usermod POD "
              "extraction / deployed world).")
        return 1
    print("\nOK — %d files in %s" % (len(written), a.dst))
    return 0

if __name__ == "__main__":
    sys.exit(main())
