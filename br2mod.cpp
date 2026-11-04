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
#define MENU_EXTRAS    0x0F        // native Extras menu
#define MENU_OPTIONS   0x17
#define MENU_CHALLENGE 0x3D        // free slot -> lands on hooked epilogue

// Action codes >= 0x110: natives stop at 0x10C, zero collisions.
#define AC_CHALLENGE_MENU 0x110
#define AC_DIFFICULTY     0x111
#define AC_BACK           0x11C
#define AC_CHAL_BASE      0x112    // 0x112.. challenge launches
#define AC_EXTRA_BASE     0x120    // 0x120.. extra launches

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
static char g_pendingChain[64];
static int g_legacyGuards = 0;
static int g_chainTrace = 0;
static int g_diffTrace = 0;
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

struct Entry { const char* title; const char* lvl; };
static const Entry CHALLENGE[] = {
    { "Horde - 15 Enemies",   "spawn_easy.lvl" },
    { "Horde - 25 Enemies",   "spawn_normal.lvl" },
    { "Horde - 50 Enemies",   "spawn_hard.lvl" },
    { "Horde - 100 Enemies",  "spawn_extreme.lvl" },
    { "Mansion Battle - 15 Enemies", "hub1.lvl" },
    { "Mansion Battle - 25 Enemies", "hub2.lvl" },
    { "Mansion Battle - 50 Enemies", "hub3.lvl" },
    { "Mansion Battle - 100 Enemies", "hub4.lvl" },
    { "Survival - Infinite",  "spawn_infinite.lvl" },
};
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

static bool g_challengeSession;
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
static void __declspec(naked) HookChallengeExit() {
    BR2_SAVE_CPU
    __asm { call FinishStandaloneSession }
    __asm { mov [esp+556],eax }
    BR2_RESTORE_CPU
    __asm {
        test eax,eax
        jz normal
        xor eax,eax
        ret
    normal:
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
typedef const char* (__cdecl *Localize_t)(const char*);
static Localize_t Localize = (Localize_t)FN_LOCALIZE;
static int  g_killCount;
static char g_hudLvl[64];
static char g_hudBuf[64];
static char g_hudShown[64];
static char g_killsLbl[40];
static char g_doneLbl[48];
// Memory-only counters for read-only diagnostics; no per-frame log/file I/O.
static volatile unsigned g_hudRenderTicks;
static volatile unsigned g_hudWrites;
static volatile unsigned g_objectiveReads;
static volatile int g_objectiveSpawned, g_objectiveAlive, g_objectiveTarget;
static volatile DWORD g_objectiveTrigger;

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

// Called immediately BEFORE the native onscreen-text renderer, after scripts
// and the normal timer update. Blank banners cannot overwrite us later in
// the same frame. The script interpreter and the level loader stay native.
static void __cdecl HudTick() {
    ++g_hudRenderTicks;
    TrackHudLevel();
    const char* lvl = (const char*)0x5E33258;
    if (!IsStandaloneChallengeLevel(lvl)) return;
    if (*(DWORD*)0x5EF4BF8 != 0) return;      // any menu open -> not in gameplay
    DWORD gso = *(DWORD*)0x7C07F8;
    if (!gso) return;
    ObjectiveProgress progress;
    ++g_objectiveReads;
    if (!ReadObjectiveProgress(&progress)) {
        g_objectiveTrigger = 0;
        return; // no authoritative wave yet: do not display an estimate
    }
    g_objectiveTrigger = progress.trigger;
    g_objectiveSpawned = progress.spawned;
    g_objectiveAlive = progress.alive;
    g_objectiveTarget = progress.target;
    g_killCount = progress.spawned - progress.alive;
    if (g_killCount<0) g_killCount = 0;
    // The wave objective is authoritative: all emitted kills accounted for and
    // nobody left alive = done. Then we own the slot and repaint the localized
    // completion text, overwriting the script's literal English banner.
    bool done = progress.target > 0 && progress.spawned >= progress.target
                && progress.alive == 0;
    // Yield the slot to script text: if the slot holds something we did not
    // write and it is still alive, leave it be -- except when done, where our
    // localized message replaces whatever the script displayed.
    // All-space strings are our masked-out banners -> treated as empty.
    const char* slot = (const char*)(gso + 0x3F0);
    float     dur  = *(float*)(gso + 0x4F0);
    if (dur < 0.0f || dur > 60.0f) return;    // slot sanity (transitions)
    if (!done && slot[0] && dur > 0.0f && strcmp(slot, g_hudShown)) {
        const char* p = slot;
        while (*p == ' ') ++p;
        if (*p) return;                     // real foreign text -> yield
    }
    // Localize only once, while we are known to be in live gameplay -- calling
    // it mid level-load can walk a string table that is being rebuilt.
    if (!g_killsLbl[0]) {
        const char* label = Localize("Kills");
        strncpy(g_killsLbl, label ? label : "Kills", sizeof g_killsLbl - 1);
        g_killsLbl[sizeof g_killsLbl - 1] = 0;
        const char* msg = Localize("Challenge Completed");
        strncpy(g_doneLbl, msg ? msg : "Challenge Completed", sizeof g_doneLbl - 1);
        g_doneLbl[sizeof g_doneLbl - 1] = 0;
    }
    const char* text = g_hudBuf;
    if (done) text = g_doneLbl;
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
    ++g_hudWrites;
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

// ------------------------- menu build logic --------------------------------
static void BuildChallengeMenu();
static void __cdecl OnMenuBuilt() {
    TraceGlobals("menu-before-injection");
    int id = *(int*)G_MENU_STATE;
    unsigned i;
    switch (id) {
    case MENU_MAIN: {
        AddMenuItem(Localize("Challenge Mode"), AC_CHALLENGE_MENU, 0);
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
            AddMenuItem(Localize(EXTRA[i].title), AC_EXTRA_BASE + i, EXTRA[i].lvl);
        break;
    case MENU_OPTIONS: {
        static char buf[64];
        // "Dhampir" is a proper name: show it verbatim in every language
        // (the native ES string is "Dhampir:" which would read wrong here).
        const char* pname = !_stricmp(PRESETS[g_diff].name, "Dhampir")
                            ? "Dhampir" : Localize(PRESETS[g_diff].name);
        sprintf(buf, "%s: %s", Localize("Difficulty"), pname);
        AddMenuItem(buf, AC_DIFFICULTY, 0);
        break;
    }
    case MENU_CHALLENGE:
        BuildChallengeMenu();
        break;
    }
    TraceGlobals("menu-after-injection");
}

// The native builder prologue runs even for our empty jump-table slot.
// Fill rows only from its epilogue; CallMenu owns selection/stack changes.
static void BuildChallengeMenu() {
    int i;
    *(int*)G_ITEM_COUNT = 0;
    for (i = 0; i < (int)(sizeof(CHALLENGE)/sizeof(CHALLENGE[0])); ++i)
        AddMenuItem(Localize(CHALLENGE[i].title), AC_CHAL_BASE + i, CHALLENGE[i].lvl);
    AddMenuItem(Localize("Back to Main Menu"), AC_BACK, 0);
}

// ------------------------- action dispatch ---------------------------------
// -1 = not ours | 0 = handled -> jmp 0x56913E | 1 = launch -> jmp 0x56914A
static int __cdecl DispatchOurs(int action) {
    Trace("ACTION code=%X",action);
    TraceGlobals("action");
    if (action == AC_CHALLENGE_MENU) { CallMenu(MENU_CHALLENGE); return 0; }
    if (action == AC_BACK)           { MenuBack();              return 0; }
    if (action == AC_DIFFICULTY) {
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
    const Entry* e = NULL;
    int i = action - AC_CHAL_BASE;
    if (i >= 0 && i < (int)(sizeof(CHALLENGE)/sizeof(CHALLENGE[0])))
        e = &CHALLENGE[i];
    i = action - AC_EXTRA_BASE;
    if (!e && i >= 0 && i < (int)(sizeof(EXTRA)/sizeof(EXTRA[0])))
        e = &EXTRA[i];
    if (!e) return -1;
    g_challengeSession = IsStandaloneChallengeLevel(e->lvl);
    // Starting the same challenge again must also reset its session counter.
    g_hudLvl[0] = 0;
    g_hudShown[0] = 0;
    g_killCount = 0;
    // 0x5EF4948 is the picked-level buffer: the state machine (0x569990)
    // stricmp()s it against the chapter name [0x7C2280] and then passes it
    // straight to 0x4F9570 on BOTH branches. 0x706B84 turned out to be a
    // read-only const (".rdata") -- writing it AV'd inside our own dll.
    strcpy((char*)G_PICKED_LVL, e->lvl);
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
        cmp edi, AC_EXTRA_BASE + 0x20
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
    char buf[64];
    GetPrivateProfileStringA("Difficulty", "preset", "Dhampir",
                             buf, sizeof buf, ".\\br2mod.ini");
    for (int i = 0; i < NUM_PRESETS; ++i)
        if (!_stricmp(buf, PRESETS[i].name)) g_diff = i;
}

static DWORD WINAPI Worker(LPVOID) {
    InitTrace();
    LoadConfig();
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
    }
    PatchChecked(0x55EB10,"\x55\x8b\xec\x6a\xff\x68\xf5\x27\x6d\x00",10,HookMenuBegin);
    PatchSite((void*)0x561E1A, 6, HookA);
    PatchSite((void*)0x5670E1, 12, HookB);
    PatchChecked(0x566430,"\xd9\xee\xd8\x1d\xb4\x02\x03\x06",8,HookSavePrompt);
    PatchCallChecked(0x4F83A9,"\xE8\x12\xEE\xFF\xFF",5,HookHudRender);
    PatchCallChecked(0x4F9137,"\xE8\xD4\xE6\xFF\xFF",5,HookChallengeExit);
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
        if (InterlockedExchange(&g_presetDirty, 0))
            WritePrivateProfileStringA("Difficulty", "preset",
                                       PRESETS[g_diff].name, ".\\br2mod.ini");
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
