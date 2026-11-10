// br2mod — dinput8.dll proxy for BloodRayne 2: Terminal Cut (GOG)
// Adds "Challenge Mode" to the main menu, extra levels inside the
// native Extras menu, and a global "Difficulty" preset inside Options.
// 100% independent of Gunsucked.
//
// Injection: rayne2.exe imports ONLY DirectInput8Create from dinput8.dll.
// Our dll in the game dir shadows System32's; we forward the export.
//
// Hooks: 2 inline patches via `push addr; ret` (6 bytes, distance-safe):
//   A) 0x561E1A  epilogue reached by EVERY menu builder -> we inspect
//      menuState and append items, or build our menu on free id 0x3D
//      (its empty jump-table slot also lands on 0x561E1A).
//   B) 0x5670E1  action dispatcher (edi=action code) -> our codes.
//
// ABI of the menu helpers (verified by disassembly):
//   FUN_0055a760(text)  : ecx/edi=display string -> item+0xC,
//                         eax=action code -> item+4, type 0,
//                         esi=secondary string -> item+0x54 (0 = skip)
//   FUN_0055a7b0(title) : same, but type 1 (non-selectable header)
//   esi MUST be 0 during the call or it is dereferenced -> crash.
//
// Build (x86 Native Tools prompt):
//   cl /O2 /MD /LD br2mod.cpp /link /DEF:dinput8.def /OUT:dinput8.dll

#include <Windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <stdlib.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <xinput.h>
#pragma comment(lib, "user32.lib")

// ----------------------------- addresses ----------------------------------
#define FN_LOCALIZE      0x5898A0  // localize(english)->localized
#define FN_ADD_MENU_ITEM 0x55A760  // ecx/edi=text eax=actionCode esi=text2
#define FN_CALL_MENU     0x565E80  // callMenu(menuId): push+enter submenu
#define FN_MENU_BACK     0x559F10  // pop menu stack (native "Back")

#define G_MENU_STATE    0x5EF4BF8
#define G_MENU_REBUILD  0x5EF5658
#define G_ITEM_COUNT    0x5EF4C28  // items in current menu build
#define G_ITEM_ARRAY    0x5EEE358  // item structs, stride 0xD8
#define G_SELIDX        0x5EF4AE8  // selected row (restored by MenuBack)
#define G_PICKED_LVL    0x5EF4948
#define G_GAME_STATE    0x7C07F8   // +0x1E8 = (dead) difficulty field
#define G_CURRENT_LEVEL 0x5E33258
#define G_SITUATION     0x7C2478
#define G_CARNAGE       0x7BF4BC
#define G_HANDLER_ARRAY 0x5DCC0A0  // [0]=POD 0x5A3FD0 [1]=loose 0x4937B0
#define G_PLAYER_OBJ    0x61276EC  // [ptr] Rayne object (from gunsucked rayne2.CT)

// Real damage writes: `fsubr [esi+0x39C8]` = health - st0(dmg), inlined per
// actor class (19 sites) + 1 rage site `fsubr [esi+0x8360]`. Patched with a
// CALL (5B) so one shared handler serves all sites — the call pushes the
// per-site return for us. Scaling: fmul st0 by dmgIn(player)/dmgOut(other).
static const DWORD HEALTH_DMG_SITES[] = {
    0x480DB1,0x4AC401,0x4B2938,0x4B2977,0x4BDEF8,0x4BDF37,0x527238,
    0x52D7D4,0x53B721,0x544B14,0x5515B4,0x58D5B4,0x58D5F3,0x5BD694,
    0x5BD6D7,0x610394,0x63C6A8,0x63C6E7,0x66DE08
};
#define RAGE_DMG_SITE 0x5515A4
// Per-class TakeDamage path: `fld [reg+0x39C8]; fsub <dmg>; fstp [reg+0x39C8]`
// (the fstp is often followed by the post-damage update call 0x4030A0). The
// victim register differs per class — one handler per register.
static const DWORD HIT_FSTP_ESI[] = {
    0x43778C,0x43B36D,0x44C68E,0x44F6CC,0x47A25D,0x4A9B70,0x4B29EA,
    0x4BDFAA,0x525240,0x533355,0x547B4D,0x5515DF,0x55219C,0x552ABC,
    0x58D65C,0x5C61D8,0x63C75A,0x640354,0x6530FD
};
static const DWORD HIT_FSTP_EDI[] = {
    0x417577,0x478806,0x47EEEF,0x4B843D,0x4C3A90,0x4EE2EB,0x539668,0x614DE6
};
static const DWORD HIT_FSTP_EAX[] = { 0x55C3A5 };
static float g_new, g_mult, g_dmgIn = 1.f, g_dmgOut = 1.f, g_cost = 1.f;

#define MENU_MAIN      0x01
#define MENU_PAUSE     0x0E
#define MENU_EXTRAS    0x0F        // native Extras menu
#define MENU_OPTIONS   0x17
#define MENU_CHALLENGE 0x3D        // free slot -> lands on hooked epilogue
#define MENU_EFFECTS   0x3E        // second free slot, same builder epilogue
#define MENU_SKINS     0x33        // third free slot, same builder epilogue
                                   // (0x09 is natively auto-popped by FUN_00566b70)

// Action codes >= 0x110: natives stop at 0x10C, zero collisions.
#define AC_CHALLENGE_MENU 0x110
#define AC_DIFFICULTY     0x111
#define AC_COMBAT         0x112
#define AC_COUNT_CYCLE    0x113    // cycle enemy-count selection
#define AC_GUNS_CYCLE     0x114    // cycle weapons on/off for the run
#define AC_BARS_CYCLE     0x115
#define AC_EFFECTS_MENU   0x116
#define AC_EFFECT_BASE    0x117
#define AC_BACK           0x11C
#define AC_SKIN           0x11D
#define AC_PHOTO          0x11E
#define AC_EXTRA_BASE     0x120    // 0x120.. extra launches
#define AC_ARENA_BASE     0x130    // 0x130.. arena launches at selected count
#define AC_SKIN_PICK_BASE 0x140    // 0x140.. skin list picks

// ----------------------------- data ---------------------------------------
struct Preset { const char* name; float dmgIn, dmgOut, cost; };
static const Preset PRESETS[] = {
    { "Minion",     0.50f, 1.50f, 0.50f },
    { "Dhampir",    1.00f, 1.00f, 1.00f },
    { "Overlord",   1.50f, 0.75f, 1.50f },
    { "Unraveller", 2.00f, 0.50f, 2.00f },
    { "Custom",     1.00f, 1.00f, 1.00f },  // multipliers live in br2mod.ini
};
#define NUM_PRESETS 5
#define DIFF_CUSTOM  4
static int g_diff = 1;
static volatile LONG g_presetDirty = 0;
enum CombatMode { CombatNormal, CombatEnhanced, CombatCount };
static const char* const COMBAT_NAMES[] = { "Normal", "Enhanced" };
static int g_combat = CombatNormal;
static volatile LONG g_combatDirty = 0;
enum CombatEffect { EffectCinematics, EffectHitstop, EffectParry, EffectDodge, EffectStyleHud, EffectCount };
static const struct { const char* key; const char* label; } COMBAT_EFFECTS[] = {
    {"finisherCinematics", "Finisher Cinematics"},
    {"impactPause", "Impact Pause"},
    {"parry", "Mod Parry"},
    {"dodgeOffset", "Dodge Offset"},
    {"styleHud", "Style and Combo"},
};
static_assert(_countof(COMBAT_EFFECTS) == EffectCount && AC_EFFECT_BASE + EffectCount == AC_BACK,
              "Combat effect table and menu actions must match");
static volatile LONG g_effects = (1L << EffectCount) - 1;
static volatile LONG g_effectsDirty = 0;
static bool CombatEffectEnabled(CombatEffect effect) {
    return (g_effects & (1L << effect)) != 0;
}
static char g_pendingChain[64];
static int g_legacyGuards = 0;
static int g_chainTrace = 0;
static int g_diffTrace = 0;
static int g_trigTrace = 0;
static FILE* g_trigLog;
static DWORD g_hObj;
static float g_hOld;
static FILE* g_dlog;
static const char g_healthtag[] = "HEALTH";
static const char g_ragetag[]   = "RAGE  ";
static void __cdecl LogDmgHit(const char* kind) {
    if (!g_dlog) { if (fopen_s(&g_dlog, "br2mod_diff.log", "a") || !g_dlog) return; }
    DWORD pl = *(DWORD*)G_PLAYER_OBJ;
    fprintf(g_dlog, "%s obj=%08X %s old=%.1f rawDmg=%.1f mult=%.2f -> %.1f\n",
            kind, g_hObj, g_hObj == pl ? "PLAYER" : "other ",
            (double)g_hOld, (double)g_new, (double)g_mult,
            (double)(g_hOld - g_new * g_mult));
    fflush(g_dlog);
}
static float IniFloat(const char* key, const char* def) {
    char b[32];
    GetPrivateProfileStringA("Custom", key, def, b, sizeof b, ".\\br2mod.ini");
    return (float)atof(b);
}
static void ApplyPreset() {
    if (g_diff == DIFF_CUSTOM) {
        g_dmgIn  = IniFloat("dmgIn",  "1.0");
        g_dmgOut = IniFloat("dmgOut", "1.0");
        g_cost   = IniFloat("cost",   "1.0");
        return;
    }
    g_dmgIn  = PRESETS[g_diff].dmgIn;
    g_dmgOut = PRESETS[g_diff].dmgOut;
    g_cost   = PRESETS[g_diff].cost;
}

// ------------------------- skin override -----------------------------------
// The game keeps the outfit id in the player-state object at +0x3CC (ids
// 1..10; native Outfits menu actions push exactly those). Every asset lookup
// (model, guns, blades, jug, harpoon) resolves it through GetOutfitIndex
// (0x4F6930), which is only called INSIDE the path getters. So the winning
// move needs no outfit-function hooks at all (Gunsucked already owns most of
// that pipeline): we call-detour the 7 `call GetOutfitIndex` sites so the
// player's +0x3CC holds our id right before any lookup reads it, and keep it
// written per-frame. Our ids are stock (<=10) so Gunsucked's own hooks pass
// them straight through. "Default" writes nothing -- native and Gunsucked
// outfit selection keep working untouched.
typedef char* (__stdcall *OutfitName_t)(int id);
static OutfitName_t GetInternalNameN = (OutfitName_t)0x4F6AD0;
static OutfitName_t GetDisplayNameN  = (OutfitName_t)0x4F6B10;
typedef unsigned int (__cdecl *GetOutfitIndex_t)(void);
#define OUTFIT_INDEX_FN 0x4F6930

static int  g_skinCount = 0;
static int  g_skinIdx = 0;                   // selected index into g_skinOrder
static int  g_skinId = 0;                    // resolved outfit id (0=default)
static int  g_skinsNeedSelect = 0;           // one-shot cursor placement on open
static DWORD g_skinsOpenedAt = 0;            // debounce for held confirm input
static int  g_nativeOutfit = -1;             // last game-written +0x3CC (-1=none)
static int  g_lastWrote = -1;                // our last +0x3CC write (-1=none)
static char g_skinIni[40] = "default";
static volatile LONG g_skinDirty = 0;
struct ModOutfit {
    int id;
    const char* internal;
    const char* label;
    const char* model;
    const char* physics;
    const char* modelFile;
    const char* skeletonFile;
    const char* physicsFile;
    const char* labelSuffix;
};
// Reserved outside both stock IDs and Gunsucked's custom outfit range.
static constexpr ModOutfit ModOutfits[] = {
    {0x4252E001, "br2_ephemera", "Ephemera", "br2_ephemera_hair.dfm", "ephemera.jug",
     "models\\br2_ephemera_hair.bfm", "data\\br2_ephemera_hair.skb", nullptr},
    {0x4252E002, "br2_kestral", "Kestral", "br2_kestral.dfm", "kestral.jug",
     "models\\br2_kestral.bfm", "data\\br2_kestral.skb", nullptr},
    {0x4252E003, "br2_dhampir_f", "Dhampir", "br2_dhampir_f.dfm", "br2_dhampir_f.jug",
     "models\\br2_dhampir_f.bfm", "data\\br2_dhampir_f.skb", "models\\br2_dhampir_f.jug"},
    {0x4252E004, "br2_ninjaf", "Ninja", "br2_ninjaf.dfm", "rayne_armor.jug",
     "models\\br2_ninjaf.bfm", nullptr, nullptr},
    {0x4252E005, "br2_fpunk_female", "Punk 1", "br2_fpunk_female.dfm", "br2_fpunk_female.jug",
     "models\\br2_fpunk_female.bfm", nullptr, "models\\br2_fpunk_female.jug"},
    {0x4252E006, "br2_fpunk_female_2", "Punk 2", "br2_fpunk_female_2.dfm", "br2_fpunk_female.jug",
     "models\\br2_fpunk_female_2.bfm", nullptr, "models\\br2_fpunk_female.jug"},
    {0x4252E007, "br2_fpunk_female_3", "Punk 3", "br2_fpunk_female_3.dfm", "br2_fpunk_female.jug",
     "models\\br2_fpunk_female_3.bfm", nullptr, "models\\br2_fpunk_female.jug"},
    {0x4252E008, "br2_fpunk_female_4", "Punk 4", "br2_fpunk_female_4.dfm", "br2_fpunk_female.jug",
     "models\\br2_fpunk_female_4.bfm", nullptr, "models\\br2_fpunk_female.jug"},
    {0x4252E009, "br2_fpunk_female_5", "Punk 5", "br2_fpunk_female_5.dfm", "br2_fpunk_female.jug",
     "models\\br2_fpunk_female_5.bfm", nullptr, "models\\br2_fpunk_female.jug"},
    {0x4252E00A, "br2_fpunk_female_6", "Punk 6", "br2_fpunk_female_6.dfm", "br2_fpunk_female.jug",
     "models\\br2_fpunk_female_6.bfm", nullptr, "models\\br2_fpunk_female.jug"},
    {0x4252E00B, "br2_zguest_f", "Ballroom Guest", "br2_zguest_f.dfm", "br2_zguest_f.jug",
     "models\\br2_zguest_f.bfm", "data\\br2_zguest_f.skb", "models\\br2_zguest_f.jug", " 1"},
    {0x4252E00C, "br2_zguest_f_2", "Ballroom Guest", "br2_zguest_f_2.dfm", "br2_zguest_f.jug",
     "models\\br2_zguest_f_2.bfm", "data\\br2_zguest_f.skb", "models\\br2_zguest_f.jug", " 2"},
    {0x4252E00D, "br2_zguest_f_3", "Ballroom Guest", "br2_zguest_f_3.dfm", "br2_zguest_f.jug",
     "models\\br2_zguest_f_3.bfm", "data\\br2_zguest_f.skb", "models\\br2_zguest_f.jug", " 3"},
    {0x4252E00E, "br2_zguest_f_4", "Ballroom Guest", "br2_zguest_f_4.dfm", "br2_zguest_f.jug",
     "models\\br2_zguest_f_4.bfm", "data\\br2_zguest_f.skb", "models\\br2_zguest_f.jug", " 4"},
    {0x4252E00F, "br2_zguest_f_5", "Ballroom Guest", "br2_zguest_f_5.dfm", "br2_zguest_f.jug",
     "models\\br2_zguest_f_5.bfm", "data\\br2_zguest_f.skb", "models\\br2_zguest_f.jug", " 5"},
    {0x4252E010, "br2_zguest_f_6", "Ballroom Guest", "br2_zguest_f_6.dfm", "br2_zguest_f.jug",
     "models\\br2_zguest_f_6.bfm", "data\\br2_zguest_f.skb", "models\\br2_zguest_f.jug", " 6"},
    {0x4252E011, "br2_zpunkf", "Zerenski Assassin", "br2_zpunkf.dfm", "zpunkf.jug",
     "models\\br2_zpunkf.bfm", "data\\br2_zpunkf.skb", nullptr, " 1"},
    {0x4252E012, "br2_zpunkf_2", "Zerenski Assassin", "br2_zpunkf_2.dfm", "zpunkf.jug",
     "models\\br2_zpunkf_2.bfm", "data\\br2_zpunkf.skb", nullptr, " 2"},
    {0x4252E013, "br2_zpunkf_3", "Zerenski Assassin", "br2_zpunkf_3.dfm", "zpunkf.jug",
     "models\\br2_zpunkf_3.bfm", "data\\br2_zpunkf.skb", nullptr, " 3"},
    {0x4252E014, "br2_zpunkf_4", "Zerenski Assassin", "br2_zpunkf_4.dfm", "zpunkf.jug",
     "models\\br2_zpunkf_4.bfm", "data\\br2_zpunkf.skb", nullptr, " 4"},
    {0x4252E015, "br2_zpunkf_5", "Zerenski Assassin", "br2_zpunkf_5.dfm", "zpunkf.jug",
     "models\\br2_zpunkf_5.bfm", "data\\br2_zpunkf.skb", nullptr, " 5"},
    {0x4252E016, "br2_zpunkf_6", "Zerenski Assassin", "br2_zpunkf_6.dfm", "zpunkf.jug",
     "models\\br2_zpunkf_6.bfm", "data\\br2_zpunkf.skb", nullptr, " 6"},
};
static constexpr int MaxNativeOutfits = 10;
static int g_skinOrder[1 + MaxNativeOutfits + sizeof(ModOutfits) / sizeof(ModOutfits[0])];

static const ModOutfit* FindModOutfit(int id) {
    for (const auto& outfit : ModOutfits)
        if (outfit.id == id) return &outfit;
    return nullptr;
}

static const char* SkinInternalName(int id) {
    const auto* outfit = FindModOutfit(id);
    return outfit ? outfit->internal : GetInternalNameN(id);
}

static bool OutfitFileExists(const char* path) {
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static void EnsureSkinOrder() {
    if (g_skinCount) return;
    int ids[MaxNativeOutfits], n = 0, dress = 0, rayne = 0, k = 0, i;
    __try {
        for (int id = 1; id <= MaxNativeOutfits; ++id) {
            const char* nm = GetInternalNameN(id);
            if (!nm || !nm[0]) continue;
            ids[n++] = id;
            if (!strcmp(nm, "rayne_dress")) dress = id;
            else if (!strcmp(nm, "rayne")) rayne = id;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!n) return;                    // table not populated yet; retry later
    g_skinOrder[k++] = 0;
    if (dress) g_skinOrder[k++] = dress;
    if (rayne) g_skinOrder[k++] = rayne;
    for (i = 0; i < n; ++i)
        if (ids[i] != dress && ids[i] != rayne) g_skinOrder[k++] = ids[i];
    for (const auto& outfit : ModOutfits) {
        if (OutfitFileExists(outfit.modelFile) &&
            (!outfit.skeletonFile || OutfitFileExists(outfit.skeletonFile)) &&
            (!outfit.physicsFile || OutfitFileExists(outfit.physicsFile)))
            g_skinOrder[k++] = outfit.id;
    }
    g_skinCount = k;
    if (_stricmp(g_skinIni, "default")) {
        __try {
            for (i = 1; i < k; ++i) {
                const char* nm = SkinInternalName(g_skinOrder[i]);
                if (nm && !_stricmp(nm, g_skinIni)) { g_skinIdx = i; break; }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_skinId = g_skinOrder[g_skinIdx];
}

// +0x3CC is the live outfit field (the engine polls it and hot-swaps models),
// so a preset must be WRITTEN there to apply. We track what the game itself
// last put there (anything but our own writes) so "Default" can restore the
// real native pick once and then stay a pure no-op.
static void ApplySkinForce() {
    DWORD gs;
    EnsureSkinOrder();
    gs = *(DWORD*)G_GAME_STATE;
    if (!gs) return;
    if (g_skinId) {
        int cur = *(int*)(gs + 0x3CC);
        if (cur != g_lastWrote) g_nativeOutfit = cur;   // fresh game-side pick
        g_lastWrote = g_skinId;
        *(int*)(gs + 0x3CC) = g_skinId;
    } else if (g_lastWrote >= 0) {
        *(int*)(gs + 0x3CC) = g_nativeOutfit >= 0 ? g_nativeOutfit : 0;
        g_lastWrote = g_nativeOutfit = -1;
    }
}

// Call-site detour target: keep +0x3CC forced moments before the index read.
static unsigned int __cdecl ForceOutfitIndex() {
    ApplySkinForce();
    if (FindModOutfit(g_skinId)) return 0; // use Rayne's equipment and effects
    return ((GetOutfitIndex_t)OUTFIT_INDEX_FN)();
}

static const char* __cdecl GetPlayerOutfitModel() {
    const auto* outfit = FindModOutfit(g_skinId);
    return outfit ? outfit->model : ((const char* (__cdecl*)())0x4F69B0)();
}

static const char* __cdecl GetPlayerOutfitPhysics() {
    const auto* outfit = FindModOutfit(g_skinId);
    return outfit ? outfit->physics : ((const char* (__cdecl*)())0x4F69C0)();
}

typedef const char* (__cdecl *Localize_t)(const char*);
static Localize_t Localize = (Localize_t)FN_LOCALIZE;
static const char* ModLocalize(const char* key, bool useNativeTable = true);

// Display label for one entry of g_skinOrder (Default/native/mod outfit),
// with the mod outfit's numeric suffix appended when present.
static void SkinDisplayName(int id, char* out, int outSize) {
    const auto* outfit = FindModOutfit(id);
    const char* name;
    if (outfit) {
        // Custom labels use only the mod's translations, away from native UI keys.
        name = ModLocalize(outfit->label, false);
    } else if (id) {
        const char* raw = GetDisplayNameN(id);
        const char* loc = raw ? Localize(raw) : 0;
        name = loc ? loc : raw;
    } else {
        name = ModLocalize("Default");
    }
    _snprintf(out, outSize - 1, "%s%s", name ? name : "?",
              outfit && outfit->labelSuffix ? outfit->labelSuffix : "");
    out[outSize - 1] = 0;
}

// Apply g_skinOrder[i] as the current skin and persist the pick.
static void ApplySkinIndex(int i) {
    const char* nm;
    g_skinIdx = i;
    g_skinId = g_skinOrder[i];
    nm = g_skinId ? SkinInternalName(g_skinId) : "default";
    strncpy(g_skinIni, nm, sizeof g_skinIni - 1);
    g_skinIni[sizeof g_skinIni - 1] = 0;
    InterlockedExchange(&g_skinDirty, 1);
    ApplySkinForce();               // field write -> engine hot-swaps the model
}
// `call GetOutfitIndex` sites inside the path getters (Dfm/Gun/Jug/Blade/
// Harpoon/InternalName/DisplayName-current variants).
static const DWORD OUTFIT_INDEX_CALLS[] = {
    0x4F69A0,0x4F69B0,0x4F69C0,0x4F69D0,0x4F6A20,0x4F6A70,0x4F6AC0
};

struct Entry { const char* title; const char* lvl; };
// Two arenas, five counts, guns on/off. lvls[0]=no guns, lvls[1]=guns.
// Stock levels (battle_*/spawn_*) load straight from the PODs; chb_* are
// generated copies (count patches, multigun inject/relocate, retitled
// scripts) produced by tools/make_modes.py.
#define NUM_CNT 5
static const int COUNTS[NUM_CNT] = { 15, 25, 50, 100, 250 };
typedef struct { const char* name; const char* lvls[2][NUM_CNT]; } Arena;
static const Arena ARENAS[] = {
    { "Mansion Battle", {{
        "a1_mansion_part1_battle_easy.lvl",
        "a1_mansion_part1_battle_normal.lvl",
        "a1_mansion_part1_battle_hard.lvl",
        "a1_mansion_part1_battle_100.lvl",
        "a1_mansion_part1_battle_250.lvl" },
      { "a1_mansion_part1_battle_15g.lvl", "a1_mansion_part1_battle_25g.lvl",
        "a1_mansion_part1_battle_50g.lvl", "a1_mansion_part1_battle_100g.lvl",
        "a1_mansion_part1_battle_250g.lvl" }} },
    { "Mod Horde", {{
        "chb_hor_15.lvl", "chb_hor_25.lvl", "chb_hor_50.lvl",
        "chb_hor_100.lvl", "chb_hor_250.lvl" },
      { "chb_hor_15g.lvl", "chb_hor_25g.lvl", "chb_hor_50g.lvl",
        "chb_hor_100g.lvl", "chb_hor_250g.lvl" }} },
};
#define NUM_ARENAS (sizeof(ARENAS)/sizeof(ARENAS[0]))
// Flat level list for best-time bookkeeping, built once at init from
// ARENAS x guns x counts so the tables cannot diverge.
#define MAX_TRACKS 64
static const char* TRACK_LVLS[MAX_TRACKS];
static int NUM_TRACKS;
static void BuildTrackList() {
    int i, j, g;
    NUM_TRACKS = 0;
    for (i = 0; i < (int)NUM_ARENAS; ++i)
        for (g = 0; g < 2; ++g)
            for (j = 0; j < NUM_CNT; ++j)
                if (ARENAS[i].lvls[g][j] && NUM_TRACKS < MAX_TRACKS)
                    TRACK_LVLS[NUM_TRACKS++] = ARENAS[i].lvls[g][j];
}
static const char* LevelBasename(const char* lvl) {
    const char* base = lvl;
    for (const char* p = lvl; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    return base;
}
// TRACK_LVLS index for a level file name, or -1.
static int TrackIndexOf(const char* lvl) {
    const char* base = LevelBasename(lvl);
    for (int i = 0; i < NUM_TRACKS; ++i)
        if (!_stricmp(base, LevelBasename(TRACK_LVLS[i]))) return i;
    return -1;
}
static const Entry EXTRA[] = {
    { "Story Mode",       "hub5.lvl" },
    { "Combat Mode",      "0combat.lvl" },
    { "Character Museum", "charactermuseum.lvl" },
    { "Suicide Run",      "arena_suicide_run.lvl" },
    { "Testing Level",    "0walkto.lvl" },
};

#include "br2diag.h"
#include "challenge_policy.h"
#include "objective_progress.h"

static void __cdecl RestoreMenuFont() {
    // The title font is shared by gameplay messages, loading screens and UI.
    // Reproduction snapshots show identical glyphs/menus but scale 1 -> 5.85
    // and line height 28 -> 163 after a challenge. Reset the actual font before
    // ANY menu items measure text; the native builder resets only UI globals.
    if (!*(DWORD*)G_MENU_STATE) return; // gameplay/level-load builder, not UI
    void* font = *(void**)0x7E6438;
    if (font) {
        typedef void (__thiscall *SetFontScale_t)(void*,float);
        ((SetFontScale_t)0x4F9690)(font,1.0f);
    }
}

// Original builder entry: push ebp; mov ebp,esp; push -1; push SEH handler.
// Reset the shared font before native builders cache any text measurements.
static void __declspec(naked) HookMenuBegin() {
    __asm { push ebp }
    __asm { mov ebp,esp }
    BR2_SAVE_CPU
    __asm { call RestoreMenuFont }
    BR2_RESTORE_CPU
    __asm {
        push -1
        push 0x6D27F5
        push 0x55EB1A
        ret
    }
}

// Diagnostic: log who called MenuBack (the native pop). The patched prologue
// lands us with [esp] = caller return address before we touch the stack.
static void __cdecl LogMenuBackCaller(DWORD ret) {
    Trace("MENUBACK caller=%08lX state=%lu depth=%lu sel=%lu",
          ret, ReadWord(G_MENU_STATE), ReadWord(0x5EF48AC), ReadWord(G_SELIDX));
}
static void __declspec(naked) HookMenuBack() {
    __asm {
        mov eax, dword ptr [esp]
        pushad
        push eax
        call LogMenuBackCaller
        pop eax
        popad
        mov eax, dword ptr ds:[0x5EF48AC]   // ds: forces memory, not imm
        cmp eax, 1
        push 0x559F18
        ret
    }
}

static bool g_challengeSession;
static bool g_photoBlockingMenu;
static int __cdecl FinishStandaloneSession() {
    if (!g_challengeSession) return 0;
    const char* current = (const char*)0x5E33258;
    if (!IsChallengeMenuBridge(current) &&
        !(IsStandaloneChallengeLevel(current) && IsChallengeMenuBridge((const char*)0x6030408))) return 0;
    // Same "stop gameplay" result as the accepted pause-menu exit in
    // 0x4F7969. Let the outer native loop unload and rebuild the real menu.
    *(char*)0x6030408 = 0;
    *(char*)0x5E335BD = 0;
    g_challengeSession = false;
    return 1;
}
// Native race fix: the per-frame tracked-target updater at 0x66EE90 reloads
// [ebx+0x8344] after the target's own vfunc (call eax @0x66EFDC) may have
// destroyed the actor and cleared the slot -> the position getter 0x56A170 is
// then called on ecx=0 (Suicide Run crash @0x56A176). Guard the getter entry:
// a null object writes a zero vec to the out param instead of dereferencing.
static void __declspec(naked) HookGetBonePos() {
    __asm {
        test ecx, ecx
        jnz ok
        mov eax, dword ptr [esp + 4]
        xor edx, edx
        mov dword ptr [eax], edx
        mov dword ptr [eax + 4], edx
        mov dword ptr [eax + 8], edx
        ret 4
    ok:
        push ebp
        mov ebp, esp
        mov eax, dword ptr [ebp + 8]
        push 0x56A176
        ret
    }
}

static void __declspec(naked) HookChallengeExit() {
    __asm {
        cmp g_photoBlockingMenu, 0
        je normal
        mov eax, 1
        ret
    normal:
    }
    BR2_SAVE_CPU
    __asm { call FinishStandaloneSession }
    __asm { mov [esp+556],eax }
    BR2_RESTORE_CPU
    __asm {
        test eax,eax
        jz native
        xor eax,eax
        ret
    native:
        push 0x4F7810
        ret
    }
}
static int __cdecl SuppressStandaloneSavePrompt() {
    const char* level = (const char*)0x5E33258;
    if (IsStandaloneChallengeLevel(level)) {
        g_challengeSession = true;
        return 1;
    }
    if (IsChallengeMenuBridge(level)) return g_challengeSession ? 1 : 0;
    g_challengeSession = false; // campaign/Extras always keep the native prompt
    return 0;
}

// ------------------------- kill counter -------------------------------------

// Fallback localization for the mod's own keys. The game's msglist is loaded
// once at boot from world\<lang>\msglist.txt -- but the loose file sometimes
// loses the open race to the POD's stock copy, leaving a stock table without
// our keys. When Localize misses we serve our own strings in the language of
// the ACTUALLY loaded table, fingerprinted through the stock key "Enemies"
// (distinct value in every language). The old 0x7BF14C read was unreliable.
// Column order: EN, FR, IT, ES, DE, JA(unused->0), RU
typedef struct { const char* key; const char* t[7]; } ModLoc;
static const ModLoc MODL[] = {
 {"Combat Effects", {"Combat Effects","Effets de combat","Effetti di combattimento","Efectos de combate","Kampfeffekte",0,"\xC1\xEE\xE5\xE2\xFB\xE5 \xFD\xF4\xF4\xE5\xEA\xF2\xFB"}},
 {"Finisher Cinematics", {"Finisher Cinematics","Effets de finition","Effetti delle mosse finali","Cinem\xE1ticas de remate","Finisher-Effekte",0,"\xDD\xF4\xF4\xE5\xEA\xF2\xFB \xE4\xEE\xE1\xE8\xE2\xE0\xED\xE8\xE9"}},
 {"Impact Pause", {"Impact Pause","Pause \xE0 l'impact","Pausa all'impatto","Pausa de impacto","Trefferpause",0,"\xCF\xE0\xF3\xE7\xE0 \xEF\xF0\xE8 \xF3\xE4\xE0\xF0\xE5"}},
 {"Mod Parry", {"Parry","Parade","Parata","Parry","Parade",0,"\xCF\xE0\xF0\xE8\xF0\xEE\xE2\xE0\xED\xE8\xE5"}},
 {"Dodge Offset", {"Dodge Offset","Combo apr\xE8s esquive","Combo dopo schivata","Dodge Offset","Dodge Offset",0,"\xCA\xEE\xEC\xE1\xEE \xEF\xEE\xF1\xEB\xE5 \xF3\xEA\xEB\xEE\xED\xE5\xED\xE8\xFF"}},
 {"Style and Combo", {"Style and Combo","Style et combo","Stile e combo","Estilo y combo","Stil und Kombo",0,"\xD1\xF2\xE8\xEB\xFC \xE8 \xEA\xEE\xEC\xE1\xEE"}},
 {"Challenge Mode", {"Challenge Mode","Mode d\xE9""fi","Modalit\xE0 sfida","Modo desaf\xEDo","Herausforderung",0,"\xD0\xE5\xE6\xE8\xEC \xE8\xF1\xEF\xFB\xF2\xE0\xED\xE8\xE9"}},
 {"Back to Main Menu", {"Back to Main Menu","Retour au menu principal","Torna al menu principale","Volver al men\xFA principal","Zur\xFC""ck zum Hauptmen\xFC",0,"\xCD\xE0\xE7\xE0\xE4 \xE2 \xE3\xEB\xE0\xE2\xED\xEE\xE5 \xEC\xE5\xED\xFE"}},
 {"Best Time", {"Best Time","Meilleur temps","Miglior tempo","Mejor tiempo","Bestzeit",0,"\xCB\xF3\xF7\xF8\xE5\xE5 \xE2\xF0\xE5\xEC\xFF"}},
 {"Time", {"Time","Temps","Tempo","Tiempo","Zeit",0,"\xC2\xF0\xE5\xEC\xFF"}},
 {"Peak Style", {"Peak style","Meilleur style","Stile max","Mejor estilo","Bester Stil",0,"\xCB\xF3\xF7\xF8\xE8\xE9 \xF1\xF2\xE8\xEB\xFC"}},
 {"New Record!", {"New Record!","Nouveau record !","Nuovo record!","\xA1Nuevo r\xE9""cord!","Neuer Rekord!",0,"\xCD\xEE\xE2\xFB\xE9 \xF0\xE5\xEA\xEE\xF0\xE4!"}},
 {"Story Mode", {"Story Mode","Mode histoire","Modalit\xE0 storia","Modo historia","Story-Modus",0,"\xD1\xFE\xE6\xE5\xF2\xED\xFB\xE9 \xF0\xE5\xE6\xE8\xEC"}},
 {"Combat Mode", {"Combat Mode","Mode combat","Modalit\xE0 combattimento","Modo combate","Kampfmodus",0,"\xC1\xEE\xE5\xE2\xEE\xE9 \xF0\xE5\xE6\xE8\xEC"}},
 {"Character Museum", {"Character Museum","Mus\xE9""e des personnages","Museo dei personaggi","Museo de personajes","Charakter-Museum",0,"\xCC\xF3\xE7\xE5\xE9 \xEF\xE5\xF0\xF1\xEE\xED\xE0\xE6\xE5\xE9"}},
 {"Suicide Run", {"Suicide Run","Course suicide","Corsa suicida","Carrera suicida","Selbstmord-Lauf",0,"\xD1\xE0\xEC\xEE\xF3\xE1\xE8\xE9\xF1\xF2\xE2\xE5\xED\xED\xFB\xE9 \xE7\xE0\xE1\xE5\xE3"}},
 {"Testing Level", {"Testing Level","Niveau de test","Livello di test","Nivel de pruebas","Test-Level",0,"\xD2\xE5\xF1\xF2\xEE\xE2\xFB\xE9 \xF3\xF0\xEE\xE2\xE5\xED\xFC"}},
 {"Difficulty", {"Difficulty","Difficult\xE9","Difficolt\xE0","Dificultad","Schwierigkeit",0,"\xD1\xEB\xEE\xE6\xED\xEE\xF1\xF2\xFC"}},
 {"Combat", {"Combat","Combat","Combattimento","Combate","Kampf",0,"\xC1\xEE\xE9"}},
 {"Combat Normal", {"Normal","Normal","Normale","Normal","Normal",0,"\xCE\xE1\xFB\xF7\xED\xFB\xE9"}},
 {"Combat Enhanced", {"Enhanced","Am\xE9lior\xE9","Migliorato","Mejorado","Erweitert",0,"\xD3\xEB\xF3\xF7\xF8\xE5\xED\xED\xFB\xE9"}},
 {"Unraveller", {"Unraveller","Le Corrupteur","Unraveller","Desmembrador","Filetierer",0,"\xC2\xEE\xED\xFE\xEA"}},
 {"Custom", {"Custom","Personnalis\xE9","Personalizzato","Personalizado","Benutzerdefiniert",0,"\xCF\xEE\xEB\xFC\xE7\xEE\xE2\xE0\xF2\xE5\xEB\xFC\xF1\xEA\xE8\xE9"}},
 {"Kills", {"Kills","Victimes","Uccisioni","Bajas","Absch\xFCsse",0,"\xD3\xE1\xE8\xE9\xF1\xF2\xE2"}},
 {"Challenge Completed", {"Challenge Completed","D\xE9""fi termin\xE9","Sfida completata","Desaf\xEDo completado","Herausforderung gemeistert",0,"\xC8\xF1\xEF\xFB\xF2\xE0\xED\xE8\xE5 \xEF\xF0\xEE\xE9\xE4\xE5\xED\xEE"}},
 {"Enemies", {"Enemies","Ennemis","Nemici","Enemigos","Gegner",0,"\xC2\xF0\xE0\xE3\xE8"}},
 {"Mod Guns", {"Guns","Armes \xE0 feu","Pistole","Pistolas","Schusswaffen",0,"\xCF\xE8\xF1\xF2\xEE\xEB\xE5\xF2\xFB"}},
 {"Challenge Bars", {"Health and Rage","Vie et rage","Vita e furia","Vida y furia","Leben und Wut",0,"\xC7\xE4\xEE\xF0\xEE\xE2\xFC\xE5 \xE8 \xFF\xF0\xEE\xF1\xF2\xFC"}},
 {"Challenge Bars Base", {"Base","Base","Base","Base","Basis",0,"\xC1\xE0\xE7\xE0"}},
 {"Challenge Bars Maximum", {"Maximum","Maximales","Massime","M\xE1ximas","Maximal",0,"\xCC\xE0\xEA\xF1\xE8\xEC\xF3\xEC"}},
 {"Yes", {"Yes","Oui","S\xEC","S\xED","Ja",0,"\xC4\xE0"}},
 {"No", {"No","Non","No","No","Nein",0,"\xCD\xE5\xF2"}},
 {"Mod Horde", {"Horde Mode","Mode horde","Modalit\xE0 orda","Modo horda","Horden-Modus",0,"\xD0\xE5\xE6\xE8\xEC \xEE\xF0\xE4\xFB"}},
 {"Mansion Battle", {"Mansion Battle","Bataille du manoir","Battaglia nella villa","Batalla de la mansi\xF3n","Villen-Kampf",0,"\xC1\xE8\xF2\xE2\xE0 \xE2 \xEE\xF1\xEE\xE1\xED\xFF\xEA\xE5"}},
 {"Skin", {"Skin","Apparence","Aspetto","Apariencia","Aussehen",0,"\xCE\xE1\xEB\xE8\xEA"}},
 {"Ballroom Guest", {"Ballroom Guest","Invit\xE9""e du bal","Invitata al ballo","Invitada del baile","Ballgast",0,"\xC3\xEE\xF1\xF2\xFC\xFF \xE1\xE0\xEB\xE0"}},
 {"Zerenski Assassin", {"Zerenski Assassin","Tueuse de Zerenski","Sicaria di Zerenski","Sicaria de Zerenski","Zerenskis Attent\xE4terin",0,"\xD3\xE1\xE8\xE9\xF6\xE0 \xC7\xE5\xF0\xE5\xED\xF1\xEA\xE8"}},
 {"Default", {"Default","Par d\xE9""faut","Predefinito","Por defecto","Standard",0,"\xCF\xEE \xF3\xEC\xEE\xEB\xF7\xE0\xED\xE8\xFE"}},
};
// Language id of the loaded msglist (1=EN 2=FR 3=IT 4=ES 5=DE 6=JA 7=RU),
// deduced from the stock key "Enemies" so it always matches the real table.
static int DetectLang(void) {
    const char* e = Localize("Enemies");
    if (!e) return 1;
    if (!strcmp(e, "Ennemis"))  return 2;                     // FR
    if (!strcmp(e, "Nemici"))   return 3;                     // IT
    if (!strcmp(e, "Enemigos")) return 4;                     // ES
    if (!strcmp(e, "Feinde") || !strcmp(e, "Gegner")) return 5; // DE
    if (!strcmp(e, "\xC2\xF0\xE0\xE3\xE8")) return 7;         // RU "Враги" cp1251
    return 1;                                                 // EN/JA/unknown
}
static const char* ModLocalize(const char* key, bool useNativeTable) {
    const char* r = useNativeTable ? Localize(key) : nullptr;
    if (r && strcmp(r, key)) return r;          // table resolved -> use it
    int i, lang = DetectLang();
    for (i = 0; i < (int)(sizeof MODL / sizeof MODL[0]); ++i)
        if (!strcmp(MODL[i].key, key)) {
            const char* s = (lang >= 1 && lang <= 7) ? MODL[i].t[lang - 1] : 0;
            if (!s) s = MODL[i].t[0];           // JA/unknown -> English
            return s;
        }
    return r ? r : key;
}
static int  g_killCount;
static char g_hudLvl[64];
static char g_hudBuf[64];
static char g_hudShown[160];
static char g_killsLbl[40];
static char g_doneLbl[48];
static char g_timeLbl[40];
static char g_recLbl[48];
static char g_doneBuf[160];

enum ChallengeBars { BarsBase, BarsMaximum, BarsCount };
static int g_bars = BarsBase;
static int g_runBars = -1;      // latched on native level load/restart
static int g_runCombat = CombatNormal;
static int g_runDifficulty;
static int g_runStylePeak;
static bool g_runCombatChanged;
static unsigned long long g_runCombatProfile;
static void* g_runSituation;

// ---- best times: .\br2mod_times.ini [Times_v2_<Preset>_Bars<Base|Max>] ----
// Unset entries read back as 0 and render "--". The run clock accumulates the
// engine's OWN scaled world delta at gameState+0x3D8 (slow-mo factor already
// applied), sampled only while HudTick runs in a challenge (menus excluded).
// The old Times_<Preset> sections lack a bars profile and are never read.
static DWORD g_bestTimes[MAX_TRACKS][NUM_PRESETS][BarsCount];
static int   g_timeChal = -1;    // CHALLENGE index whose run is being timed
static float g_runAccum;         // accumulated run seconds (game time)
static int   g_runArmed;         // first enemy emitted -> clock counts
static int   g_runDone;          // completion latched -> no more counting
static int   g_doneStreak;
static bool  g_resultPauseLocked;

static bool __cdecl ChallengeResultsBlockPause(const BYTE* game) {
    const char* level = (const char*)G_CURRENT_LEVEL;
    if (!IsStandaloneChallengeLevel(level)) return false;
    // The script can show its native banner before HudTick latches our result.
    if (!g_resultPauseLocked && *(const float*)(game + 0x4F0) > 0.0f &&
        !strcmp((const char*)(game + 0x3F0), "Challenge Completed"))
        g_resultPauseLocked = true;
    return g_resultPauseLocked || g_runDone;
}

// Esc and Start converge here before the engine changes menu/audio state.
static void __declspec(naked) HookChallengePause() {
    BR2_SAVE_CPU
    __asm {
        push edi
        call ChallengeResultsBlockPause
        add esp, 4
        test al, al
        jnz blocked
    }
    BR2_RESTORE_CPU
    __asm {
        push 0
        push 0x4F7B00
        push 0x561FD0
        ret
    blocked:
    }
    BR2_RESTORE_CPU
    __asm {
        push 0x4F7B68
        ret
    }
}

#include "parry.inl"
#include "enhanced_combat.inl"

static int ChallengeBarsRank(int bars) {
    return bars == BarsMaximum ? 10 : 0;
}

static void __fastcall HookInitChallengeRun(void* game, void*, int flag) {
    ((void (__thiscall*)(void*, int))0x4F5640)(game, flag);
    ResetParry();
    ResetCombatIncoming();
    g_runBars = -1;
    SampleEnhancedSettings();
    g_runCombatProfile = g_combatProfile;
    g_runCombat = g_combat;
    g_runDifficulty = g_diff;
    g_runStylePeak = 0;
    g_runCombatChanged = false;
    g_parryCooling = false;
    g_runSituation = NULL;
    g_timeChal = TrackIndexOf((const char*)G_CURRENT_LEVEL);
    g_hudLvl[0] = 0;
    g_hudShown[0] = 0;
    g_killCount = 0;
    g_runArmed = 0;
    g_runDone = 0;
    g_resultPauseLocked = false;
    g_runAccum = 0.0f;
    g_doneStreak = 0;
    if (g_timeChal < 0) return;

    // Same Rayne resolver as the native level initialization at 0x4F9076.
    typedef BYTE* (__cdecl* ResolveActor)(void*, void*);
    BYTE* rayne = ((ResolveActor)0x4050C0)(*(void**)0x7DCD68, *(void**)0x5EC07B0);
    if (!rayne || !*(void**)G_SITUATION) return;

    // The native update uses capacity = 1000 + 100 * Carnage rank.
    g_runBars = g_bars;
    g_runSituation = *(void**)G_SITUATION;
    const float capacity = 1000.0f + 100.0f * ChallengeBarsRank(g_runBars);
    *(float*)(rayne + 0x39C8) = capacity; // health starts full
    *(float*)(rayne + 0x39CC) = capacity;
    *(float*)(rayne + 0x8360) = 0.0f;    // rage starts empty
    *(float*)(rayne + 0x8364) = capacity;
}

static int __fastcall HookChallengeBarsRank(void* carnage, void*) {
    if (g_runBars >= 0 && g_timeChal >= 0 &&
        g_runSituation == *(void**)G_SITUATION &&
        carnage == *(void**)G_CARNAGE &&
        !_stricmp(LevelBasename((const char*)G_CURRENT_LEVEL),
                  LevelBasename(TRACK_LVLS[g_timeChal])))
        return ChallengeBarsRank(g_runBars);
    return ((int (__thiscall*)(void*))0x4D5A40)(carnage);
}

#define TIMES_FILE ".\\br2mod_times.ini"
static void FormatTime(DWORD ms, char* out) {
    sprintf(out, "%u'%02u'%02u", ms / 60000u,
            (ms / 1000u) % 60u, (ms / 10u) % 100u);
}
static void TimeKey(const char* lvl, char* out) {
    const char* base = lvl;
    const char* p;
    int n = 0;
    for (p = lvl; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    for (; base[n] && base[n] != '.' && n < 63; ++n)
        out[n] = (char)tolower((unsigned char)base[n]);
    out[n] = 0;
}
static void TimeSection(int difficulty, int bars, int combat, unsigned long long profile,
                        char (&out)[96]) {
    const char* capacity = bars == BarsMaximum ? "Max" : "Base";
    if (combat == CombatEnhanced)
        sprintf_s(out, "Times_v4_%s_Bars%s_Enhanced_%016llX", PRESETS[difficulty].name, capacity, profile);
    else
        sprintf_s(out, "Times_v2_%s_Bars%s", PRESETS[difficulty].name, capacity);
}
static void LoadBestTimes() {
    char key[64], sec[96];
    for (int d = 0; d < NUM_PRESETS; ++d) {
        for (int bars = 0; bars < BarsCount; ++bars) {
            TimeSection(d, bars, CombatNormal, 0, sec);
            for (int i = 0; i < NUM_TRACKS; ++i) {
                TimeKey(TRACK_LVLS[i], key);
                g_bestTimes[i][d][bars] = GetPrivateProfileIntA(sec, key, 0, TIMES_FILE);
            }
        }
    }
}
struct ChallengeRecords {
    DWORD bestTime;
    int styleAtBestTime, bestStyle;
    DWORD timeAtBestStyle;
};
enum ChallengeRecordFlags { RecordTime = 1, RecordStyle = 2 };

static ChallengeRecords ReadChallengeRecords(int idx, int difficulty, int bars, int combat,
                                             unsigned long long profile) {
    if (idx < 0 || idx >= NUM_TRACKS) return {};
    if (combat == CombatNormal) return {g_bestTimes[idx][difficulty][bars]};
    char key[64], sec[96];
    TimeKey(TRACK_LVLS[idx], key);
    TimeSection(difficulty, bars, combat, profile, sec);
    char value[64];
    GetPrivateProfileStringA(sec, key, "", value, sizeof value, TIMES_FILE);
    ChallengeRecords records{};
    if (sscanf_s(value, "%lu,%d,%d,%lu", &records.bestTime, &records.styleAtBestTime,
                 &records.bestStyle, &records.timeAtBestStyle) != 4 ||
        records.styleAtBestTime < 0 || records.styleAtBestTime > 999999 ||
        records.bestStyle < records.styleAtBestTime || records.bestStyle > 999999 ||
        (records.bestStyle && !records.timeAtBestStyle)) return {};
    return records;
}

static int SaveChallengeRecords(int idx, DWORD ms, int style) {
    if (idx < 0 || idx >= NUM_TRACKS || g_runBars < 0 || g_runCombatChanged) {
        if (g_runCombatChanged) LogParry("record-profile-changed", ms);
        return 0;
    }
    if (!ms) return 0;
    const bool enhanced = g_runCombat == CombatEnhanced;
    const int difficulty = enhanced ? g_runDifficulty : g_diff;
    auto records = ReadChallengeRecords(idx, difficulty, g_runBars, g_runCombat, g_runCombatProfile);
    int flags = 0;
    if (!records.bestTime || ms < records.bestTime || (ms == records.bestTime && style > records.styleAtBestTime)) {
        records.bestTime = ms;
        records.styleAtBestTime = style;
        flags |= RecordTime;
    }
    if (style > 0 && (style > records.bestStyle ||
        (style == records.bestStyle && ms < records.timeAtBestStyle))) {
        records.bestStyle = style;
        records.timeAtBestStyle = ms;
        flags |= RecordStyle;
    }
    if (!flags) return 0;
    char key[64], val[64], sec[96];
    TimeKey(TRACK_LVLS[idx], key);
    if (enhanced)
        sprintf_s(val, "%lu,%d,%d,%lu", records.bestTime, records.styleAtBestTime,
                  records.bestStyle, records.timeAtBestStyle);
    else
        sprintf_s(val, "%lu", records.bestTime);
    TimeSection(difficulty, g_runBars, g_runCombat, g_runCombatProfile, sec);
    if (!WritePrivateProfileStringA(sec, key, val, TIMES_FILE)) {
        LogParry("record-write-failed", ms, flags, 0, style);
        return 0;
    }
    if (!enhanced) g_bestTimes[idx][difficulty][g_runBars] = ms;
    LogParry("record-saved", ms, flags, 0, style, 0, 0, NULL, NULL, 0,
             (float)records.bestTime, (float)records.bestStyle, (float)records.timeAtBestStyle);
    return flags;
}

static void TrackHudLevel() {
    const char* lvl = (const char*)0x5E33258;
    if (strncmp(lvl, g_hudLvl, sizeof g_hudLvl)) {
        strncpy(g_hudLvl, lvl, sizeof g_hudLvl - 1);
        g_hudLvl[sizeof g_hudLvl - 1] = 0;
        g_killCount = 0;
        g_hudShown[0] = 0;
        if (IsStandaloneChallengeLevel(lvl)) g_challengeSession = true;
        else if (!IsChallengeMenuBridge(lvl)) g_challengeSession = false;
    }
}

// ---- feed-grab diagnostic: [Diagnostics] feedLog=1 --------------------------
// Logs the grabbed victim's anim/orientation candidates every frame so we can
// see exactly when the enemy stops turning relative to Rayne's grab anim.
static volatile unsigned g_feedMotionBlocks;
static int   g_feedLog = -1;
static DWORD g_feedVictim;
static FILE* g_feedFp;
static int   g_feedFrame;
static void FeedDiagTick() {
    DWORD rayne, victim;
    __try { rayne = *(DWORD*)0x61276EC; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (!rayne) return;
    __try { victim = *(DWORD*)(rayne + 0x83C0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    if (g_feedLog < 0)
        g_feedLog = GetPrivateProfileIntA("Diagnostics", "feedLog", 0,
                                          ".\\br2mod.ini");
    if (!g_feedLog) return;
    if (!victim) {
        if (g_feedFp) { fclose(g_feedFp); g_feedFp = NULL; }
        g_feedVictim = 0;
        return;
    }
    if (victim != g_feedVictim) {
        DWORD vtbl = 0;
        __try { vtbl = *(DWORD*)victim; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        if (g_feedFp) fclose(g_feedFp);
        g_feedFp = fopen("br2mod_feed.log", "a");
        g_feedVictim = victim;
        g_feedFrame = 0;
        if (g_feedFp) fprintf(g_feedFp, "victim=%08x vtbl=%08x\n",
                              victim, vtbl);
    }
    if (!g_feedFp) return;
    DWORD ranim, vanim; float f[12], rp[3], rframe, vframe;
    __try {
        ranim = *(DWORD*)(rayne + 0x17C);
        vanim = *(DWORD*)(victim + 0x17C);
        memcpy(f, (void*)(victim + 0x38), 8 * 4);           // +0x38..+0x54
        f[8]  = *(float*)(victim + 0x628);
        f[9]  = *(float*)(victim + 0x39C8);
        f[10] = *(float*)(victim + 0x39CC);
        f[11] = *(float*)(victim + 0x24);
        memcpy(rp,(void*)(rayne+0x38),sizeof rp);
        rframe = *(float*)(rayne+0x180);
        vframe = *(float*)(victim+0x180);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }
    fprintf(g_feedFp,
            "%3d R=%03x V=%03x 38=%.3f 3c=%.3f 40=%.3f 44=%.3f 48=%.3f "
            "4c=%.3f 50=%.3f 54=%.3f 628=%.3f c8=%.4f cc=%.4f 24=%.4f "
            "Rpos=(%.3f,%.3f,%.3f) Rframe=%.3f Vframe=%.3f blocked=%u\n",
            g_feedFrame++, ranim, vanim, f[0], f[1], f[2], f[3], f[4],
            f[5], f[6], f[7], f[8], f[9], f[10], f[11],
            rp[0],rp[1],rp[2],rframe,vframe,g_feedMotionBlocks);
}

// ---- feed wind-up yaw pin ---------------------------------------------------
// While Rayne's grab lunge plays (anims 0xE3/0xE5/0xE7/0xF3) the victim is
// already locked (Rayne+0x83C0) but the "you are grabbed" commit only arrives
// with the drain anim. If the enemy's AI was steering it keeps turning for
// ~50 frames -> the back-feed then plays on an enemy facing Rayne. While the
// victim is targeted and NOT yet in a grabbed anim (0x70-0x85) we pin its yaw
// field (victim+0x48; +0x50 facing is derived from it) so it just stays put.
// Victims already in the grabbed anim keep the native sync alignment.
static DWORD g_pinVictim;
static DWORD g_initVictim;
static DWORD g_seen73;
static int   g_pinActive;
static float g_pinYaw;
typedef void (__thiscall* PlayAnim_t)(void* self, int anim, float f1, float f2);
#define PLAYANIM ((PlayAnim_t)0x658EB0)
// The native grabbed anim the victim should hold per Rayne wind-up anim
// (from live feed logs: f3->0x73, e3->0x42, e5->0x70, e7->0x71).
static int FeedExpectedVictimAnim(DWORD ranim) {
    switch (ranim) {
    case 0xE3: return 0x42;
    case 0xE5: return 0x70;
    case 0xE7: return 0x71;
    case 0xF3: return 0x73;
    }
    return -1;
}

// Preserve a rear-grab clip only AFTER native contact has requested it.
// Replaying 73 at frame zero from the render callback can anticipate contact
// and repeatedly discard root-motion progress while Rayne keeps lunging.
// While the victim holds 0x73 during the F3 wind-up, drop ANY request that is
// not a grabbed-family anim (locomotion AND combat anims alike: 0x08/0x16/
// 0x5A/0x5B/0x41 and 0x11/0x19/0x6A/0x68/0x69/0x6C/0x6E seen in live logs).
static int __cdecl KeepNativeRearFeedPose(DWORD controller,int requested) {
    if ((requested >= 0x70 && requested <= 0x85) || requested == 0x42)
        return 0;                       // grabbed family -> always allow
    __try {
        DWORD rayne = *(DWORD*)0x61276EC;
        if (!rayne || *(DWORD*)(rayne+0x17C)!=0xF3) return 0;
        DWORD victim = *(DWORD*)(rayne+0x83C0);
        if (!victim || controller!=victim+0x158) return 0;
        if (*(DWORD*)(victim+0x17C)!=0x73 || !(*(float*)(victim+0x39C8)>0.0f)) return 0;
        ++g_feedMotionBlocks;
        return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void __declspec(naked) HookFeedMotionRequest() {
    __asm { push ebp }
    __asm { mov ebp,esp }
    BR2_SAVE_CPU
    __asm { push dword ptr [ebp+8] }
    __asm { push ecx }
    __asm { call KeepNativeRearFeedPose }
    __asm { add esp,8 }
    __asm { mov [esp+556],eax }
    BR2_RESTORE_CPU
    __asm {
        test eax,eax
        jz native
        pop ebp
        ret 0xC
    native:
        push esi
        mov esi,ecx
        push 0x609D96
        ret
    }
}
static void FeedYawPinTick() {
    DWORD rayne = 0, victim = 0, ranim = 0, vanim = 0;
    __try {
        rayne = *(DWORD*)0x61276EC;
        if (!rayne) { g_pinVictim = 0; g_pinActive = 0; return; }
        victim = *(DWORD*)(rayne + 0x83C0);
        ranim  = *(DWORD*)(rayne + 0x17C);
        if (victim) vanim = *(DWORD*)(victim + 0x17C);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_pinVictim = 0; g_pinActive = 0; return; }
    int expected = FeedExpectedVictimAnim(ranim);
    if (!victim || expected < 0) { g_pinVictim = 0; g_pinActive = 0; return; }
    if (ranim==0xF3) {
        // Native contact usually requests 0x73 at frame 0 -- but victims in
        // combat anims (0x11/0x19/0x6A/...) are never granted it by the
        // classifier and keep attacking/turning through the whole wind-up.
        // Fire the same request ONCE per victim (re-fire only if it already
        // held 0x73 and was bounced out -- an ineligible victim degrades to
        // native behaviour, never a request spam/restart loop).
        __try {
            int alive = (*(float*)(victim + 0x39C8) > 0.0f);
            if (vanim == 0x73) g_seen73 = victim;
            int kicked = (g_initVictim != victim) || (g_seen73 == victim);
            if (vanim != 0x73 && vanim != 0x74 && alive && kicked) {
                PLAYANIM((void*)victim, 0x73, 0.0f, -1.0f);
                g_initVictim = victim;
                if (victim != g_pinVictim || !g_pinActive) {
                    g_pinVictim = victim;
                    g_pinYaw = *(float*)(victim + 0x48);
                    g_pinActive = 1;
                }
                *(float*)(victim + 0x48) = g_pinYaw;
            } else {
                g_pinActive = 0;   // held: native sync alignment owns yaw
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_pinVictim = 0; g_pinActive = 0; }
        return;
    }
    __try {
        // The victim entered its grabbed anim at contact, but its AI keeps
        // steering until the drain commit and re-issues locomotion anims
        // (0x08/0x16/0x5A/0x5B/0x41...), bouncing it out. Re-request the
        // proper grabbed anim through the engine's own PlayAnim -- it is a
        // no-op while the clip already matches, so this only fires on the
        // frames the AI bounced it. Grabbed anims in the 0x70-0x85 range or
        // 0x42 are left alone (legit commit/other variants).
        int grabbed = (vanim >= 0x70 && vanim <= 0x85) || vanim == 0x42;
        if (!grabbed) {
            PLAYANIM((void*)victim, expected, 0.0f, -1.0f);
            if (victim != g_pinVictim || !g_pinActive) {
                g_pinVictim = victim;
                g_pinYaw = *(float*)(victim + 0x48);
                g_pinActive = 1;
            }
            *(float*)(victim + 0x48) = g_pinYaw;
        } else {
            g_pinActive = 0;   // held anim: native sync alignment owns yaw
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { g_pinVictim = 0; g_pinActive = 0; }
}

// Called immediately BEFORE the native onscreen-text renderer, after scripts
// and the normal timer update. Blank banners cannot overwrite us later in
// the same frame. The script interpreter and the level loader stay native.
static void __cdecl HudTick() {
    if (g_photoBlockingMenu) return;
    TrackHudLevel();
    FeedDiagTick();
    FeedYawPinTick();
    const char* lvl = (const char*)0x5E33258;
    int menuOpen = *(DWORD*)0x5EF4BF8 != 0;   // any menu open -> not in gameplay
    DWORD gso = *(DWORD*)0x7C07F8;
    if (gso && !menuOpen) DrawCombatCombo((BYTE*)gso);
    // Game-time clock: the engine's own scaled world delta (slow-mo factor
    // already applied by the game; pauses/menus are excluded by the gate).
    if (g_runArmed && !g_runDone && !menuOpen && gso &&
        IsStandaloneChallengeLevel(lvl))
        g_runAccum += *(float*)(gso + 0x3D8);
    if (!menuOpen) ApplySkinForce();
    if (!IsStandaloneChallengeLevel(lvl)) return;
    if (menuOpen) return;
    if (!gso) return;
    ChallengeResultsBlockPause((const BYTE*)gso);
    if (g_trigTrace) {
        static DWORD s_lastDump = 0;
        DWORD now = GetTickCount();
        if (now - s_lastDump > 2000) {
            s_lastDump = now;
            if (!g_trigLog)
                fopen_s(&g_trigLog, "br2mod_trig.log", "w");
            if (g_trigLog) {
                fprintf(g_trigLog, "--- lvl=%s\n", lvl);
                DumpObjectiveTriggers(g_trigLog);
            }
        }
    }
    ObjectiveProgress progress;
    if (!ReadObjectiveProgress(&progress))
        return; // no authoritative wave yet: do not display an estimate
    // Fill the HUD labels the moment a live objective exists: the early-outs
    // below (slot sanity / yield-to-script) used to run first, so a run that
    // finished while the script's own display held the slot would build its
    // completion text out of still-empty labels -> "- : 0'53'96 -".
    if (!g_killsLbl[0]) {
        const char* label = ModLocalize("Kills");
        strncpy(g_killsLbl, (label && label[0]) ? label : "Kills",
                sizeof g_killsLbl - 1);
        g_killsLbl[sizeof g_killsLbl - 1] = 0;
        const char* msg = ModLocalize("Challenge Completed");
        strncpy(g_doneLbl, (msg && msg[0]) ? msg : "Challenge Completed",
                sizeof g_doneLbl - 1);
        g_doneLbl[sizeof g_doneLbl - 1] = 0;
        const char* tl = ModLocalize("Time");
        strncpy(g_timeLbl, (tl && tl[0]) ? tl : "Time", sizeof g_timeLbl - 1);
        g_timeLbl[sizeof g_timeLbl - 1] = 0;
        const char* rl = ModLocalize("New Record!");
        strncpy(g_recLbl, (rl && rl[0]) ? rl : "New Record!",
                sizeof g_recLbl - 1);
        g_recLbl[sizeof g_recLbl - 1] = 0;
    }
    if (!g_runArmed && progress.spawned > 0)
        g_runArmed = 1;              // combat actually begins with first spawn
    g_killCount = progress.spawned - progress.alive;
    if (g_killCount<0) g_killCount = 0;
    // The wave objective is authoritative: all emitted kills accounted for and
    // nobody left alive = done. Then we own the slot and repaint the localized
    // completion text, overwriting the script's literal English banner.
    // Between waves the trigger briefly reports spawned>=target with alive==0
    // before the next batch is emitted -> "done" flickers for 1-2 frames.
    // Require a sustained completion (~0.75s) before owning the slot.
    bool rawDone = progress.target > 0 && progress.spawned >= progress.target
                && progress.alive == 0;
    g_doneStreak = rawDone ? g_doneStreak + 1 : 0;
    bool done = g_doneStreak >= 45;
    if (done && g_runArmed && !g_runDone) {
        DWORD ms = (DWORD)(g_runAccum * 1000.0f);
        char tb[16];
        SampleEnhancedSettings();
        g_runDone = 1;
        const bool showStyle = g_runCombat == CombatEnhanced && g_enhanced.styleEnabled;
        const int style = showStyle ? g_runStylePeak : 0;
        const int records = SaveChallengeRecords(g_timeChal, ms, style);
        FormatTime(ms, tb);
        LogParry("challenge-result", ms, records, 0, style, 0, 0, NULL, NULL, 0,
                 g_runCombatChanged ? 1.0f : 0.0f);
        if (showStyle)
            sprintf_s(g_doneBuf, "%s - %s: %s - %s: %d (%s)%s%s", g_doneLbl, g_timeLbl, tb,
                      ModLocalize("Peak Style"), style, StyleGrade(style), records ? " - " : "",
                      records ? g_recLbl : "");
        else
            sprintf_s(g_doneBuf, "%s - %s: %s%s%s", g_doneLbl, g_timeLbl, tb,
                      records ? " - " : "", records ? g_recLbl : "");
    }
    if (done && !g_runDone) g_doneBuf[0] = 0; // not armed this run
    // Yield the slot to script text: if the slot holds something we did not
    // write and it is still alive, leave it be -- except when done, where our
    // localized message replaces whatever the script displayed.
    // All-space strings are our masked-out banners -> treated as empty.
    const char* slot = (const char*)(gso + 0x3F0);
    float     dur  = *(float*)(gso + 0x4F0);
    if (dur < 0.0f || dur > 60.0f) return;    // slot sanity (transitions)
    if (!done && !g_runArmed && slot[0] && dur > 0.0f
        && strcmp(slot, g_hudShown)) {
        const char* p = slot;
        while (*p == ' ') ++p;
        if (*p) return;                     // real foreign text -> yield
    }
    const char* text = g_hudBuf;
    if (done) text = g_doneBuf[0] ? g_doneBuf : g_doneLbl;
    else if (progress.target > 0)
        sprintf(g_hudBuf, "%s: %d/%d", g_killsLbl, g_killCount, progress.target);
    else sprintf(g_hudBuf, "%s: %d", g_killsLbl, g_killCount);
    __asm {
        mov  ecx, gso
        push 0
        push 0
        push 0
        push 0x3F800000                      // f2 = 1.0f
        push 0xBF800000                      // duration = -1.0f (auto)
        mov  eax, text
        push eax
        mov  eax, 0x4F4560                   // native onscreen-text setter
        call eax
    }
    strcpy(g_hudShown, text);
}
// CALL detour at 0x4F83A9: preserves the caller's ECX/game-state and CPU state,
// then tail-enters the original renderer with its original return address.
// Never patch 0x4F93B0..BB: the loader jumps BACK to 0x4F93B5 internally.
static void __declspec(naked) HookHudRender() {
    BR2_SAVE_CPU
    __asm { call HudTick }
    BR2_RESTORE_CPU
    __asm {
        push 0x4F71C0
        ret
    }
}

// NOTE: display() passes its argument literally -- the interpreter path
// 0x4CD88D..0x4CD946 contains no localization lookup at all, so
// display(Challenge Completed) always renders English. A detour on the
// setter call proved unstable (AV at 0x639587 inside the setter's
// whitespace skipper); left native until a safe site is proven.

// 0x566430 is only the automatic "Save Game?" prompt. The native routine
// checks saved transition health then calls CallMenu(0x0B); it has no other
// state changes. For challenges return before that check, like its no-prompt
// branch. Campaign levels replay the original x87 comparison verbatim.
static void __declspec(naked) HookSavePrompt() {
    BR2_SAVE_CPU
    __asm { call SuppressStandaloneSavePrompt }
    __asm { mov [esp+556],eax } // saved EAX: 528-byte area + pushad slot 28
    BR2_RESTORE_CPU
    __asm {
        test eax,eax
        jz campaign
        ret
    campaign:
        fldz
        fcomp dword ptr ds:[0x60302B4]
        push 0x566438
        ret
    }
}

// ------------------------- game call helpers -------------------------------
typedef void (__cdecl *CallMenu_t)(int);
typedef void (__cdecl *MenuBack_t)(void);
static CallMenu_t CallMenu = (CallMenu_t)FN_CALL_MENU;
static MenuBack_t MenuBack = (MenuBack_t)FN_MENU_BACK;

// Game ABI: ecx/edi=display string, eax=action code, esi=secondary
// string -> item+0x54 (for level items = the .lvl filename! The native
// launch path stricmp/reads it; NULL crashes). `call` para restaurar esi.
static void __declspec(naked) AddMenuItem(const char* text, int action,
                                        const char* text2) {
    __asm {
        push esi
        mov  ecx, [esp + 8]
        mov  eax, [esp + 12]
        mov  esi, [esp + 16]
        mov  edx, FN_ADD_MENU_ITEM
        call edx
        pop  esi
        ret
    }
}

#include "photo_mode.inl"

// ------------------------- menu build logic --------------------------------
static void BuildChallengeMenu();
static void __cdecl OnMenuBuilt() {
    TraceGlobals("menu-before-injection");
    int id = *(int*)G_MENU_STATE;
    if (id) {
        SampleEnhancedSettings();
        ResetParry();
    }
    unsigned i;
    switch (id) {
    case MENU_PAUSE:
        AddPhotoMenuItem();
        break;
    case MENU_MAIN: {
        AddMenuItem(ModLocalize("Challenge Mode"), AC_CHALLENGE_MENU, 0);
        // mover nuestro item ANTES de "Extras": items [NG,Load,Opt,Ext,Quit,CH]
        // -> shift [n-3..n-2] (Extras,Quit) una posicion abajo, CH a n-3
        {
            int n = *(int*)G_ITEM_COUNT;
            if (n >= 3) {
                char tmp[0xD8];
                char* arr = (char*)G_ITEM_ARRAY;
                memcpy(tmp, arr + (n - 1) * 0xD8, 0xD8);
                memmove(arr + (n - 2) * 0xD8, arr + (n - 3) * 0xD8, 2 * 0xD8);
                memcpy(arr + (n - 3) * 0xD8, tmp, 0xD8);
            }
        }
        break;
    }
    case MENU_EXTRAS:
        for (i = 0; i < sizeof(EXTRA)/sizeof(EXTRA[0]); ++i)
            AddMenuItem(ModLocalize(EXTRA[i].title), AC_EXTRA_BASE + i, EXTRA[i].lvl);
        break;
    case MENU_OPTIONS: {
        static char combat[64];
        sprintf(combat, "%s: %s", ModLocalize("Combat"),
                ModLocalize(g_combat == CombatEnhanced ? "Combat Enhanced" : "Combat Normal"));
        AddMenuItem(combat, AC_COMBAT, 0);
        if (g_combat == CombatEnhanced)
            AddMenuItem(ModLocalize("Combat Effects"), AC_EFFECTS_MENU, 0);
        static char buf[64];
        // "Dhampir" is a proper name: show it verbatim in every language
        // (the native ES string is "Dhampir:" which would read wrong here).
        const char* pname = !_stricmp(PRESETS[g_diff].name, "Dhampir")
                            ? "Dhampir" : ModLocalize(PRESETS[g_diff].name);
        sprintf(buf, "%s: %s", ModLocalize("Difficulty"), pname);
        AddMenuItem(buf, AC_DIFFICULTY, 0);
        EnsureSkinOrder();
        {
            static char sbuf[96];
            char name[72];
            SkinDisplayName(g_skinId, name, sizeof name);
            _snprintf(sbuf, sizeof sbuf - 1, "%s: %s", ModLocalize("Skin"), name);
            sbuf[sizeof sbuf - 1] = 0;
            AddMenuItem(sbuf, AC_SKIN, 0);
        }
        break;
    }
    case MENU_SKINS: {
        // Menu 0x33 rebuilds every frame (native dynamic-menu quirk, like
        // cheats 0x35): never touch G_SELIDX here or the cursor snaps back.
        strcpy_s((char*)0x5EEDAC8, 72, ModLocalize("Skin"));
        *(unsigned char*)G_MENU_REBUILD = 1; // triggers centered-panel layout
        *(int*)0x5EF5640 = 2;
        *(int*)0x5EF5638 = 7;      // visible rows, same as native list menus
        EnsureSkinOrder();
        for (int i = 0; i < g_skinCount; ++i) {
            char name[72], row[80];
            SkinDisplayName(g_skinOrder[i], name, sizeof name);
            _snprintf(row, sizeof row - 1, "%s%s", name,
                      i == g_skinIdx ? "  *" : "");
            row[sizeof row - 1] = 0;
            AddMenuItem(row, AC_SKIN_PICK_BASE + i, 0);
        }
        AddMenuItem(ModLocalize("Back"), AC_BACK, 0);
        if (g_skinsNeedSelect) {   // one-shot: land cursor on current skin
            g_skinsNeedSelect = 0;
            *(int*)G_SELIDX = g_skinIdx;
        }
        break;
    }
    case MENU_CHALLENGE:
        BuildChallengeMenu();
        break;
    case MENU_EFFECTS: {
        // The generic builder clears the panel flag. Restore it on every
        // build so the renderer lays out the title and rows at screen center.
        strcpy_s((char*)0x5EEDAC8, 72, ModLocalize("Combat Effects"));
        *(unsigned char*)G_MENU_REBUILD = 1; // native centered panel at 0x563E90
        *(int*)0x5EF5640 = 2;
        for (int effect = 0; effect < EffectCount; ++effect) {
            char label[72];
            sprintf_s(label, "%s: %s", ModLocalize(COMBAT_EFFECTS[effect].label),
                      ModLocalize(CombatEffectEnabled((CombatEffect)effect) ? "On" : "Off"));
            AddMenuItem(label, AC_EFFECT_BASE + effect, 0);
        }
        AddMenuItem(ModLocalize("Back"), AC_BACK, 0);
        break;
    }
    }
    TraceGlobals("menu-after-injection");
}

// The native builder prologue runs even for our empty jump-table slot.
// Fill rows only from its epilogue; CallMenu owns selection/stack changes.
static int g_countSel = 1;      // COUNTS index shown on the cycler (25 default)
static int g_guns = 0;          // 0 = blades only, 1 = bloodguns too
static volatile LONG g_chalDirty = 0;  // [Challenge] options need flushing

static void FormatBest(DWORD best, char* tb) {
    if (best) sprintf(tb, "%u'%02u'%02u", best / 60000u,
                      (best / 1000u) % 60u, (best / 10u) % 100u);
    else strcpy(tb, "--");
}

static void BuildChallengeMenu() {
    int i;
    *(int*)G_ITEM_COUNT = 0;
    // Label buffer inside a menu item is 72 bytes: keep texts under it.
    char lbl[72], tb[16];
    _snprintf(lbl, sizeof lbl - 1, "%s: %d",
              ModLocalize("Enemies"), COUNTS[g_countSel]);
    lbl[sizeof lbl - 1] = 0;
    AddMenuItem(lbl, AC_COUNT_CYCLE, 0);
    _snprintf(lbl, sizeof lbl - 1, "%s: %s",
              ModLocalize("Mod Guns"),
              ModLocalize(g_guns ? "Yes" : "No"));
    lbl[sizeof lbl - 1] = 0;
    AddMenuItem(lbl, AC_GUNS_CYCLE, 0);
    _snprintf(lbl, sizeof lbl - 1, "%s: %s", ModLocalize("Challenge Bars"),
              ModLocalize(g_bars == BarsMaximum ? "Challenge Bars Maximum" : "Challenge Bars Base"));
    lbl[sizeof lbl - 1] = 0;
    AddMenuItem(lbl, AC_BARS_CYCLE, 0);
    // Time first, then style. When they come from different runs, show the
    // paired result in parentheses next to each independent record.
    for (i = 0; i < (int)NUM_ARENAS; ++i) {
        int ti = TrackIndexOf(ARENAS[i].lvls[g_guns][g_countSel]);
        const auto records = ReadChallengeRecords(ti, g_diff, g_bars, g_combat, g_combatProfile);
        const char* name = ModLocalize(ARENAS[i].name);
        FormatBest(records.bestTime, tb);
        if (g_combat != CombatEnhanced || !g_enhanced.styleEnabled) {
            _snprintf(lbl, sizeof lbl - 1, "%s - %s", name, tb);
        } else if (!records.bestStyle) {
            _snprintf(lbl, sizeof lbl - 1, "%s - %s / --", name, tb);
        } else if (records.styleAtBestTime == records.bestStyle && records.bestTime == records.timeAtBestStyle) {
            _snprintf(lbl, sizeof lbl - 1, "%s - %s / %d(%s)", name, tb, records.bestStyle, StyleGrade(records.bestStyle));
        } else {
            char styleTime[16];
            FormatBest(records.timeAtBestStyle, styleTime);
            _snprintf(lbl, sizeof lbl - 1, "%s - %s(%d) / %d(%s) [%s]", name, tb, records.styleAtBestTime,
                      records.bestStyle, StyleGrade(records.bestStyle), styleTime);
        }
        lbl[sizeof lbl - 1] = 0;
        AddMenuItem(lbl, AC_ARENA_BASE + i, 0);
    }
    AddMenuItem(ModLocalize("Back to Main Menu"), AC_BACK, 0);
}

// ------------------------- action dispatch ---------------------------------
// -1 = not ours | 0 = handled -> jmp 0x56913E | 1 = launch -> jmp 0x56914A
static int __cdecl DispatchOurs(int action) {
    Trace("ACTION code=%X",action);
    TraceGlobals("action");
    if (action == AC_PHOTO) {
        RequestPhotoFromPause();
        return 0;
    }
    if (action == AC_CHALLENGE_MENU) { CallMenu(MENU_CHALLENGE); return 0; }
    if (action == AC_BACK)           { MenuBack();              return 0; }
    if (action == AC_EFFECTS_MENU) {
        if (g_combat == CombatEnhanced && *(int*)G_MENU_STATE == MENU_OPTIONS)
            CallMenu(MENU_EFFECTS);
        return 0;
    }
    if (action >= AC_EFFECT_BASE && action < AC_EFFECT_BASE + EffectCount) {
        if (g_combat == CombatEnhanced && *(int*)G_MENU_STATE == MENU_EFFECTS) {
            const LONG bit = 1L << (action - AC_EFFECT_BASE);
            InterlockedXor(&g_effects, bit);
            InterlockedOr(&g_effectsDirty, bit);
            *(unsigned char*)G_MENU_REBUILD = 1;
        }
        return 0;
    }
    if (action == AC_COMBAT) {
        g_combat = g_combat == CombatNormal ? CombatEnhanced : CombatNormal;
        if (g_runBars >= 0) g_runCombatChanged = true;
        InterlockedExchange(&g_combatDirty, 1);
        *(unsigned char*)G_MENU_REBUILD = 1;
        return 0;
    }
    if (action == AC_DIFFICULTY) {
        if (g_runBars >= 0 && g_runCombat == CombatEnhanced) g_runCombatChanged = true;
        g_diff = (g_diff + 1) % NUM_PRESETS;
        ApplyPreset();
        // Persist lazily: WritePrivateProfileStringA rewrites the whole INI on
        // the game thread -- its first (cold-cache, AV-scanned) write hitched
        // the menu. The Worker's writer loop flushes it off-thread instead.
        InterlockedExchange(&g_presetDirty, 1);
        DWORD gs = *(DWORD*)G_GAME_STATE;
        if (gs) *(int*)(gs + 0x1e8) = g_diff + 1;
        *(unsigned char*)G_MENU_REBUILD = 1;
        return 0;
    }
    if (action == AC_SKIN) {
        EnsureSkinOrder();
        if (g_skinCount > 1 && *(int*)G_MENU_STATE == MENU_OPTIONS) {
            g_skinsNeedSelect = 1;
            g_skinsOpenedAt = GetTickCount();
            CallMenu(MENU_SKINS);
        }
        return 0;
    }
    if (action >= AC_SKIN_PICK_BASE && action < AC_SKIN_PICK_BASE + g_skinCount) {
        if (*(int*)G_MENU_STATE == MENU_SKINS
            && GetTickCount() - g_skinsOpenedAt > 200) { // swallow held confirm
            ApplySkinIndex(action - AC_SKIN_PICK_BASE);
            MenuBack();
            *(unsigned char*)G_MENU_REBUILD = 1;
        }
        return 0;
    }
    if (action == AC_COUNT_CYCLE) {
        g_countSel = (g_countSel + 1) % NUM_CNT;
        InterlockedExchange(&g_chalDirty, 1);
        *(unsigned char*)G_MENU_REBUILD = 1;
        return 0;
    }
    if (action == AC_GUNS_CYCLE) {
        g_guns ^= 1;
        InterlockedExchange(&g_chalDirty, 1);
        *(unsigned char*)G_MENU_REBUILD = 1;
        return 0;
    }
    if (action == AC_BARS_CYCLE) {
        g_bars = g_bars == BarsBase ? BarsMaximum : BarsBase;
        InterlockedExchange(&g_chalDirty, 1);
        *(unsigned char*)G_MENU_REBUILD = 1;
        return 0;
    }
    const char* lvl = NULL;
    int i;
    i = action - AC_ARENA_BASE;
    if (i >= 0 && i < (int)NUM_ARENAS)
        lvl = ARENAS[i].lvls[g_guns][g_countSel];
    i = action - AC_EXTRA_BASE;
    const Entry* e = NULL;
    if (!lvl && i >= 0 && i < (int)(sizeof(EXTRA)/sizeof(EXTRA[0])))
        e = &EXTRA[i];
    if (!lvl && e) lvl = e->lvl;
    if (!lvl) return -1;
    g_challengeSession = IsStandaloneChallengeLevel(lvl);
    // 0x5EF4948 is the picked-level buffer: the state machine (0x569990)
    // stricmp()s it against the chapter name [0x7C2280] and then passes it
    // straight to 0x4F9570 on BOTH branches. 0x706B84 turned out to be a
    // read-only const (".rdata") -- writing it AV'd inside our own dll.
    strcpy((char*)G_PICKED_LVL, lvl);
    return 1;
}

// runs inside the 0x4F93B0 pump right at its `cmp byte [0x6030408],0` — the
// last possible moment, after the pump's pending-unload processing that can
// wipe the buffer. Stats mirror 0x4F5810's layout; no hero -> defaults.
static void __cdecl ArmChain() {
    FILE* f; fopen_s(&f, "br2mod_chain.log", "a");
    if (f) { fprintf(f, "pump buf='%s' state=%02X\n",
             (char*)0x6030408, *(DWORD*)0x5EF4BF8); fclose(f); }
    if (!g_pendingChain[0]) return;
    memset((void*)0x6030408, 0, 0x1a8);
    strcpy((char*)0x6030408, g_pendingChain);
    *(float*)0x6030488 = 1000.0f;   // +0x80 hero health (hitPoints=1000)
    *(float*)0x603048C = 0.0f;      // +0x84 rage
    *(float*)0x6030490 = 0.0f;      // +0x88 ammo float
    *(float*)0x6030494 = 1.0f;      // +0x8c persistent float
    *(DWORD*)0x603051C = g_diff + 1; // +0x114 difficulty (1-4)
    ApplySkinForce();               // keep the forced id through chain loads
    *(char*)0x5E335BD = 1;          // chain-pending flag (pump clears it)
    g_pendingChain[0] = 0;
}

// Site 0x4F9405 (inside the 0x4F93B0 pump): arm the chain buffer at the exact
// name check. Stolen (7B): cmp byte [0x6030408],0 -> back to 0x4F940C
static void __declspec(naked) HookLoad() {
    BR2_SAVE_CPU
    __asm { call ArmChain }
    BR2_RESTORE_CPU
    __asm {
        cmp  byte ptr ds:[0x6030408], 0
        push 0x4F940C
        ret
    }
}

// Log every script chainToLevel(name) call: site 0x4F5A30 entry.
// Stolen (8B): push ebp / mov ebp,esp / fldz / push esi / mov esi,ecx
static void __cdecl LogChain(const char* name) {
    FILE* f; fopen_s(&f, "br2mod_chain.log", "a");
    if (f) { fprintf(f, "chainToLevel('%s') state=%02X\n",
             name ? name : "?", *(DWORD*)0x5EF4BF8); fclose(f); }
}
static void __declspec(naked) HookChainCmd() {
    __asm {
        push ebp
        mov  ebp, esp
        fldz
        push esi
        mov  esi, ecx
    }
    BR2_SAVE_CPU
    __asm {
        push dword ptr [ebp + 8]
        call LogChain
        add  esp, 4
    }
    BR2_RESTORE_CPU
    __asm {
        push 0x4F5A38
        ret
    }
}

// ------------------------- naked thunks ------------------------------------
// The native panel replaces its measured height with 320 + two row heights
// above a threshold. The eighth Options row crosses it, leaving a large gap.
static void __declspec(naked) HookOptionsPanelHeight() {
    __asm {
        pushfd
        cmp dword ptr ds:[G_MENU_STATE], MENU_OPTIONS
        jne stock
        cmp g_combat, CombatEnhanced
        jne stock
        popfd
        push 0x5640CD // use the measured height already in [ebp - 0x5AC]
        ret
    stock:
        popfd
        fild dword ptr [ebp - 0x5C4]
        push 0x5640BF
        ret
    }
}

// HookA @0x561E1A: stolen `mov ecx,[0x7c07f8]` (6B) -> `push HookA; ret` (6B)
static void __declspec(naked) HookA() {
    BR2_SAVE_CPU
    __asm { call OnMenuBuilt }
    BR2_RESTORE_CPU
    __asm {
        // MSVC inline asm treats a bare numeric [constant] as an immediate.
        // DS: is required: replay 8B 0D F8 07 7C 00, NOT B9 F8 07 7C 00.
        mov ecx, dword ptr ds:[0x7c07f8]
        push 0x561E20
        ret
    }
}

// HookB @0x5670E1: stolen `cmp edi,0x10c`+`ja 0x568e43` (12B) ->
// `push HookB; ret` + 6 nops. edi = action code.
static void __declspec(naked) HookB() {
    __asm {
        cmp edi, AC_CHALLENGE_MENU
        jb  orig
        cmp edi, AC_SKIN_PICK_BASE + 0x40
        jae orig
        pushad
        push edi
        call DispatchOurs
        add esp, 4
        mov [esp + 0x1c], eax        // store result in saved-eax slot
        popad
        test eax, eax
        jz  handled
        cmp eax, 1
        je  launch
    orig:
        cmp edi, 0x10c
        jbe inrange
        push 0x568E43
        ret
    inrange:
        push 0x5670ED
        ret
    handled:
        push 0x56913E
        ret
    launch:
        mov eax, 1
        push 0x56914A
        ret
    }
}

// ------------------------- difficulty hooks --------------------------------
// Entered via `call` patches at each `fsubr [esi+0x39C8]` site: st0 = raw
// damage, esi = victim object. Scale st0, log optionally, replay the stolen
// instruction, ret -> the site+5 nop -> the original fstp stores the result.
static void __declspec(naked) HookDmgHealth() {
    __asm {
        pushfd
        pushad
        fst  dword ptr [g_new]           // raw damage for the log
        mov  eax, [g_dmgOut]
        cmp  esi, dword ptr ds:[G_PLAYER_OBJ]
        jne  use
        mov  eax, [g_dmgIn]
    use:
        mov  [g_mult], eax
        fmul dword ptr [g_mult]          // st0 = dmg * mult
        cmp  dword ptr [g_diffTrace], 0
        je   apply
        mov  eax, [esi + 0x39C8]
        mov  [g_hOld], eax
        mov  [g_hObj], esi
    }
    BR2_SAVE_CPU
    __asm {
        push offset g_healthtag
        call LogDmgHit
        add  esp, 4
    }
    BR2_RESTORE_CPU
    __asm {
    apply:
        fsubr dword ptr [esi + 0x39C8]   // stolen: health - scaled dmg
    }
    __asm { popad }
    __asm { popfd }
    __asm { ret }
}

// TakeDamage tail: entered via `call` at `fstp [reg+0x39C8]` — st0 = the
// already-computed new health. old>new means real damage: rescale the
// difference by dmgIn/dmgOut, then replay the fstp ourselves.
static void __declspec(naked) HookHitEsi() {
    __asm {
        pushfd
        pushad
        mov  eax, [esi + 0x39C8]
        fst  dword ptr [g_new]
        cmp  eax, dword ptr [g_new]
        jbe  done                            // heal/equal -> store raw
        mov  [g_hObj], esi
        mov  [g_hOld], eax
        mov  ecx, [g_dmgOut]
        cmp  esi, dword ptr ds:[G_PLAYER_OBJ]
        jne  scaled
        mov  ecx, [g_dmgIn]
    scaled:
        mov  [g_mult], ecx
        fld  dword ptr [esi + 0x39C8]
        fsub dword ptr [g_new]               // st0 = dmg
        fmul dword ptr [g_mult]
        fsubr dword ptr [esi + 0x39C8]       // st0 = old - dmg*mult
        fxch st(1)
        fstp st(0)                           // st0 = scaled new
        cmp  dword ptr [g_diffTrace], 0
        je   done
    }
    BR2_SAVE_CPU
    __asm {
        push offset g_healthtag
        call LogDmgHit
        add  esp, 4
    }
    BR2_RESTORE_CPU
    __asm {
    done:
        fstp dword ptr [esi + 0x39C8]
    }
    __asm { popad }
    __asm { popfd }
    __asm { ret }
}

static void __declspec(naked) HookHitEdi() {
    __asm {
        pushfd
        pushad
        mov  eax, [edi + 0x39C8]
        fst  dword ptr [g_new]
        cmp  eax, dword ptr [g_new]
        jbe  done
        mov  [g_hObj], edi
        mov  [g_hOld], eax
        mov  ecx, [g_dmgOut]
        cmp  edi, dword ptr ds:[G_PLAYER_OBJ]
        jne  scaled
        mov  ecx, [g_dmgIn]
    scaled:
        mov  [g_mult], ecx
        fld  dword ptr [edi + 0x39C8]
        fsub dword ptr [g_new]
        fmul dword ptr [g_mult]
        fsubr dword ptr [edi + 0x39C8]
        fxch st(1)
        fstp st(0)
        cmp  dword ptr [g_diffTrace], 0
        je   done
    }
    BR2_SAVE_CPU
    __asm {
        push offset g_healthtag
        call LogDmgHit
        add  esp, 4
    }
    BR2_RESTORE_CPU
    __asm {
    done:
        fstp dword ptr [edi + 0x39C8]
    }
    __asm { popad }
    __asm { popfd }
    __asm { ret }
}

// eax is the victim itself — use ecx/edx as scratch.
static void __declspec(naked) HookHitEax() {
    __asm {
        pushfd
        pushad
        mov  ecx, [eax + 0x39C8]
        fst  dword ptr [g_new]
        cmp  ecx, dword ptr [g_new]
        jbe  done
        mov  [g_hObj], eax
        mov  [g_hOld], ecx
        mov  edx, [g_dmgOut]
        cmp  eax, dword ptr ds:[G_PLAYER_OBJ]
        jne  scaled
        mov  edx, [g_dmgIn]
    scaled:
        mov  [g_mult], edx
        fld  dword ptr [eax + 0x39C8]
        fsub dword ptr [g_new]
        fmul dword ptr [g_mult]
        fsubr dword ptr [eax + 0x39C8]
        fxch st(1)
        fstp st(0)
        cmp  dword ptr [g_diffTrace], 0
        je   done
    }
    BR2_SAVE_CPU
    __asm {
        push offset g_healthtag
        call LogDmgHit
        add  esp, 4
    }
    BR2_RESTORE_CPU
    __asm {
    done:
        fstp dword ptr [eax + 0x39C8]
    }
    __asm { popad }
    __asm { popfd }
    __asm { ret }
}

// Same for `fsubr [esi+0x8360]`: st0 = rage cost, only the player pays it.
static void __declspec(naked) HookDmgRage() {
    __asm {
        pushfd
        pushad
        fst  dword ptr [g_new]
        mov  eax, [g_cost]
        mov  [g_mult], eax
        fmul dword ptr [g_mult]
        cmp  dword ptr [g_diffTrace], 0
        je   apply
        mov  eax, [esi + 0x8360]
        mov  [g_hOld], eax
        mov  [g_hObj], esi
    }
    BR2_SAVE_CPU
    __asm {
        push offset g_ragetag
        call LogDmgHit
        add  esp, 4
    }
    BR2_RESTORE_CPU
    __asm {
    apply:
        fsubr dword ptr [esi + 0x8360]   // stolen: rage - scaled cost
        popad
        popfd
        ret
    }
}

// ------------------------- CRT stricmp NULL guard --------------------------
// 0x6B3D80 = fast _stricmp tail-called by the 0x6B3E79 wrapper when
// locale==0. The wrapper null-checks its args, but some engine code calls
// the fast path DIRECTLY with a bad pointer -> AV @0x6B3D8D (seen while
// loading mission-type LVLs). Guard: unreadable/NULL arg -> INT_MAX
// (mismatch, same as CRT EINVAL path) + log caller to br2mod_stricmp.log
// so we can find the real lookup. Returns -1 => run the original code.
static FILE* g_silog;
static int __cdecl StricmpGuard(DWORD ret, const char* a, const char* b) {
    MEMORY_BASIC_INFORMATION mbi;
    int badA = ((DWORD)a < 0x10000) ||
               !VirtualQuery(a, &mbi, sizeof mbi) ||
               mbi.State != MEM_COMMIT ||
               (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    int badB = ((DWORD)b < 0x10000) ||
               !VirtualQuery(b, &mbi, sizeof mbi) ||
               mbi.State != MEM_COMMIT ||
               (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    if (!badA && !badB) return -1;
    char sa[160],sb[160]; SafeText(a,sa,sizeof sa); SafeText(b,sb,sizeof sb);
    Trace("BAD-STRICMP caller=%08lX a=%08lX bad=%d '%s' b=%08lX bad=%d '%s' glass=%08lX",
          ret,(DWORD)a,badA,sa,(DWORD)b,badB,sb,ReadWord(0x7C0BD0));
    if (!g_silog) fopen_s(&g_silog, "br2mod_stricmp.log", "w");
    if (g_silog) {
        fprintf(g_silog, "caller=%08X a=%08X b=%08X\n", ret,
                (DWORD)a, (DWORD)b);
        fflush(g_silog);
    }
    return 0x7fffffff;
}

// Stolen (8B): mov edi,edi / push ebp / mov ebp,esp / mov edx,[ebp+0xC]
static void __declspec(naked) HookStricmp() {
    __asm {
        push dword ptr [esp + 8]          // b
        push dword ptr [esp + 8]          // a
        push dword ptr [esp + 8]          // ret addr
        call StricmpGuard
        add  esp, 12
        cmp  eax, -1
        je   runorig
        ret                               // eax = INT_MAX -> "mismatch"
    runorig:
        mov edi, edi
        push ebp
        mov ebp, esp
        mov edx, dword ptr [ebp + 0xC]
        push 0x6B3D88
        ret
    }
}

// ------------------- legacy effect-cache unreadable-slot guard -------------
// 0x5DCDD30 = 150-slot effect cache (0x4A28E9 says "effect cache").
// This legacy experiment only checks mapping/readability, NOT allocation
// lifetime. A bad first stricmp argument can be the REQUESTED name in EDI;
// it does not by itself prove that any cache slot is stale. Keep the existing
// behavior for this diagnostic comparison; do not add a full cache reset.
static int __cdecl IsBadPtr(const void* p) {
    // Cache: pointers in the registry cluster in a few heap regions, so keep
    // the last-validated [page,regionEnd) and skip the syscall on hits --
    // otherwise the per-slot-per-lookup VirtualQuery makes loads take ages.
    static DWORD g_lo = 0, g_hi = 0;
    DWORD a = (DWORD)p;
    if (a < 0x10000) return 1;
    if (a >= g_lo && a < g_hi) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) ||
        mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        Trace("LEGACY-PRUNE unreadable cache entry=%08lX",(DWORD)p);
        g_lo = g_hi = 0; return 1;
    }
    g_lo = (DWORD)mbi.BaseAddress;
    g_hi = g_lo + mbi.RegionSize;
    return 0;
}

// Stolen (7B): push eax / push edi / call 0x6B3E79  (add esp,8 done by caller
// at 0x4A0C85). Bad entry -> prune slot, jump to 0x4A0C8C (inc esi / loop).
static void __declspec(naked) HookRegPrune() {
    __asm {
        push eax                          // save entry
        push eax                          // arg
        call IsBadPtr
        add  esp, 4                       // cdecl arg cleanup
        test eax, eax
        pop  eax                          // restore entry (flags kept)
        jz   entry_ok
        mov  dword ptr [esi*4 + 0x5DCDD30], 0
        push 0x4A0C8C
        ret
    entry_ok:
        push eax
        push edi
        mov  edx, 0x6B3E79
        call edx
        push 0x4A0C85
        ret
    }
}

// ------------------------- patch machinery ---------------------------------
static void PatchSite(void* site, int siteLen, void* target) {
    DWORD old;
    if (!VirtualProtect(site, siteLen, PAGE_EXECUTE_READWRITE, &old)) {
        Trace("PATCH failed address=%08lX error=%lu",(DWORD)site,GetLastError()); return;
    }
    BYTE* p = (BYTE*)site;
    p[0] = 0x68;                          // push imm32
    *(DWORD*)(p + 1) = (DWORD)target;
    p[5] = 0xC3;                          // ret
    for (int i = 6; i < siteLen; ++i) p[i] = 0x90;
    VirtualProtect(site, siteLen, old, &old);
    FlushInstructionCache(GetCurrentProcess(),site,siteLen);
    Trace("PATCH address=%08lX length=%d target=%08lX",(DWORD)site,siteLen,(DWORD)target);
}

static void PatchChecked(DWORD site,const char* expected,int length,void* target) {
    if (memcmp((void*)site,expected,length)) {
        Trace("PATCH SKIPPED signature mismatch address=%08lX",site); return;
    }
    PatchSite((void*)site,length,target);
}

static void EnableSubmenuCancel(int menu, BYTE expected) {
    // Native Esc/B dispatch indexes this table by menu ID minus one.
    // Case 8 goes straight to MenuBack, as used by stock submenus.
    BYTE* entry = (BYTE*)(0x5691DC + menu - 1);
    DWORD old;
    if (*entry != expected ||
        !VirtualProtect(entry, 1, PAGE_EXECUTE_READWRITE, &old)) {
        Trace("PATCH failed: cancel entry for menu %X", menu);
        return;
    }
    *entry = 0x08;
    VirtualProtect(entry, 1, old, &old);
}

// `call rel32` detour: pushes the site return so ONE handler can serve many
// sites sharing the same stolen instruction. Handler must replay it and ret.
static void PatchCallChecked(DWORD site,const char* expected,int length,void* target) {
    if (memcmp((void*)site,expected,length)) {
        Trace("PATCH SKIPPED signature mismatch address=%08lX",site); return;
    }
    DWORD old;
    if (!VirtualProtect((void*)site,length,PAGE_EXECUTE_READWRITE,&old)) {
        Trace("PATCH failed address=%08lX error=%lu",site,GetLastError()); return;
    }
    BYTE* p=(BYTE*)site;
    p[0]=0xE8;
    *(DWORD*)(p+1)=(DWORD)target-(site+5);
    for (int i=5;i<length;++i) p[i]=0x90;
    VirtualProtect((void*)site,length,old,&old);
    FlushInstructionCache(GetCurrentProcess(),(void*)site,length);
    Trace("PATCH-CALL address=%08lX length=%d target=%08lX",site,length,(DWORD)target);
}

// loose-file priority: wait until both handlers are installed.
// Swap ONLY if still POD-first — if Gunsucked already made it
// loose-first we leave it alone (coexistence).
static void EnableLooseFilePriority() {
    DWORD* h = (DWORD*)G_HANDLER_ARRAY;
    while (!h[0] || !h[1]) Sleep(50);
    if (h[0] == 0x5A3FD0) {          // POD first -> flip to loose-first
        DWORD old;
        VirtualProtect(h, 8, PAGE_READWRITE, &old);
        DWORD t = h[0]; h[0] = h[1]; h[1] = t;
        VirtualProtect(h, 8, old, &old);
    }
}

// ------------------------- crash logger ------------------------------------
// Vectored handler: on AV, log EIP + ebp-frame call chain to
// br2mod_crash.log then let the game's own dialog run.
static LONG CALLBACK CrashLog(EXCEPTION_POINTERS* ep) {
    LONG traceResult = TraceException(ep);
    if (traceResult == EXCEPTION_CONTINUE_EXECUTION) return traceResult;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    FILE* f;
    if (fopen_s(&f, "br2mod_crash.log", "w") != 0) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = ep->ContextRecord;
    fprintf(f, "AV at %08X  eip=%08X esp=%08X ebp=%08X\n",
            (DWORD)ep->ExceptionRecord->ExceptionAddress,
            c->Eip, c->Esp, c->Ebp);
    DWORD* fr = (DWORD*)c->Ebp;
    for (int i = 0; i < 24 && fr; ++i) {
        DWORD words[2]; if (!SafeRead(fr,words,sizeof words)) break;
        fprintf(f, "  ret=%08X\n", words[1]);
        if (words[0] <= (DWORD)fr || words[0]-(DWORD)fr > 0x100000) break;
        fr = (DWORD*)words[0];
    }
    // Dump loaded modules (PEB walk) so we can identify any crash address
    // that lands outside rayne2.exe itself.
    char* peb = (char*)__readfsdword(0x30);
    if (peb) {
        char* ldr = *(char**)(peb + 0x0C);
        LIST_ENTRY* head = (LIST_ENTRY*)(ldr + 0x0C);
        DWORD eip = c->Eip;
        for (LIST_ENTRY* e = head->Flink; e != head; e = e->Flink) {
            char* mod = (char*)e;
            DWORD base = *(DWORD*)(mod + 0x18);
            DWORD size = *(DWORD*)(mod + 0x20);
            USHORT nlen = *(USHORT*)(mod + 0x2C);
            WCHAR* nbuf = *(WCHAR**)(mod + 0x30);
            char name[128] = {0};
            if (nbuf) {
                int n = nlen / 2; if (n > 120) n = 120;
                for (int i = 0; i < n; ++i)
                    name[i] = (nbuf[i] < 128) ? (char)nbuf[i] : '?';
            }
            fprintf(f, "%s mod %08X-%08X %s\n",
                    (eip >= base && eip < base + size) ? ">>>" : "   ",
                    base, base + size, name);
        }
    }
    fclose(f);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ------------------------- dinput8 forwarding -------------------------------
extern "C" {
typedef HRESULT (WINAPI *DI8C_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static DI8C_t g_realDI8C;
HRESULT WINAPI DirectInput8Create(
    HINSTANCE a, DWORD b, REFIID c, LPVOID* d, LPUNKNOWN e) {
    if (!g_realDI8C) {
        char p[MAX_PATH];
        GetSystemDirectoryA(p, MAX_PATH);
        strcat(p, "\\dinput8.dll");
        g_realDI8C = (DI8C_t)GetProcAddress(LoadLibraryA(p), "DirectInput8Create");
    }
    return g_realDI8C(a, b, c, d, e);
}
}

// ------------------------- init --------------------------------------------
static void LoadConfig() {
    g_legacyGuards = GetPrivateProfileIntA("Diagnostics","LegacyGuards",0,".\\br2mod.ini");
    g_chainTrace = GetPrivateProfileIntA("Diagnostics","ChainTrace",0,".\\br2mod.ini");
    g_diffTrace = GetPrivateProfileIntA("Diagnostics","DifficultyTrace",0,".\\br2mod.ini");
    g_trigTrace = GetPrivateProfileIntA("Diagnostics","TrigTrace",0,".\\br2mod.ini");
    char buf[64];
    GetPrivateProfileStringA("Difficulty", "preset", "Dhampir",
                             buf, sizeof buf, ".\\br2mod.ini");
    for (int i = 0; i < NUM_PRESETS; ++i)
        if (!_stricmp(buf, PRESETS[i].name)) g_diff = i;
    GetPrivateProfileStringA("Combat", "mode", "Normal", buf, sizeof buf, ".\\br2mod.ini");
    g_combat = !_stricmp(buf, "Enhanced") ? CombatEnhanced : CombatNormal;
    for (int effect = 0; effect < EffectCount; ++effect)
        if (!GetPrivateProfileIntA("Combat", COMBAT_EFFECTS[effect].key, 1, ".\\br2mod.ini"))
            g_effects &= ~(1L << effect);
    GetPrivateProfileStringA("Appearance", "skin", "default",
                             g_skinIni, sizeof g_skinIni, ".\\br2mod.ini");
    {   // remembered challenge options
        int n = GetPrivateProfileIntA("Challenge", "count", 25,
                                      ".\\br2mod.ini");
        for (int i = 0; i < NUM_CNT; ++i)
            if (COUNTS[i] == n) { g_countSel = i; break; }
        g_guns = GetPrivateProfileIntA("Challenge", "guns", 0,
                                       ".\\br2mod.ini") ? 1 : 0;
        g_bars = GetPrivateProfileIntA("Challenge", "maxBars", 0,
                                       ".\\br2mod.ini") == 1 ? BarsMaximum : BarsBase;
    }
}

static DWORD WINAPI Worker(LPVOID) {
    InitTrace();
    LoadConfig();
    ReadEnhancedSettings();
    BuildTrackList();
    LoadBestTimes();
    EnableLooseFilePriority();       // polls until handlers installed
    Sleep(500);
    if (g_traceEnabled) {
        PatchChecked(0x64D230,"\x55\x8b\xec\x81\xec\x2c\x02\x00\x00",9,HookOpenTrace);
        PatchChecked(0x64D26E,"\x8b\x16\x83\xc4\x04\x84\xc0",7,HookExistsTrace);
        PatchChecked(0x64D2A5,"\x8b\x4d\xfc\x5f\x5e\x33\xcd",7,HookOpenedTrace);
        PatchChecked(0x5038EC,"\x8b\x0d\xd0\x0b\x7c\x00",6,HookGlassTrace);
        PatchChecked(0x57A200,"\x55\x8b\xec\x6a\xff\x68\x72\x2b\x6d\x00",10,HookSituationTrace);
        PatchChecked(0x4A2B20,"\x55\x8b\xec\x81\xec\x04\x01\x00\x00",9,HookEffectRegisterTrace);
        PatchChecked(0x4A2953,"\x8b\x45\x08\x50\x53\x8b\xce",7,HookEffectLoadTrace);
        PatchChecked(0x559F10,"\xa1\xac\x48\xef\x05\x83\xf8\x01",8,HookMenuBack);
    }
    PatchChecked(0x55EB10,"\x55\x8b\xec\x6a\xff\x68\xf5\x27\x6d\x00",10,HookMenuBegin);
    PatchChecked(0x609D90,"\x55\x8b\xec\x56\x8b\xf1",6,HookFeedMotionRequest);
    PatchChecked(0x56A170,"\x55\x8b\xec\x8b\x45\x08",6,HookGetBonePos);
    PatchSite((void*)0x561E1A, 6, HookA);
    PatchChecked(0x5640B9,"\xDB\x85\x3C\xFA\xFF\xFF",6,HookOptionsPanelHeight);
    PatchSite((void*)0x5670E1, 12, HookB);
    EnableSubmenuCancel(MENU_CHALLENGE, 0x0A);
    EnableSubmenuCancel(MENU_EFFECTS, 0x0B);
    EnableSubmenuCancel(MENU_SKINS, 0x0B);
    PatchChecked(0x566430,"\xd9\xee\xd8\x1d\xb4\x02\x03\x06",8,HookSavePrompt);
    PatchCallChecked(0x4F83A9,"\xE8\x12\xEE\xFF\xFF",5,HookHudRender);
    PatchChecked(0x4F7AF9,"\x6A\x00\xE8\xD0\xA4\x06\x00",7,HookChallengePause);
    PatchCallChecked(0x4F9137,"\xE8\xD4\xE6\xFF\xFF",5,HookChallengeExit);
    PatchCallChecked(0x4F8DB3,"\xE8\x68\xFA\xFF\xFF",5,HookParryInput);
    PatchCallChecked(0x663EF3,"\x68\x74\x2D\x72\x00\x56\x8B\xCF\xFF\xD2",10,HookFeedRepel);
    PatchCallChecked(0x6738E4,"\x68\xBC\x3E\x72\x00\x56\x8B\xCF\xFF\xD0",10,HookFeedRepelAlternate);
    PatchCallChecked(0x67492C,"\xE8\xAF\xD5\x01\x00",5,HookFeedEscapeRootAngles);
    PatchCallChecked(0x4F44FF,"\xE8\x7C\x52\x15\x00",5,HookParryWorldFactor);
    PatchCallChecked(0x6498D8,"\xE8\x63\xFD\xFF\xFF",5,HookParryActorFactor);
    PatchChecked(0x551206,"\x53\x8B\xCE\xE8\xE2\x7A\x11\x00",8,HookParryContact);
    // Native character update calls that advance the actor using its +0x590 channel.
    PatchCallChecked(0x479DF0,"\xE8\xEB\xFA\x1C\x00",5,HookEnhancedActorUpdate);
    PatchCallChecked(0x4823CF,"\xE8\x0C\x75\x1C\x00",5,HookEnhancedActorUpdate);
    PatchCallChecked(0x4AEF0F,"\xE8\xCC\xA9\x19\x00",5,HookEnhancedActorUpdate);
    PatchCallChecked(0x4BA679,"\xE8\x62\xF2\x18\x00",5,HookEnhancedActorUpdate);
    PatchCallChecked(0x4C5C79,"\xE8\x62\x3C\x18\x00",5,HookEnhancedActorUpdate);
    PatchCallChecked(0x4DCCFE,"\xE8\xDD\xCB\x16\x00",5,HookEnhancedActorUpdate);
    PatchCallChecked(0x528C9F,"\xE8\x3C\x0C\x12\x00",5,HookEnhancedActorUpdate);
    PatchCallChecked(0x5344CA,"\xE8\x11\x54\x11\x00",5,HookEnhancedActorUpdate);
    PatchCallChecked(0x53CA8C,"\xE8\x4F\xCE\x10\x00",5,HookEnhancedActorUpdate);
    PatchCallChecked(0x54892D,"\xE8\xAE\x0F\x10\x00",5,HookEnhancedActorUpdate);
    PatchCallChecked(0x55212D,"\xE8\xAE\x77\x0F\x00",5,HookEnhancedActorUpdate);
    PatchCallChecked(0x5C7959,"\xE8\x82\x1F\x08\x00",5,HookEnhancedActorUpdate);
    PatchCallChecked(0x5D0977,"\xE8\x64\x8F\x07\x00",5,HookEnhancedActorUpdate);
    PatchCallChecked(0x6159DF,"\xE8\xFC\x3E\x03\x00",5,HookEnhancedActorUpdate);
    PatchCallChecked(0x617BAA,"\xE8\x31\x1D\x03\x00",5,HookEnhancedActorUpdate);
    PatchCallChecked(0x64116D,"\xE8\x6E\x87\x00\x00",5,HookEnhancedActorUpdate);
    PatchChecked(0x55123C,"\x81\x3B\x87\x00\x00\x00\x0F\x84\xE8\x08\x00\x00",12,HookCombatRecoveryGate);
    PatchChecked(0x5515E5,"\x6A\x00\x53\x8B\xCE\xE8\xC1\x1D\xEF\xFF",10,HookCombatDamageApplied);
    PatchChecked(0x669350,"\x55\x8B\xEC\xA1\x20\xCF\x7D\x00",8,HookCombatOutgoing);
    PatchChecked(0x662948,"\x89\xB3\x74\x83\x00\x00",6,HookCombatHarpoonPrimary);
    PatchChecked(0x662B4A,"\x89\xBB\x74\x83\x00\x00",6,HookCombatHarpoonSecondary);
    PatchCallChecked(0x5868E4,"\xE8\x57\xCD\xF5\xFF",5,HookScatterProjectile);
    PatchCallChecked(0x586607,"\xE8\x84\xF1\xE7\xFF",5,HookGunMessageInit);
    PatchCallChecked(0x5508C0,"\xE8\x0B\xBB\xFF\xFF",5,HookDodgeBlade);
    PatchCallChecked(0x5508B7,"\xE8\x74\xBE\xFF\xFF",5,HookDodgeKick);
    PatchChecked(0x54D33C,"\x5F\x5E\x5B\x8B\xE5\x5D\xC2\x04\x00",9,HookDodgeMoveStarted);
    PatchCallChecked(0x672A7B,"\xE8\xA0\x51\xF9\xFF",5,HookCombatAnimationStep); // Rayne
    PatchCallChecked(0x5BB9DE,"\xE8\x3D\xC2\x04\x00",5,HookCombatAnimationStep); // punk controller
    PatchCallChecked(0x5CD9DA,"\xE8\x41\xA2\x03\x00",5,HookCombatAnimationStep); // alternate controller
    PatchCallChecked(0x450BF6,"\xE8\xB5\xBA\xFF\xFF",5,HookCinematicDetach);
    PatchCallChecked(0x5B3E4D,"\x8B\x07\x8B\x50\x3C\x8D\x8D\x78\xFF\xFF\xFF\x51\x8B\xCF\xFF\xD2",16,HookCinematicCollision);
    PatchCallChecked(0x66D1AC,"\x8B\x42\x3C\x57\xFF\xD0",6,HookCinematicKick);
    PatchCallChecked(0x4089C6,"\x8B\x50\x3C\x56\xFF\xD2",6,HookCinematicRay);
    PatchCallChecked(0x4098D8,"\x8B\x52\x3C\x89\x85\x48\xFB\xFF\xFF\x8D\x85\xEC\xFA\xFF\xFF\x50\x8B\xCB\xFF\xD2",20,HookCinematicWeapon);
    PatchCallChecked(0x662851,"\x8B\x52\x3C\x8D\x85\xC4\xF6\xFF\xFF\x50\xFF\xD2",12,HookCinematicHarpoon);
    PatchCallChecked(0x4A219B,"\xE8\xF0\x61\x11\x00",5,HookCombatFxStart);
    // Both launch and native restart enter this level loop after loading Rayne.
    PatchCallChecked(0x4F9026,"\xE8\x15\xC6\xFF\xFF",5,HookInitChallengeRun);
    PatchCallChecked(0x55213B,"\xE8\x00\x39\xF8\xFF",5,HookChallengeBarsRank); // health cap
    PatchCallChecked(0x55215E,"\xE8\xDD\x38\xF8\xFF",5,HookChallengeBarsRank); // rage cap
    PatchCallChecked(0x592142,"\xE8\xF9\x38\xF4\xFF",5,HookChallengeBarsRank); // HUD length
    // A native rank-up refills both pools. Use the same fixed rank on both
    // sides of the comparison so a challenge cannot inherit a near rank-up.
    PatchCallChecked(0x4D5E58,"\xE8\xE3\xFB\xFF\xFF",5,HookChallengeBarsRank);
    PatchCallChecked(0x4D5ECB,"\xE8\x70\xFB\xFF\xFF",5,HookChallengeBarsRank);
    InstallPhotoMode();
    // 0x4CD940 (display setter call) detour removed: unstable, see note above.
    ApplyPreset();
    for (unsigned i = 0; i < sizeof(HEALTH_DMG_SITES)/sizeof(HEALTH_DMG_SITES[0]); ++i)
        PatchCallChecked(HEALTH_DMG_SITES[i], "\xD8\xAE\xC8\x39\x00\x00", 6, HookDmgHealth);
    for (unsigned i = 0; i < sizeof(HIT_FSTP_ESI)/sizeof(HIT_FSTP_ESI[0]); ++i)
        PatchCallChecked(HIT_FSTP_ESI[i], "\xD9\x9E\xC8\x39\x00\x00", 6, HookHitEsi);
    for (unsigned i = 0; i < sizeof(HIT_FSTP_EDI)/sizeof(HIT_FSTP_EDI[0]); ++i)
        PatchCallChecked(HIT_FSTP_EDI[i], "\xD9\x9F\xC8\x39\x00\x00", 6, HookHitEdi);
    for (unsigned i = 0; i < sizeof(HIT_FSTP_EAX)/sizeof(HIT_FSTP_EAX[0]); ++i)
        PatchCallChecked(HIT_FSTP_EAX[i], "\xD9\x98\xC8\x39\x00\x00", 6, HookHitEax);
    PatchCallChecked(RAGE_DMG_SITE, "\xD8\xAE\x60\x83\x00\x00", 6, HookDmgRage);
    // Outfit override: retarget the `call GetOutfitIndex` inside every path
    // getter so +0x3CC holds our id right before the lookup. Each site is a
    // 5-byte E8 rel32 to 0x4F6930; Gunsucked's own function hooks (if any)
    // sit downstream and pass stock ids straight through.
    for (unsigned i = 0; i < sizeof(OUTFIT_INDEX_CALLS)/sizeof(OUTFIT_INDEX_CALLS[0]); ++i) {
        DWORD site = OUTFIT_INDEX_CALLS[i];
        char exp[5]; exp[0] = (char)0xE8;
        *(DWORD*)(exp + 1) = OUTFIT_INDEX_FN - (site + 5);
        PatchCallChecked(site, exp, 5, ForceOutfitIndex);
    }
    PatchCallChecked(0x549C3E,"\xE8\x6D\xCD\xFA\xFF",5,GetPlayerOutfitModel);
    PatchCallChecked(0x54A25D,"\xE8\x4E\xC7\xFA\xFF",5,GetPlayerOutfitModel);
    PatchCallChecked(0x54A3CA,"\xE8\xF1\xC5\xFA\xFF",5,GetPlayerOutfitPhysics);
    // These were diagnostic workarounds for the wrong-this corruption now
    // fixed in HookA. VirtualQuery on every stricmp dominated character load.
    // Leave the native CRT/cache lookup and native chain path intact by default.
    if (g_legacyGuards) {
        PatchSite((void*)0x6B3D80,8,HookStricmp);
        PatchSite((void*)0x4A0C7E,7,HookRegPrune);
    }
    if (g_chainTrace) {
        PatchSite((void*)0x4F9405,7,HookLoad);
        PatchSite((void*)0x4F5A30,8,HookChainCmd);
    }
    TraceGlobals("hooks-installed");
    ArmGlassWatch();
    // Lazy INI writer: keeps all br2mod.ini disk I/O off the game thread.
    for (;;) {
        Sleep(500);
        FlushParryLog();
        ReadEnhancedSettings();
        // WritePrivateProfileStringA leaves stale NUL padding when the new
        // value is shorter than the old one: delete the key first.
        if (InterlockedExchange(&g_presetDirty, 0)) {
            WritePrivateProfileStringA("Difficulty", "preset", NULL, ".\\br2mod.ini");
            WritePrivateProfileStringA("Difficulty", "preset",
                                       PRESETS[g_diff].name, ".\\br2mod.ini");
        }
        if (InterlockedExchange(&g_combatDirty, 0)) {
            WritePrivateProfileStringA("Combat", "mode", NULL, ".\\br2mod.ini");
            WritePrivateProfileStringA("Combat", "mode",
                                       COMBAT_NAMES[g_combat], ".\\br2mod.ini");
        }
        const LONG effectsDirty = InterlockedExchange(&g_effectsDirty, 0);
        if (effectsDirty) {
            const LONG effects = InterlockedCompareExchange(&g_effects, 0, 0);
            for (int effect = 0; effect < EffectCount; ++effect) {
                const LONG bit = 1L << effect;
                if ((effectsDirty & bit) && !WritePrivateProfileStringA("Combat",
                    COMBAT_EFFECTS[effect].key, (effects & bit) ? "1" : "0", ".\\br2mod.ini"))
                    InterlockedOr(&g_effectsDirty, bit);
            }
        }
        if (InterlockedExchange(&g_skinDirty, 0)) {
            WritePrivateProfileStringA("Appearance", "skin", NULL, ".\\br2mod.ini");
            WritePrivateProfileStringA("Appearance", "skin",
                                       g_skinIni, ".\\br2mod.ini");
        }
        if (InterlockedExchange(&g_chalDirty, 0)) {
            char vb[8];
            sprintf(vb, "%d", COUNTS[g_countSel]);
            WritePrivateProfileStringA("Challenge", "count", NULL, ".\\br2mod.ini");
            WritePrivateProfileStringA("Challenge", "count", vb, ".\\br2mod.ini");
            WritePrivateProfileStringA("Challenge", "guns", NULL, ".\\br2mod.ini");
            WritePrivateProfileStringA("Challenge", "guns",
                                       g_guns ? "1" : "0", ".\\br2mod.ini");
            WritePrivateProfileStringA("Challenge", "maxBars",
                                       g_bars == BarsMaximum ? "1" : "0", ".\\br2mod.ini");
        }
    }
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_attachThreadId = GetCurrentThreadId();
        AddVectoredExceptionHandler(1, CrashLog);
        CreateThread(NULL, 0, Worker, NULL, 0, NULL);
    }
    return TRUE;
}
