// Enhanced combat. Lock-on is sampled after native keyboard/controller mapping.
// Resolve the contact inside Rayne's melee handler. The time manager slows
// the attacker in its current animation.
namespace {

#include "enhanced_settings.inl"

constexpr unsigned CharacterTimeChannelOffset = 0x590;
bool g_enhancedGameplay;
void ResetCombatEffects();
void TickCombat(BYTE* rayne, DWORD now, float gameDeltaMs);
bool TickFeedEscape(BYTE* rayne, bool pressed, float gameDeltaMs);
bool SuppressFeedEscapeHit(BYTE* rayne, BYTE* hit);
float LocalHitstopRate(void* channel, float delta);
float HitstopWorldRate(DWORD now);
float ShowcaseWorldRate(DWORD now);
float FeedExecutionWorldRate(DWORD now);
void FlushCombatImpacts();
void MarkShowcaseParry(BYTE* rayne, const BYTE* attacker, DWORD now);
void CombatSound(BYTE* actor, const char* sample);

struct DeflectedHit {
    BYTE* attacker;
    DWORD id, tick;
};
DeflectedHit g_deflectedHit; // the attacker currently receiving the slowdown

struct IncomingHit {
    const BYTE* attacker;
    DWORD id, at;
    bool deflected;
    bool receivedHitstopSeen;
    float healthBefore;
};
IncomingHit g_incomingHits[256];
unsigned g_incomingCursor;

void ResetCombatIncoming() {
    memset(g_incomingHits, 0, sizeof g_incomingHits);
    g_incomingCursor = 0;
}

IncomingHit* FindIncoming(const BYTE* attacker, DWORD id) {
    if (!id) return NULL;
    for (unsigned n = 0; n < min(g_incomingCursor, (unsigned)_countof(g_incomingHits)); ++n) {
        auto& entry = g_incomingHits[(g_incomingCursor - 1 - n) % _countof(g_incomingHits)];
        if (entry.id == id && entry.attacker == attacker) return &entry;
    }
    return NULL;
}

IncomingHit* RememberIncoming(const BYTE* attacker, DWORD id, DWORD now, bool deflected) {
    if (!id) return NULL;
    if (auto* existing = FindIncoming(attacker, id)) return existing;
    auto& entry = g_incomingHits[g_incomingCursor++ % _countof(g_incomingHits)];
    entry = {attacker, id, now, deflected};
    return &entry;
}
bool g_parryTargetLogged;
float g_combatFxScale = 1.0f;
DWORD g_parryPressedAt;
float g_parryAgeSeconds;
float g_parryNativeWorldFactor = 1.0f;
float g_parryPulseFactor = 1.0f;
float g_combatWorldRate = 1.0f;
bool g_globalHitstopApplied;
DWORD g_parryPulseAt;
bool g_parryPulseActive;
bool g_parryPulseLogged;
bool g_parryHeld = true;
bool g_parryArmed;
bool g_parryCooling;
DWORD g_parryCooldownAt;

#ifdef BR2_COMBAT_TEST
struct CombatTraceContext {
    DWORD hit, group;
    const BYTE* actor;
    const BYTE* other;
};
CombatTraceContext g_combatTraceContext;
struct CombatTraceScope {
    CombatTraceContext previous;
    CombatTraceScope(DWORD id, const BYTE* actor, const BYTE* other, DWORD group)
        : previous(g_combatTraceContext) { g_combatTraceContext = {id, group, actor, other}; }
    ~CombatTraceScope() { g_combatTraceContext = previous; }
};
#define TRACE_COMBAT_SCOPE(id, actor, other, group) CombatTraceScope traceScope(id, actor, other, group)

struct CombatEvent {
    const char* result;
    const char* detail;
    DWORD tick, frame, age, vtable, hitId, actor, other, group;
    int kind, severity, state;
    float combatMs, worldFactor, a, b, c;
    unsigned long long profile;
    LARGE_INTEGER counter;
    char level[64];
};

constexpr unsigned CombatLogCapacity = 8192;
CombatEvent g_combatEvents[CombatLogCapacity];
volatile LONG g_combatRead, g_combatWrite, g_combatLost;
DWORD g_combatFrame;
unsigned long long g_tracedProfile;
bool g_traceWasEnabled;

bool CombatTraceEnabled() { return g_enhanced.trace != 0; }

void LogParry(const char* result, DWORD age = 0, int kind = 0, int severity = 0,
              int state = 0, DWORD vtable = 0, DWORD hitId = 0, const BYTE* actor = NULL,
              const BYTE* other = NULL, DWORD group = 0, float a = 0, float b = 0, float c = 0,
              const char* detail = "") {
    if (!CombatTraceEnabled()) return;
    const unsigned write = (unsigned)InterlockedCompareExchange(&g_combatWrite, 0, 0);
    const unsigned read = (unsigned)InterlockedCompareExchange(&g_combatRead, 0, 0);
    if (write - read >= CombatLogCapacity) {
        InterlockedIncrement(&g_combatLost);
        return;
    }
    auto& e = g_combatEvents[write % CombatLogCapacity];
    const auto& ctx = g_combatTraceContext;
    e = {result, detail, GetTickCount(), g_combatFrame, age, vtable, hitId ? hitId : ctx.hit,
         (DWORD)(actor ? actor : ctx.actor), (DWORD)(other ? other : ctx.other),
         group ? group : ctx.group, kind, severity, state, g_parryAgeSeconds * 1000.0f, g_parryNativeWorldFactor,
         a, b, c, g_combatProfile};
    QueryPerformanceCounter(&e.counter);
    strncpy_s(e.level, (const char*)G_CURRENT_LEVEL, _TRUNCATE);
    InterlockedIncrement(&g_combatWrite);
}

void TraceCombatFrame() {
    ++g_combatFrame;
    if (!CombatTraceEnabled()) {
        g_traceWasEnabled = false;
        return;
    }
    if (g_traceWasEnabled && g_tracedProfile == g_combatProfile) return;
    g_traceWasEnabled = true;
    g_tracedProfile = g_combatProfile;
    LogParry("config-begin");
    ForEachCombatSetting([](const char* key, float value) {
        LogParry(key, 0, 0, 0, 0, 0, 0, NULL, NULL, 0, value);
    });
    LogParry("config-end");
}

void FlushParryLog() {
    static bool headerWritten;
    static LARGE_INTEGER origin, frequency;
    unsigned read = (unsigned)InterlockedCompareExchange(&g_combatRead, 0, 0);
    const unsigned write = (unsigned)InterlockedCompareExchange(&g_combatWrite, 0, 0);
    if (read == write) return;
    FILE* file;
    if (fopen_s(&file, "br2mod_combat_test.log", "a") != 0) return;
    if (!headerWritten) {
        origin = g_combatEvents[read % CombatLogCapacity].counter;
        QueryPerformanceFrequency(&frequency);
        SYSTEMTIME utc;
        GetSystemTime(&utc);
        fprintf(file, "\nBR2-COMBAT-TEST v2 pid=%lu UTC=%04u-%02u-%02uT%02u:%02u:%02uZ\n",
                GetCurrentProcessId(), utc.wYear, utc.wMonth, utc.wDay,
                utc.wHour, utc.wMinute, utc.wSecond);
        headerWritten = true;
    }
    const LONG lost = InterlockedExchange(&g_combatLost, 0);
    if (lost) fprintf(file, "TRACE-OVERFLOW lost=%ld\n", lost);
    for (; read != write; ++read) {
        const auto& e = g_combatEvents[read % CombatLogCapacity];
        const auto micros = (unsigned long long)((e.counter.QuadPart - origin.QuadPart) *
                                                   (1000000.0 / frequency.QuadPart));
        fprintf(file, "seq=%u frame=%lu us=%llu tick=%lu level=\"%s\" profile=%016llX event=%s age_ms=%lu type=%02X severity=%d state=%X vt=%08lX hit=%lu actor=%08lX other=%08lX group=%lu combat_ms=%.2f world=%.9g a=%.9g b=%.9g c=%.9g detail=\"%s\"\n",
                read + 1, e.frame, micros, e.tick, e.level, e.profile, e.result, e.age, e.kind,
                e.severity, e.state, e.vtable, e.hitId, e.actor, e.other, e.group,
                e.combatMs, e.worldFactor, e.a, e.b, e.c, e.detail);
    }
    if (fclose(file) == 0) InterlockedExchange(&g_combatRead, (LONG)read);
}
#else
#define LogParry(...) ((void)0)
#define TRACE_COMBAT_SCOPE(...) ((void)0)
constexpr bool CombatTraceEnabled() { return false; }
void TraceCombatFrame() {}
void FlushParryLog() {}
#endif

void ResetParry() {
    if (g_enhancedGameplay) LogParry("combat-reset");
    if (g_parryPulseLogged)
        LogParry("parry-pulse-cancelled", GetTickCount() - g_parryPulseAt, 0, 0, 0, 0,
                 g_deflectedHit.id, g_deflectedHit.attacker);
    if (g_parryArmed) {
        g_parryCooling = true;
        g_parryCooldownAt = GetTickCount();
    }
    g_deflectedHit = {};
    g_parryAgeSeconds = 0.0f;
    g_parryPulseActive = false;
    g_parryPulseLogged = false;
    g_parryPulseFactor = 1.0f;
    g_combatWorldRate = 1.0f;
    g_globalHitstopApplied = false;
    g_parryHeld = true; // require a release after a menu or level transition
    g_parryArmed = false;
    g_enhancedGameplay = false;
    ResetCombatEffects();
}

bool EnhancedCombatActive() {
    return g_enhancedGameplay && g_combat == CombatEnhanced &&
           !*(int*)G_MENU_STATE && !g_photoBlockingMenu;
}

bool ParryWindowOpen(DWORD ageMs) {
    return g_parryArmed && ageMs <= (DWORD)g_enhanced.parryMaxInputMs &&
           g_parryAgeSeconds * 1000.0f <= g_enhanced.parryWindowMs;
}

// Sample the unmodified world factor before applying the impact pulse.
// Actor channels also keep their own native factor (powers, scripts, etc.).
// Scale the returned values, never the time manager's persistent state.
float __fastcall SampleCombatWorldFactor(void* manager, const BYTE* game, int channel) {
    const float native = ((float (__thiscall*)(void*, int))0x649780)(manager, channel);
    g_parryNativeWorldFactor = native;
    if (!EnhancedCombatActive()) return native;
    const DWORD now = GetTickCount();
    FlushCombatImpacts();

    g_parryPulseFactor = 1.0f;
    const DWORD elapsed = now - g_parryPulseAt;
    const DWORD hold = g_enhanced.parrySlowHoldMs;
    const DWORD end = hold + g_enhanced.parrySlowFadeMs;
    if (!g_enhanced.parryEnabled || elapsed >= end) g_parryPulseActive = false;
    if (!g_parryPulseActive && g_parryPulseLogged) {
        LogParry("parry-pulse-end", elapsed, 0, 0, 0, 0, g_deflectedHit.id, g_deflectedHit.attacker);
        g_parryPulseLogged = false;
    }
    if (g_parryPulseActive) {
        if (!g_parryPulseLogged) {
            g_parryPulseLogged = true;
            LogParry("pulse-started", 0, 0, 0, 0, 0, g_deflectedHit.id, g_deflectedHit.attacker, NULL, 0, g_enhanced.parryWorldRate);
        }
        g_parryPulseFactor = g_enhanced.parryWorldRate;
        if (elapsed > hold) {
            const float recovery = float(elapsed - hold) / g_enhanced.parrySlowFadeMs;
            g_parryPulseFactor += (1.0f - g_enhanced.parryWorldRate) * recovery;
        }
    }
    const float finisher = ShowcaseWorldRate(now);
    const float execution = FeedExecutionWorldRate(now);
    const float hitstop = HitstopWorldRate(now);
    g_globalHitstopApplied = hitstop < 1.0f;
    g_combatWorldRate = min(hitstop, min(g_parryPulseFactor, min(finisher, execution)));
    const float world = native * g_combatWorldRate;
    const int frameMs = *(const int*)(game + 0x3D4);
    // The world clock truncates to whole milliseconds. Preserve a positive
    // native step here while character channels keep their fractional delta.
    if (g_globalHitstopApplied && native > 0.0f && frameMs > 0)
        return max(world, min(native, 1.0001f / frameMs));
    return world;
}

void __declspec(naked) HookParryWorldFactor() {
    __asm {
        mov edx, edi
        jmp SampleCombatWorldFactor
    }
}

float __fastcall HookParryActorFactor(void* manager, void*, void* channel) {
    const float native = ((float (__thiscall*)(void*, void*))0x649640)(manager, channel);
    if (!EnhancedCombatActive() || *(BYTE*)0x5E335BE)
        return native;
    return native * g_combatWorldRate;
}

float __fastcall HookEnhancedActorUpdate(void* channel, void*) {
    const float delta = ((float (__thiscall*)(void*))0x6498E0)(channel);
    if (!EnhancedCombatActive() || *(BYTE*)0x5E335BE)
        return delta;

    // Apply local hitstop only at actor updates. Other reads of the native
    // delta divide by it when calculating knockback or contact impulses.
    float rate = 1.0f;
    if (!g_globalHitstopApplied && g_parryPulseActive && g_deflectedHit.attacker &&
        (DWORD)channel == (DWORD)g_deflectedHit.attacker + CharacterTimeChannelOffset) {
        rate = g_enhanced.parryAttackerRate;
        if (!g_parryTargetLogged) {
            g_parryTargetLogged = true;
            LogParry("attacker-slowed", 0, 0, 0, 0, 0,
                     g_deflectedHit.id, g_deflectedHit.attacker);
        }
    }
    const float hitstop = LocalHitstopRate(channel, delta);
    const float adjusted = delta * min(rate, hitstop / g_combatWorldRate);
    return adjusted;
}

void __fastcall HookParryInput(BYTE* game, void*) {
    ((void (__thiscall*)(BYTE*))0x4F8820)(game);
    BYTE* rayne = *(BYTE**)0x7DCD68;
    if (g_combat != CombatEnhanced || !rayne || !*(void**)G_SITUATION || g_photoBlockingMenu ||
        *(int*)G_MENU_STATE || *(int*)(game + 0x324) || *(int*)(game + 0x39C) ||
        *(int*)(rayne + 0x3A48) < 0) {
        if (g_enhancedGameplay) ResetParry();
        return;
    }

    SampleEnhancedSettings();
    g_enhancedGameplay = true;
    const DWORD now = GetTickCount();
    TraceCombatFrame();
    // The native world delta already includes time powers and global hitstop.
    const float gameDeltaMs = max(0.0f, *(const float*)(game + 0x3D8) * 1000.0f);
    TickCombat(rayne, now, gameDeltaMs);
    // Native state 1 is held. Ignored presses never re-arm on their own.
    const bool held = *(int*)(rayne + 0x3A70) == 1;
    const bool pressed = held && !g_parryHeld;
    g_parryHeld = held;
    if (g_parryArmed) {
        const float rawDelta = *(float*)(game + 0x3DC);
        if (rawDelta > 0.0f)
            g_parryAgeSeconds += rawDelta * max(g_enhanced.parryClockFloor, g_parryNativeWorldFactor);
        if (!ParryWindowOpen(now - g_parryPressedAt)) {
            g_parryArmed = false;
            g_parryCooling = true;
            g_parryCooldownAt = now;
            LogParry("parry-expired", now - g_parryPressedAt);
        }
    }
    if (g_parryCooling && now - g_parryCooldownAt >= (DWORD)g_enhanced.parryCooldownMs)
        g_parryCooling = false;
    if (!TickFeedEscape(rayne, pressed, gameDeltaMs) && pressed && g_enhanced.parryEnabled) {
        if (!g_parryArmed && !g_parryCooling && g_enhanced.parryWindowMs) {
            g_parryPressedAt = now;
            g_parryAgeSeconds = 0.0f;
            g_parryArmed = true;
            LogParry("press", 0, 0, 0, *(int*)(rayne + 0x17C));
        } else {
            LogParry("press-cooldown");
        }
    }
    const DWORD pulseDuration = g_enhanced.parrySlowHoldMs + g_enhanced.parrySlowFadeMs;
    if (g_deflectedHit.attacker &&
        now - g_deflectedHit.tick > max((DWORD)g_enhanced.parryRepeatMs, pulseDuration))
        g_deflectedHit = {};
}

bool CanParryAttacker(BYTE* actor) {
    const DWORD* vtable = *(const DWORD**)actor;
    if (vtable[2] != 0x5C7910 || vtable[0x200 / 4] != 0x5BAB60 ||
        vtable[0x130 / 4] != 0x5BBA50)
        return false;
    return ((int (__thiscall*)(BYTE*))0x5BBA50)(actor) == 0;
}

struct ParryVec { float x, y, z; };

bool CombatEffectCached(const char* name) {
    // The native spawner treats a cache miss as an error. Its lookup takes
    // the name in EDI and returns the cache index or -1.
    int index;
    __asm {
        mov edi, name
        mov eax, 0x4A0C70
        call eax
        mov index, eax
    }
    return index >= 0;
}

void __fastcall HookCombatFxStart(BYTE* system, void*) {
    if (g_combatFxScale != 1.0f) {
        // The spawner has copied this emitter's settings out of the cache.
        // 0x5B7B46 reads start size, variation and end size before emission.
        *(float*)(system + 0xA4) *= g_combatFxScale;
        *(float*)(system + 0xA8) *= g_combatFxScale;
        *(float*)(system + 0xAC) *= g_combatFxScale;
    }
    ((void (__thiscall*)(BYTE*))0x5B8390)(system);
}

void CombatShake(float strength, int durationMs) {
    if (strength <= 0.0f || durationMs <= 0) return;
    void* view = *(void**)0x7C0A0C;
    if (!view) return;
    const BYTE* state = (const BYTE*)view;
    if (*(const float*)(state + 0x22C) > 0.0f && *(const float*)(state + 0x228) > strength)
        return; // a lighter impact cannot replace a stronger active shake
    ((void (__thiscall*)(void*, float, float))0x4FA2A0)(view, strength, durationMs / 1000.0f);
    LogParry("camera-shake", durationMs, 0, 0, 0, 0, 0, NULL, NULL, 0, strength);
}

bool SpawnCombatFx(const char* name, const ParryVec& point, const ParryVec& normal,
                   float scale, int lifetimeMs) {
    if (!CombatEffectCached(name)) {
        LogParry("fx-not-cached", 0, 0, 0, 0, 0, 0, NULL, NULL, 0, 0, 0, 0, name);
        return false;
    }
    const ParryVec zero = {};
    typedef DWORD (__cdecl* SpawnEffect)(const char*, const ParryVec*, const ParryVec*,
                                       const ParryVec*, const ParryVec*, float);
    const float previous = g_combatFxScale;
    g_combatFxScale = scale;
    const DWORD handle = ((SpawnEffect)0x4A2DA0)(name, &point, &normal, &zero, &zero, lifetimeMs / 1000.0f);
    g_combatFxScale = previous;
    LogParry(handle ? "fx-spawned" : "fx-pool-full", lifetimeMs, 0, 0, 0, 0,
             0, NULL, NULL, 0, scale, (float)handle, 0, name);
    if (handle) LogParry("fx-position", 0, 0, 0, 0, 0, 0, NULL, NULL, 0,
                         point.x, point.y, point.z, name);
    return handle != 0;
}

ParryVec CombatImpactPoint(BYTE* victim, const BYTE* hit) {
    ParryVec point;
    ((void (__thiscall*)(BYTE*, ParryVec*, const BYTE*))0x402D70)(victim, &point, hit + 0x28);
    return point;
}

void ParryFeedback(BYTE* rayne, const BYTE* hit) {
    if (g_enhanced.parrySound) CombatSound(rayne, "imp_blade_ME_?.wav");

    CombatShake(g_enhanced.parryShake, g_enhanced.parryShakeMs);
    if (!g_enhanced.parrySpark) return;
    const ParryVec point = CombatImpactPoint(rayne, hit);
    const ParryVec& source = *(const ParryVec*)(hit + 0x14);
    ParryVec normal = {point.x - source.x, point.y - source.y, point.z - source.z};
    ((void (__thiscall*)(ParryVec*))0x406C60)(&normal);
    if (!SpawnCombatFx(*(const char**)0x754F74, point, normal, g_enhanced.parrySparkScale,
                       g_enhanced.parrySparkMs))
        SpawnCombatFx(*(const char**)0x754FD0, point, normal, 1.0f, g_enhanced.parryFallbackSparkMs);
}

int __cdecl TryParry(BYTE* rayne, BYTE* hit) {
    if (*(BYTE**)0x7DCD68 != rayne || !EnhancedCombatActive() ||
        !CombatEffectEnabled(EffectParry) || !g_enhanced.parryEnabled) return 0;
    if (SuppressFeedEscapeHit(rayne, hit)) return 1;

    const DWORD now = GetTickCount();
    const DWORD age = now - g_parryPressedAt;
    const int kind = *(const int*)hit;
    const DWORD hitId = *(const DWORD*)(hit + 4);
    const int severity = *(const int*)(hit + 8);
    const int state = *(int*)(rayne + 0x17C);
    BYTE* attacker = *(BYTE* const*)(hit + 0x24);
    const bool melee = kind == 0x67 || kind == 0x84;
    const IncomingHit* previous = FindIncoming(attacker, hitId);
    if (previous || (hitId && hitId == *(DWORD*)(rayne + 0xF4))) {
        const bool repeat = previous && previous->deflected &&
            now - previous->at <= (DWORD)g_enhanced.parryRepeatMs;
        LogParry(repeat ? "parry-repeat-blocked" : "incoming-already-resolved",
                 age, kind, severity, state, 0, hitId, attacker, rayne);
        return repeat ? 1 : 0;
    }
    const DWORD vtable = CombatTraceEnabled() && attacker ? ReadWord((DWORD)attacker) : 0;
    const char* rejected = NULL;
    if (!melee)
        rejected = "other-hit-type";
    else if (!hitId || !ParryWindowOpen(age))
        rejected = "outside-window";
    else if (severity < g_enhanced.parryMinSeverity || severity > g_enhanced.parryMaxSeverity || !(*(const float*)(hit + 0xC) > 0.0f) ||
        *(void**)(rayne + 0x83C0) || *(void**)(rayne + 0x8374) ||
        !(state == 0 || state == 0x14 || state == 0x180 ||
          ((BYTE (__cdecl*)(int))0x5498A0)(state)) ||
        ((bool (__thiscall*)(BYTE*))0x660DA0)(rayne))
        rejected = "ineligible-state";
    if (!rejected && (!attacker || !*(void**)G_SITUATION || !CanParryAttacker(attacker)))
        rejected = "unsupported-attacker";
    if (!rejected && ((int (__thiscall*)(BYTE*, const BYTE*))0x668890)(rayne, hit) != 1)
        rejected = "not-frontal";
    LogParry(rejected ? rejected : "accepted", age, kind, severity, state, vtable, hitId, attacker,
             rayne, hitId, *(const float*)(hit + 0xC));
    if (rejected)
        return 0;

    g_parryArmed = false;
    TRACE_COMBAT_SCOPE(hitId, attacker, rayne, hitId);
    RememberIncoming(attacker, hitId, now, true);
    g_deflectedHit = {attacker, hitId, now};
    g_parryTargetLogged = false;
    g_parryCooldownAt = g_deflectedHit.tick;
    g_parryCooling = true;
    g_parryPulseAt = g_deflectedHit.tick;
    g_parryPulseLogged = false;
    g_parryPulseActive = true;
    if (state == 0x79 || state == 0x7A || (state >= 0x9B && state <= 0xA2))
        ((void (__thiscall*)(BYTE*, int))0x415600)(rayne + 0x158, 0x181);
    MarkShowcaseParry(rayne, attacker, g_deflectedHit.tick);
    ParryFeedback(rayne, hit);
    return 1;
}

void __declspec(naked) HookParryContact() {
    __asm {
        pushfd
        cmp g_enhancedGameplay, 0
        je inactive
        popfd
    }
    BR2_SAVE_CPU
    __asm {
        push dword ptr [esp + 544] // original EBX, the hit message
        push esi // Rayne
        call TryParry
        add esp, 8
        test eax, eax
        jnz accepted
    }
    BR2_RESTORE_CPU
    __asm {
        jmp stock
    inactive:
        popfd
    stock:
        push ebx
        mov ecx, esi
        push 0x55120E
        push 0x668CF0
        ret
    accepted:
    }
    BR2_RESTORE_CPU
    __asm {
        // Same stack as the stock early exit. Skip damage and guard recoil.
        push 0x551B30
        ret
    }
}

} // namespace
