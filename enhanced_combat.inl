// Enhanced effects are driven by the game thread and never write global time factors.
namespace {

constexpr DWORD RayneActor = 0x7DCD68;
constexpr int DamageBullet = 0x65, DamageBlade = 0x67, DamageHarpoon = 0x6F;
constexpr int DamageFire = 0x66;
constexpr int DamageExplosion = 0x70, DamageBladeHeavy = 0x7D, DamageKick = 0x84;
constexpr unsigned AttackButtons[] = {0x3A88, 0x3A90};

struct ImpactKey { DWORD id; const BYTE* target; bool special; };
struct BaseImpact {
    BYTE* rayne;
    BYTE* victim;
    DWORD id, group;
    int type, severity;
    bool scatter;
};
BaseImpact g_baseImpacts[64];
unsigned g_baseImpactCount;
ImpactKey g_recentImpacts[64];
unsigned g_impactCursor;
struct ScatterHit { DWORD id, volley; };
ScatterHit g_scatterHits[128];
unsigned g_scatterCursor;
DWORD g_gunShotId;
DWORD g_rangedAt;
bool g_hasRangedAt;
int g_comboHits;
DWORD g_comboAt;
bool g_recoveryActive;
DWORD g_recoveryAt;
#ifdef BR2_COMBAT_TEST
bool g_tracedCombatHudFont;
#endif

enum HitstopKind { HitstopImpact, HitstopSpecial, HitstopReceived };

struct Hitstop {
    const BYTE* target;
    DWORD startAt, duration, id, group;
    unsigned sampled;
    HitstopKind kind;
    bool worldPending;
#ifdef BR2_COMBAT_TEST
    unsigned tracedPose, tracedUpdate;
#endif
};
Hitstop g_hitstop;

bool HitstopActive(DWORD now) {
    return g_enhanced.hitstopEnabled && g_hitstop.duration &&
        ((g_enhanced.hitstopGlobal && g_hitstop.worldPending) || now - g_hitstop.startAt < g_hitstop.duration) &&
        (g_hitstop.kind != HitstopReceived || g_enhanced.receivedHitstopEnabled);
}

float HitstopWorldRate(DWORD now) {
    if (!g_enhanced.hitstopGlobal || !HitstopActive(now)) return 1.0f;
    const bool received = g_hitstop.kind == HitstopReceived;
    const float rate = received ? g_enhanced.receivedHitstopWorldRate : g_enhanced.hitstopWorldRate;
    if (g_hitstop.worldPending) {
        const DWORD delay = now - g_hitstop.startAt;
        g_hitstop.startAt = now;
        g_hitstop.worldPending = false;
        LogParry("hitstop-world", delay, 0, 0, 0, 0, g_hitstop.id, g_hitstop.target,
                 *(BYTE**)RayneActor, g_hitstop.group, rate, (float)g_hitstop.duration, 0,
                 received ? "received" : "outgoing");
    }
    return rate;
}

float HitstopRateFor(bool rayne) {
    if (g_hitstop.kind == HitstopReceived)
        return rayne ? g_enhanced.receivedHitstopRayneRate : g_enhanced.receivedHitstopAttackerRate;
    return rayne ? g_enhanced.hitstopRayneRate : g_enhanced.hitstopTargetRate;
}

const BYTE* g_harpoonTarget;
DWORD g_harpoonBufferedAt;
unsigned g_harpoonBufferedButton;
bool g_harpoonHeld[2];

void ResetShowcase();
void TickShowcase(BYTE* rayne, DWORD now, float gameDeltaMs);
void DamageShowcase();
void ResetCombatCinematic();
void ResetFeedEscape();
float FeedEscapeAnimationRate(const BYTE* rayne);

#include "feed_execution.inl"

void ResetCombatEffects() {
    ResetShowcase();
    ResetFeedExecution();
    ResetFeedEscape();
    ResetCombatCinematic();
#ifdef BR2_COMBAT_TEST
    g_tracedCombatHudFont = false;
#endif
    g_hitstop = {};
    g_baseImpactCount = 0;
    g_comboHits = 0;
    g_recoveryActive = false;
    g_harpoonTarget = NULL;
    g_harpoonBufferedButton = 0;
    memset(g_harpoonHeld, 0, sizeof g_harpoonHeld);
    memset(g_recentImpacts, 0, sizeof g_recentImpacts);
    g_impactCursor = 0;
    g_gunShotId = 0;
    g_scatterCursor = 0;
    g_hasRangedAt = false;
    memset(g_scatterHits, 0, sizeof g_scatterHits);
}

void TickCombat(BYTE* rayne, DWORD now, float gameDeltaMs) {
    TickShowcase(rayne, now, gameDeltaMs);
    // Only skip the animation fallback when this update used the scaled actor delta.
    g_hitstop.sampled = 0;
#ifdef BR2_COMBAT_TEST
    if (g_hitstop.duration && !HitstopActive(now) && g_hitstop.id) {
        LogParry("hitstop-end", now - g_hitstop.startAt, 0, 0, 0, 0, g_hitstop.id,
                 g_hitstop.target, rayne, g_hitstop.group);
        g_hitstop.id = 0;
    }
#endif
    if (g_comboHits && (!g_enhanced.comboEnabled ||
        now - g_comboAt >= (DWORD)g_enhanced.comboWindowMs))
        g_comboHits = 0;

    // Store only a new press during a confirmed pull. Feed it to the native
    // input handler on return to a controllable state, which chooses the move.
    const auto target = *(BYTE**)(rayne + 0x8374);
    for (unsigned i = 0; i < _countof(AttackButtons); ++i) {
        const bool held = *(int*)(rayne + AttackButtons[i]) == 1;
        if (g_enhanced.harpoonBufferEnabled && g_harpoonTarget && target == g_harpoonTarget &&
            held && !g_harpoonHeld[i]) {
            g_harpoonBufferedAt = now;
            g_harpoonBufferedButton = AttackButtons[i];
            LogParry("harpoon-buffer-armed", 0, 0, 0, AttackButtons[i], 0, 0, target, rayne);
        }
        g_harpoonHeld[i] = held;
    }
    if (!g_enhanced.harpoonBufferEnabled || !g_enhanced.harpoonComboBufferMs ||
        now - g_harpoonBufferedAt >= (DWORD)g_enhanced.harpoonComboBufferMs)
        g_harpoonBufferedButton = 0;
    if (target || *(int*)(rayne + 0x836C)) return;
    g_harpoonTarget = NULL;
    if (!g_harpoonBufferedButton || *(void**)(rayne + 0x83C0)) return;
    const int state = *(int*)(rayne + 0x17C);
    if (state != 0 && state != 0x14 && state != 0x180 &&
        !((BYTE (__cdecl*)(int))0x5498A0)(state)) return;
    int& button = *(int*)(rayne + g_harpoonBufferedButton);
    if (!*(int*)(rayne + AttackButtons[0]) && !*(int*)(rayne + AttackButtons[1])) {
        button = 1;
        LogParry("harpoon-buffer-played", now - g_harpoonBufferedAt, 0, 0, g_harpoonBufferedButton, 0, 0, rayne);
    }
    g_harpoonBufferedButton = 0;
}

float LocalHitstopRate(void* channel, float delta) {
    if (g_enhanced.hitstopGlobal || !HitstopActive(GetTickCount())) return 1.0f;
    const auto rayne = *(const BYTE**)RayneActor;
    const bool isRayne = (DWORD)channel == (DWORD)rayne + CharacterTimeChannelOffset;
    if (!isRayne && (DWORD)channel != (DWORD)g_hitstop.target + CharacterTimeChannelOffset) return 1.0f;
    const float rate = HitstopRateFor(isRayne);
    const unsigned bit = isRayne ? 1 : 2;
    if (rate < 1.0f) g_hitstop.sampled |= bit;
#ifdef BR2_COMBAT_TEST
    if (rate < 1.0f && !(g_hitstop.tracedUpdate & bit)) {
        g_hitstop.tracedUpdate |= bit;
        LogParry(isRayne ? "hitstop-rayne" : "hitstop-target", GetTickCount() - g_hitstop.startAt,
                 0, 0, 0, 0, g_hitstop.id, isRayne ? rayne : g_hitstop.target,
                 isRayne ? g_hitstop.target : rayne, g_hitstop.group, rate, delta, 0,
                 g_hitstop.kind == HitstopReceived ? "received" : "outgoing");
    }
#endif
    return rate;
}

void ScheduleHitstop(const BYTE* target, int duration, DWORD now, DWORD id, DWORD group,
                     HitstopKind kind = HitstopImpact) {
    if (!g_enhanced.hitstopEnabled || !duration) return;
    const BYTE* rayne = *(const BYTE**)RayneActor;
    if (!rayne || *(void* const*)(rayne + 0x83C0) || *(void* const*)(rayne + 0x8374)) return;
    const bool upgrade = kind == HitstopSpecial && g_hitstop.kind == HitstopImpact &&
        g_hitstop.group == group && HitstopActive(now);
    const bool replacesReceived = kind != HitstopReceived && g_hitstop.kind == HitstopReceived;
    if (!upgrade && !replacesReceived && g_hitstop.duration &&
        (HitstopActive(now) || now - g_hitstop.startAt < (DWORD)g_enhanced.hitstopGapMs)) {
        LogParry("hitstop-coalesced", duration, 0, 0, 0, 0, id, target, NULL, group,
                 0, 0, 0, kind == HitstopReceived ? "received" : "outgoing");
        return;
    }
    const DWORD start = upgrade ? g_hitstop.startAt : now;
    const unsigned sampled = upgrade ? g_hitstop.sampled & 1 : 0;
    const bool pending = upgrade ? g_hitstop.worldPending : g_enhanced.hitstopGlobal != 0;
    g_hitstop = {target, start, (DWORD)duration, id, group, sampled, kind, pending};
    LogParry(upgrade ? "hitstop-upgrade" : "hitstop-start", duration, NativeCombatPower(rayne), 0, 0, 0, id,
             target, rayne, group, HitstopRateFor(true), HitstopRateFor(false), 0,
             kind == HitstopReceived ? "received" : "outgoing");
}

bool IsMeleeImpact(int type) {
    return type == DamageBlade || type == DamageBladeHeavy || type == DamageKick;
}

BYTE* CombatCharacter(BYTE* actor) {
    return ((BYTE* (__cdecl*)(BYTE*, void*))0x4050C0)(actor, *(void**)0x7DCF20);
}

#include "feed_escape.inl"
#include "combat_cinematic.inl"

bool TwistedWindProtected(const BYTE* rayne) {
    if (!g_enhanced.twistedWindInvulnerabilityEnabled ||
        !g_enhanced.twistedWindInvulnerabilityMs ||
        *(const int*)(rayne + 0x8350) != 0x3D || *(const int*)(rayne + 0x17C) != 0x2F)
        return false;
    // combo_circle_kick runs at 30 fps with both kick ranges authored at frames
    // 14..35. Read the controller's frame so protection tracks native time powers.
    constexpr float firstKickFrame = 14.0f, endKickFrame = 35.0f;
    const float end = min(endKickFrame, firstKickFrame + g_enhanced.twistedWindInvulnerabilityMs * 0.03f);
    const float frame = *(const float*)(rayne + 0x180);
    return frame >= firstKickFrame && frame < end;
}

bool RecoveryEligible(const BYTE* hit) {
    if (!g_enhanced.recoveryEnabled || !g_enhanced.recoveryMs) return false;
    const int type = *(const int*)hit;
    const int severity = *(const int*)(hit + 8);
    if (severity < 1 || severity > g_enhanced.recoveryMaxSeverity) return false;
    if (!(IsMeleeImpact(type) ? g_enhanced.recoveryMelee :
          type == DamageBullet && g_enhanced.recoveryBullets)) return false;
    BYTE* attacker = *(BYTE* const*)(hit + 0x24);
    return attacker && attacker != *(BYTE**)RayneActor && CombatCharacter(attacker);
}

int __cdecl ProtectRecovery(BYTE* rayne, const BYTE* hit, float damage) {
    if (!EnhancedCombatActive() || rayne != *(BYTE**)RayneActor ||
        *(const int*)hit == 0x87 || !(damage > 0.0f)) return 0;
    const DWORD id = *(const DWORD*)(hit + 4);
    const int kind = *(const int*)hit, severity = *(const int*)(hit + 8);
    BYTE* attacker = *(BYTE* const*)(hit + 0x24);
    if (IsMeleeImpact(kind) && TwistedWindProtected(rayne) &&
        attacker && attacker != rayne && CombatCharacter(attacker)) {
        LogParry("twisted-wind-blocked", 0, kind, severity, 0x2F, 0, id, attacker, rayne,
                 0, *(const float*)(rayne + 0x180), damage);
        return 1;
    }
    const DWORD now = GetTickCount();
    auto* incoming = RememberIncoming(attacker, id, now, false);
    if (!g_recoveryActive || now - g_recoveryAt >= (DWORD)g_enhanced.recoveryMs ||
        *(void**)(rayne + 0x83C0) || !RecoveryEligible(hit)) {
        if (incoming && g_enhanced.receivedHitstopEnabled && IsMeleeImpact(kind) && !incoming->deflected)
            incoming->healthBefore = *(float*)(rayne + 0x39C8);
        LogParry("incoming-native", 0, kind, severity, *(int*)(rayne + 0x17C), 0,
                 id, attacker, rayne, 0, damage);
        return 0;
    }
    LogParry("recovery-blocked", now - g_recoveryAt, kind, severity,
             *(int*)(rayne + 0x17C), 0, id, attacker, rayne, 0, damage);
    return 1;
}

void ConfirmReceivedHitstop(BYTE* rayne, const BYTE* hit, DWORD now) {
    const int kind = *(const int*)hit, severity = *(const int*)(hit + 8);
    if (!g_enhanced.hitstopEnabled || !g_enhanced.receivedHitstopEnabled || !IsMeleeImpact(kind)) return;
    const bool heavy = severity >= g_enhanced.receivedHitstopMinSeverity;
    const int duration = heavy ? g_enhanced.receivedHitstopMs : g_enhanced.receivedLightHitstopMs;
    BYTE* attacker = *(BYTE* const*)(hit + 0x24);
    const DWORD id = *(const DWORD*)(hit + 4);
    auto* incoming = FindIncoming(attacker, id);
    if (!incoming || incoming->deflected || incoming->receivedHitstopSeen) return;
    const float healthBefore = incoming->healthBefore;
    incoming->healthBefore = 0.0f;
    const float healthAfter = *(float*)(rayne + 0x39C8);
    if (!duration || !(healthBefore > 0.0f && healthBefore > healthAfter) ||
        !attacker || attacker == rayne || !CombatCharacter(attacker)) return;
    incoming->receivedHitstopSeen = true;
    LogParry("hitstop-received-confirmed", 0, kind, severity, *(int*)(rayne + 0x17C), 0,
             id, attacker, rayne, id, healthBefore, healthAfter, (float)duration, heavy ? "heavy" : "light");
    ScheduleHitstop(attacker, duration, now, id, id, HitstopReceived);
}

void __cdecl ConfirmRayneDamage(BYTE* rayne, const BYTE* hit, float damage) {
    if (!EnhancedCombatActive() || rayne != *(BYTE**)RayneActor || !(damage > 0.0f))
        return;
    LogParry("damage-applied", 0, *(const int*)hit, *(const int*)(hit + 8),
             *(int*)(rayne + 0x17C), 0, *(const DWORD*)(hit + 4), *(BYTE* const*)(hit + 0x24),
             rayne, 0, damage, *(float*)(rayne + 0x39C8), *(float*)(rayne + 0x8360));
    if (g_enhanced.comboBreakOnDamage) g_comboHits = 0;
    FeedEscapeDamageApplied(rayne, hit);
    DamageShowcase();
    g_harpoonBufferedButton = 0;
    const DWORD now = GetTickCount();
    ConfirmReceivedHitstop(rayne, hit, now);
    if (RecoveryEligible(hit)) {
        g_recoveryAt = now;
        g_recoveryActive = true;
        LogParry("recovery-start", 0, *(const int*)hit, *(const int*)(hit + 8),
                 0, 0, *(const DWORD*)(hit + 4), *(BYTE* const*)(hit + 0x24));
    }
}

void __declspec(naked) HookCombatRecoveryGate() {
    __asm {
        cmp dword ptr [ebx], 0x87
        je ignored
        pushfd
        cmp g_enhancedGameplay, 0
        je inactive
        popfd
    }
    BR2_SAVE_CPU
    __asm {
        push dword ptr [ebp - 4]
        push dword ptr [esp + 548] // original EBX, after pushing the damage
        push esi
        call ProtectRecovery
        add esp, 12
        test eax, eax
        jnz blocked
    }
    BR2_RESTORE_CPU
    __asm {
        jmp stock
    inactive:
        popfd
    stock:
        cmp dword ptr [ebx], 0x87 // preserve the native flags at continuation
        push 0x551248
        ret
    blocked:
    }
    BR2_RESTORE_CPU
    __asm {
    ignored:
        push 0x551B30 // EDI has not been pushed yet
        ret
    }
}

void __declspec(naked) HookCombatDamageApplied() {
    __asm {
        pushfd
        cmp g_enhancedGameplay, 0
        je inactive
        popfd
    }
    BR2_SAVE_CPU
    __asm {
        push dword ptr [ebp - 4]
        push dword ptr [esp + 548]
        push esi
        call ConfirmRayneDamage
        add esp, 12
    }
    BR2_RESTORE_CPU
    __asm {
        jmp stock
    inactive:
        popfd
    stock:
        push 0
        push ebx
        mov ecx, esi
        push 0x5515EF
        push 0x4433B0
        ret
    }
}

BYTE* __fastcall HookGunMessageInit(BYTE* message, void*) {
    BYTE* result = ((BYTE* (__thiscall*)(BYTE*))0x405790)(message);
    if (EnhancedCombatActive()) g_gunShotId = *(DWORD*)(message + 4);
    return result;
}

void __fastcall HookScatterProjectile(void* manager, void*, const void* origin, const void* direction,
                                     const BYTE* message, float speed) {
    if (EnhancedCombatActive() && g_gunShotId &&
        *(BYTE* const*)(message + 0x24) == *(BYTE**)RayneActor) {
        g_scatterHits[g_scatterCursor++ % _countof(g_scatterHits)] =
            {*(const DWORD*)(message + 4), g_gunShotId};
    }
    ((void (__thiscall*)(void*, const void*, const void*, const BYTE*, float))0x4E3640)
        (manager, origin, direction, message, speed);
}

void CombatSound(BYTE* actor, const char* sample) {
    const DWORD* vtable = *(const DWORD**)actor;
    const DWORD handle = ((DWORD (__thiscall*)(BYTE*, const char*))vtable[0x24 / 4])(actor, sample);
    LogParry("sound-request", 0, 0, 0, 0, 0, 0, NULL, NULL, 0, (float)handle, 0, 0, sample);
}

#include "combat_showcase.inl"

void DamageShowcase() {
    if (g_enhanced.styleBreakOnDamage) g_style = {};
    g_dodgeOffset = {};
    g_riposteTarget = NULL;
}

void __cdecl ConfirmOutgoingHit(BYTE* rayne, BYTE* victim, const BYTE* hit) {
    if (!EnhancedCombatActive() || rayne != *(BYTE**)RayneActor ||
        !hit[0x68] || !(*(const float*)(hit + 0x6C) > 0.0f) ||
        !victim || victim == rayne || !CombatCharacter(victim)) return;
    const DWORD id = *(const DWORD*)(hit + 4);
    const bool ferrilContact = rayne == g_ferrilContactActor && id == g_ferrilContactId;
    // Claws use the native unarmed collision packet. Classify them as blade
    // impacts for feedback without rewriting the packet seen by the enemy.
    const int type = *(const int*)hit;
    const int severity = *(const int*)(hit + 8);
    const bool melee = IsMeleeImpact(type);
    // Native weapon packet types. Fire counts when the packet carries a weapon.
    const bool ranged = type == DamageBullet || type == DamageExplosion ||
                        (type == DamageFire && *(void* const*)(hit + 0x20));
    if (!id || (!melee && !ranged && type != DamageHarpoon)) return;
    if (melee && !ferrilContact && id != *(DWORD*)(rayne + 0x82E4) && id != *(DWORD*)(rayne + 0x82E8) &&
        id != *(DWORD*)(rayne + 0x82EC) && id != *(DWORD*)(rayne + 0x82F0)) return;
    DWORD attack = id;
    bool scatter = false;
    if (type == DamageBullet) {
        for (const auto& pellet : g_scatterHits) {
            if (pellet.id != id) continue;
            scatter = true;
            if (g_enhanced.comboGroupScatter) attack = pellet.volley;
            break;
        }
    }
    bool firstTarget = true, special = false;
    for (const auto& previous : g_recentImpacts) {
        if (previous.id != attack) continue;
        if (previous.target == victim) {
            LogParry("outgoing-duplicate", 0, type, severity, 0, 0, id, victim, rayne, attack);
            return;
        }
        firstTarget = false;
        special |= previous.special;
    }
    TRACE_COMBAT_SCOPE(id, victim, rayne, attack);
    auto& impact = g_recentImpacts[g_impactCursor++ % _countof(g_recentImpacts)];
    impact = {attack, victim, special};
    LogParry("outgoing-hit", 0, type, severity, *(int*)(rayne + 0x17C), 0,
             id, victim, rayne, attack, *(const float*)(hit + 0x6C), *(float*)(victim + 0x39C8));
    const DWORD now = GetTickCount();
    if (g_enhanced.comboEnabled && (melee || (ranged && g_enhanced.comboRanged) ||
        (type == DamageHarpoon && g_enhanced.comboHarpoonDamage))) {
        if (now - g_comboAt >= (DWORD)g_enhanced.comboWindowMs) g_comboHits = 0;
        if (g_comboHits < INT_MAX) ++g_comboHits;
        g_comboAt = now;
        LogParry("combo-hit", 0, type, severity, g_comboHits, 0, id, victim, rayne, attack);
    }
    const bool claimed = ShowcaseConfirmedHit(rayne, victim, hit, type, severity, now,
                                               attack, firstTarget, special);
    if (claimed) {
        impact.special = true;
        for (unsigned i = 0; i < g_baseImpactCount; ++i)
            if (g_baseImpacts[i].group == attack) g_baseImpacts[i].group = 0;
    } else if (firstTarget && (melee || ranged)) {
        if (melee) {
            const bool heavy = severity >= g_enhanced.heavyMinSeverity || type == DamageBladeHeavy;
            ScheduleHitstop(victim, heavy ? g_enhanced.heavyHitstopMs :
                            type == DamageKick ? g_enhanced.kickHitstopMs : g_enhanced.bladeHitstopMs,
                            now, id, attack);
        }
        if (g_baseImpactCount < _countof(g_baseImpacts))
            g_baseImpacts[g_baseImpactCount++] = {rayne, victim, id, attack, type, severity, scatter};
        else
            LogParry("feedback-queue-full", 0, type, severity, 0, 0, id, victim, rayne, attack);
    }
}

void FlushCombatImpacts() {
    const unsigned count = g_baseImpactCount;
    g_baseImpactCount = 0;
    const DWORD now = GetTickCount();
    for (unsigned i = 0; i < count; ++i) {
        const auto impact = g_baseImpacts[i];
        if (!impact.group || impact.rayne != *(BYTE**)RayneActor) continue;
        TRACE_COMBAT_SCOPE(impact.id, impact.victim, impact.rayne, impact.group);
        const int type = impact.type, severity = impact.severity;
        if (IsMeleeImpact(type)) {
            const bool heavy = severity >= g_enhanced.heavyMinSeverity || type == DamageBladeHeavy;
            const bool kick = type == DamageKick;
            if (kick ? g_enhanced.kickSound : g_enhanced.bladeSound)
                CombatSound(impact.rayne, kick ? "imp_kick_FL_2.wav" : "imp_blade_FL_1.wav");
            CombatShake(heavy ? g_enhanced.heavyShake : kick ? g_enhanced.kickShake :
                        g_enhanced.bladeShake, g_enhanced.meleeShakeMs);
        } else if (g_enhanced.rangedFeedback) {
            if (g_hasRangedAt && now - g_rangedAt < (DWORD)g_enhanced.rangedGapMs) {
                LogParry("ranged-feedback-gap", now - g_rangedAt, type, severity, 0, 0,
                         impact.id, impact.victim, impact.rayne, impact.group);
                continue;
            }
            const bool heavy = type == DamageExplosion || impact.scatter;
            g_hasRangedAt = true;
            g_rangedAt = now;
            if (heavy) ScheduleHitstop(impact.victim, g_enhanced.rangedHeavyHitstopMs,
                                       now, impact.id, impact.group);
            CombatShake(heavy ? g_enhanced.rangedHeavyShake : g_enhanced.rangedLightShake,
                        g_enhanced.rangedShakeMs);
        }
        LogParry("feedback-base", 0, type, severity, 0, 0, impact.id, impact.victim, impact.rayne, impact.group);
    }
}

void __declspec(naked) HookCombatOutgoing() {
    __asm {
        pushfd
        cmp g_enhancedGameplay, 0
        je inactive
        popfd
    }
    BR2_SAVE_CPU
    __asm {
        push dword ptr [esp + 572] // original second argument (hit)
        push dword ptr [esp + 572] // original first argument (victim)
        push ecx                   // attacker (Rayne)
        call ConfirmOutgoingHit
        add esp, 12
    }
    BR2_RESTORE_CPU
    __asm {
        jmp stock
    inactive:
        popfd
    stock:
        push ebp
        mov ebp, esp
        mov eax, dword ptr ds:[0x7DCF20]
        push 0x669358
        ret
    }
}

void __cdecl ConfirmHarpoon(BYTE* rayne, BYTE* target) {
    if (!EnhancedCombatActive() || rayne != *(BYTE**)RayneActor || !CombatCharacter(target))
        return;
    if (g_harpoonTarget == target) return;
    g_harpoonTarget = target;
    g_harpoonBufferedButton = 0;
    LogParry("harpoon-attached", 0, 0, 0, 0, 0, 0, target);
    AddStyle(rayne, StyleHarpoon, g_enhanced.styleHarpoon, GetTickCount());
    if (!g_enhanced.harpoonFeedback) return;
    if (g_enhanced.harpoonSound) CombatSound(rayne, "harpoon-MT1.wav");
    CombatShake(g_enhanced.harpoonShake, g_enhanced.harpoonShakeMs);
}

void __declspec(naked) HookCombatHarpoonPrimary() {
    __asm {
        mov [ebx + 0x8374], esi
        pushfd
        cmp g_enhancedGameplay, 0
        je inactive
        popfd
    }
    BR2_SAVE_CPU
    __asm {
        push esi
        push dword ptr [esp + 548]
        call ConfirmHarpoon
        add esp, 8
    }
    BR2_RESTORE_CPU
    __asm {
        jmp stock
    inactive:
        popfd
    stock:
        push 0x66294E
        ret
    }
}

void __declspec(naked) HookCombatHarpoonSecondary() {
    __asm {
        mov [ebx + 0x8374], edi
        pushfd
        cmp g_enhancedGameplay, 0
        je inactive
        popfd
    }
    BR2_SAVE_CPU
    __asm {
        push edi
        push dword ptr [esp + 548]
        call ConfirmHarpoon
        add esp, 8
    }
    BR2_RESTORE_CPU
    __asm {
        jmp stock
    inactive:
        popfd
    stock:
        push 0x662B50
        ret
    }
}

void DrawCombatCombo(BYTE* game) {
    if (!CombatEffectEnabled(EffectStyleHud) || !EnhancedCombatActive() ||
        (*(int*)(game + 0x390) && *(int*)(game + 0xC))) return;
    const DWORD now = GetTickCount();
    const bool combo = g_enhanced.comboEnabled && g_comboHits >= g_enhanced.comboMinHits &&
        now - g_comboAt < (DWORD)g_enhanced.comboWindowMs;
    if (!combo && (!g_enhanced.styleEnabled || (!g_style.points && g_dodgeOffset.flashMs <= 0.0f))) return;

    // The onscreen-message slot can switch fonts and leave a script's scale
    // behind. Size these two labels like the native default message font.
    BYTE* font = *(BYTE**)0x7E643C;
    const int width = *(int*)0x7BF140, height = *(int*)0x7BF144;
    if (!font || width <= 0 || height <= 0) return;
    const float base = *(float*)(font + 0x2494);
    if (!(base > 0.0f) || !_finite(base)) return;
    const float scale = (18.2f * height / 480.0f) / base;
    const float oldScale = *(float*)(font + 0x2490);
    const int oldHeight = *(int*)(font + 0x2468), oldSpacing = *(int*)(font + 0x2470);
#ifdef BR2_COMBAT_TEST
    if (!g_tracedCombatHudFont && CombatTraceEnabled()) {
        g_tracedCombatHudFont = true;
        LogParry("combat-hud-font", 0, 0, 0, height, 0, 0, font, *(BYTE**)(game + 0x4FC),
                 0, oldScale, scale, base);
    }
#endif
    __try {
        ((void (__thiscall*)(BYTE*, float))0x4F9690)(font, scale);
        if (combo) {
            char label[32];
            sprintf_s(label, "Combo x%d", g_comboHits);
            ((void (__thiscall*)(void*, int, int, DWORD, DWORD, const char*))0x4E6C60)
                (font, (int)(width * g_enhanced.comboX), (int)(height * g_enhanced.comboY),
                 0xFFFFFFFF, 0xFF000000, label);
        }
        DrawStyleRank(font, width, height, now);
    } __finally {
        *(float*)(font + 0x2490) = oldScale;
        *(int*)(font + 0x2468) = oldHeight;
        *(int*)(font + 0x2470) = oldSpacing;
    }
}

} // namespace
