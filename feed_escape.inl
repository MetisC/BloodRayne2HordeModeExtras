// Frontal feeding repel. Keep the native paired shove and use the enemy's
// authored weapon window as the clock for the player's recovery press.
constexpr int FeedRepelAttack = 0x82;

struct FeedEscape {
    BYTE* rayne;
    BYTE* enemy;
    const DWORD* vtable;
    int enemyMotion;
    bool stockPairing;
    float fps, first, end;
    bool bladeFollowUp;
    bool attempted, armed, rolled;
    float travelX, travelZ, travelMs, travelAge;
#ifdef BR2_COMBAT_TEST
    bool windowLogged, groundWaitLogged, attachmentWaitLogged;
    float originX, originZ, forwardX, forwardZ;
#endif
};
FeedEscape g_feedEscape;

void ResetFeedEscape() { g_feedEscape = {}; }

bool FeedEscapeEnabled() {
    return EnhancedCombatActive() && CombatEffectEnabled(EffectParry) &&
           g_enhanced.parryEnabled && g_enhanced.feedEscapeWindowMs;
}

bool FeedEscapeRayneState(int state) {
    if (g_feedEscape.rolled) return state == 0xD2 || state == 0xCD;
    return state == 0xF1 || state == 0xA4 || state == 0xC7 || state == 0xC8;
}

float FeedEscapeAnimationRate(const BYTE* rayne) {
    return g_feedEscape.rolled && g_feedEscape.rayne == rayne && FeedEscapeEnabled() &&
           FeedEscapeRayneState(*(const int*)(rayne + 0x17C)) ? g_enhanced.feedEscapeRollRate : 1.0f;
}

ParryVec* __cdecl HookFeedEscapeRootAngles(ParryVec* angles, const void* rootRotation) {
    const auto convert = (ParryVec* (__cdecl*)(ParryVec*, const void*))0x691EE0;
    ParryVec* result = convert(angles, rootRotation);
    const BYTE* rayne = g_feedEscape.rayne;
    // The recovery clip adds a half-turn through its root rotation track.
    // Remove that yaw before native movement applies it, without turning Rayne afterward.
    if (g_feedEscape.rolled && rayne && rootRotation == rayne + 0x168 &&
        g_enhanced.feedEscapeKeepFacing && FeedEscapeEnabled() &&
        FeedEscapeRayneState(*(const int*)(rayne + 0x17C)))
        result->y = 0.0f;
    return result;
}

bool FeedEscapeEnemyFrame(float& frame) {
    const auto& escape = g_feedEscape;
    const DWORD* vtable;
    const BYTE* pairedRayne = nullptr;
    struct { int state; float frame; } animation;
    // Rayne's knock-off animation event 0x644 clears the pairing through
    // 0x67375E before the enemy's weapon becomes active. Keep the actor captured
    // at the native repel call while its follow-up animation is still active.
    if (!SafeRead(escape.enemy, &vtable, sizeof vtable) || vtable != escape.vtable ||
        (escape.stockPairing &&
         !SafeRead(escape.enemy + 0x3A44, &pairedRayne, sizeof pairedRayne)) ||
        (pairedRayne && pairedRayne != escape.rayne) ||
        !SafeRead(escape.enemy + 0x17C, &animation, sizeof animation) ||
        animation.state != escape.enemyMotion)
        return false;
    frame = animation.frame;
    return true;
}

void __cdecl CaptureFeedEscape(BYTE* enemy, BYTE* rayne, int accepted) {
    if (rayne != *(BYTE**)RayneActor) return;
    ResetFeedEscape();
    LogParry("feed-escape-repel", 0, accepted, 0, *(int*)(rayne + 0x17C),
             (DWORD)*(const DWORD**)enemy, 0, enemy, rayne, 0,
             (float)g_enhanced.feedEscapeWindowMs, (float)g_enhanced.parryEnabled);
    if (!FeedEscapeEnabled() || accepted != 1) return;

    const DWORD* vtable = *(const DWORD**)enemy;
    const bool stockPairing = vtable[0x1B4 / 4] == 0x5BC520;
    if (stockPairing && *(BYTE**)(enemy + 0x3A44) != rayne) return;

    BYTE* skeleton = *(BYTE**)(enemy + 0x178);
    if (!skeleton) return;
    const auto findMotion = (int (__thiscall*)(BYTE*, const char*, int))0x605200;
    const int motion = stockPairing && *(int*)(enemy + 0x17C) == FeedRepelAttack
        ? FeedRepelAttack : findMotion(skeleton, "feedrepel", 0);
    if (motion < 0 || motion >= *(int*)(skeleton + 0x2418) ||
        *(int*)(enemy + 0x17C) != motion) return;
    const BYTE* clips = *(const BYTE**)(skeleton + 0x241C);
    if (!clips) return;
    const BYTE* clip = clips + motion * 0xA4;
    const float fps = *(const float*)(clip + 0x6C);
    const unsigned count = *(const unsigned*)(clip + 0x98);
    struct Range { int name; float first, end; };
    const Range* ranges = *(const Range**)(clip + 0x9C);
    if (!ranges || !count || count > 32 || !(fps > 0.0f && fps <= 240.0f)) return;

    const auto findRange = (int (__thiscall*)(BYTE*, const char*, int))0x605280;
    const int weapon = findRange(skeleton, "weapondamage2", 0);
    const int blade = findRange(skeleton, "bladedamage2", 0);
    const Range* followUp = NULL;
    for (unsigned i = 0; i < count; ++i) {
        const Range& range = ranges[i];
        if (range.name >= 0 && (range.name == weapon || range.name == blade) &&
            range.first >= 0.0f && range.first < range.end && range.end <= 1000.0f &&
            (!followUp || range.first < followUp->first))
            followUp = &range;
    }
    if (!followUp) return;
    g_feedEscape = {rayne, enemy, vtable, motion, stockPairing, fps,
                    followUp->first, followUp->end, followUp->name == blade};
    g_parryArmed = false;
    LogParry("feed-escape-start", 0, 0, 0, motion, (DWORD)vtable,
             0, enemy, rayne, 0, followUp->first, followUp->end, fps);
}

void StartFeedEscapeRoll(BYTE* rayne, int state, float frame) {
    auto& escape = g_feedEscape;
    if (!((int (__thiscall*)(BYTE*))0x4410E0)(rayne)) {
#ifdef BR2_COMBAT_TEST
        if (!escape.groundWaitLogged) {
            escape.groundWaitLogged = true;
            LogParry("feed-escape-ground-wait", 0, 0, 0, state, 0, 0, escape.enemy, rayne,
                     0, *(float*)(rayne + 0x180));
        }
#endif
        return;
    }

    BYTE*& attached = *(BYTE**)(rayne + 0x83C0);
    if (attached && CombatCharacter(attached) != escape.enemy) {
#ifdef BR2_COMBAT_TEST
        if (!escape.attachmentWaitLogged) {
            escape.attachmentWaitLogged = true;
            LogParry("feed-escape-attachment-changed", 0, 0, 0, state, 0, 0, escape.enemy, rayne);
        }
#endif
        return;
    }

    // End the pairing with the stock enemy release and player cleanup. The
    // attacker keeps its current animation, so its follow-up can swing past Rayne.
    // The native knock-off event uses the same character cast and release,
    // including for enemies with a different feedrepel handler.
    if ((escape.stockPairing && *(BYTE**)(escape.enemy + 0x3A44) == rayne) ||
        (!escape.stockPairing && attached)) {
        ((void (__thiscall*)(BYTE*))0x448720)(escape.enemy);
    }
    if (attached) {
        attached = NULL;
        ((void (__thiscall*)(BYTE*))0x661800)(rayne);
    }

    const float x = *(float*)(rayne + 0x38), z = *(float*)(rayne + 0x40);
    float dx = x - *(float*)(escape.enemy + 0x38), dz = z - *(float*)(escape.enemy + 0x40);
    float length = sqrtf(dx * dx + dz * dz);
    // The paired animation may place both actor origins at the same point.
    if (length < 0.25f) {
        dx = -*(float*)(rayne + 0x58);
        dz = -*(float*)(rayne + 0x70);
        length = sqrtf(dx * dx + dz * dz);
    }
    if (length > 0.01f) {
        escape.travelX = dx * g_enhanced.feedEscapeTravelDistance / length;
        escape.travelZ = dz * g_enhanced.feedEscapeTravelDistance / length;
        escape.travelMs = (float)g_enhanced.feedEscapeTravelMs;
    }
#ifdef BR2_COMBAT_TEST
    escape.originX = x;
    escape.originZ = z;
    escape.forwardX = *(float*)(rayne + 0x58);
    escape.forwardZ = *(float*)(rayne + 0x70);
#endif

    // Match the native backward ground roll setup at 0x676611.
    const bool alternate = state == 0xC8;
    const int roll = alternate ? 0xCD : 0xD2;
    __try {
        if (!ActorHasAnim((DWORD)rayne, roll)) {
            LogParry("feed-escape-roll-unavailable", 0, roll, 0, state,
                     0, 0, escape.enemy, rayne, 0, frame, 0, 0);
            return;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        LogParry("feed-escape-roll-unavailable", 0, roll, 0, state,
                 0, 0, escape.enemy, rayne, 0, frame, 0, 0);
        return;
    }
    ((void (__thiscall*)(BYTE*, int, float, float))0x658EB0)(rayne, roll, 0.0f, -1.0f);
    *(int*)(rayne + 0x8350) = alternate ? 0x4F : 0x4B;
    const auto nextEvent = (DWORD (__cdecl*)())0x4030D0;
    if (!alternate) {
        *(DWORD*)(rayne + 0x82EC) = nextEvent();
        *(DWORD*)(rayne + 0x82F0) = nextEvent();
    }
    *(float*)(rayne + 0x82CC) = 0.0f;
    *(DWORD*)(rayne + 0x82F8) = nextEvent();
    escape.rolled = true;
    LogParry("feed-escape-roll", 0, roll, 0, state, 0, 0, escape.enemy, rayne,
             0, frame, escape.first, (float)*(int*)(rayne + 0x17C));
}

void TickFeedEscapeTravel(BYTE* rayne, float gameDeltaMs) {
    auto& escape = g_feedEscape;
    if (escape.travelAge >= escape.travelMs || gameDeltaMs <= 0.0f) return;
    const float before = 1.0f - escape.travelAge / escape.travelMs;
    escape.travelAge = min(escape.travelMs, escape.travelAge + gameDeltaMs);
    const float after = 1.0f - escape.travelAge / escape.travelMs;
    const float step = before * before - after * after;
    // Native collision movement at 0x44BC00 consumes this world-space offset.
    *(float*)(rayne + 0x3FC) += escape.travelX * step;
    *(float*)(rayne + 0x404) += escape.travelZ * step;
}

bool TickFeedEscape(BYTE* rayne, bool pressed, float gameDeltaMs) {
    auto& escape = g_feedEscape;
    if (!escape.rayne) return false;
    const int state = *(int*)(rayne + 0x17C);
    if (escape.rayne != rayne || !FeedEscapeEnabled() ||
        !FeedEscapeRayneState(state)) {
#ifdef BR2_COMBAT_TEST
        if (escape.rayne == rayne && escape.rolled) {
            const float x = *(float*)(rayne + 0x58), z = *(float*)(rayne + 0x70);
            LogParry("feed-escape-facing-end", 0, g_enhanced.feedEscapeKeepFacing, 0, state,
                     0, 0, escape.enemy, rayne, 0, x * escape.forwardX + z * escape.forwardZ, x, z);
        }
#endif
        LogParry("feed-escape-state-exit", 0, escape.armed, escape.rolled, state,
                 0, 0, escape.enemy, rayne, 0, *(float*)(rayne + 0x38) - escape.originX,
                 *(float*)(rayne + 0x40) - escape.originZ, escape.travelAge);
        ResetFeedEscape();
        return false;
    }
    if (escape.rolled) {
        TickFeedEscapeTravel(rayne, gameDeltaMs);
        return true;
    }
#ifdef BR2_COMBAT_TEST
    const bool traceWindow = CombatTraceEnabled() && !escape.windowLogged;
#else
    constexpr bool traceWindow = false;
#endif
    if ((pressed && !escape.attempted) || traceWindow || escape.armed) {
        const float start = max(0.0f, escape.first - escape.fps * g_enhanced.feedEscapeWindowMs / 1000.0f);
        float frame = -1.0f;
        const bool validFrame = FeedEscapeEnemyFrame(frame);
        const bool inWindow = validFrame && frame >= start && frame < escape.first;
        const bool bladeActive = escape.bladeFollowUp && validFrame &&
                                 frame >= escape.first && frame <= escape.end;
#ifdef BR2_COMBAT_TEST
        if (traceWindow && inWindow) {
            escape.windowLogged = true;
            LogParry("feed-escape-window", 0, 0, 0, state, 0, 0,
                     escape.enemy, rayne, 0, frame, start, escape.first);
        }
#endif
        if (pressed && !escape.attempted) {
            escape.armed = inWindow || bladeActive;
            escape.attempted = escape.armed || !escape.bladeFollowUp;
            LogParry(inWindow ? "feed-escape-armed" : "feed-escape-missed", 0, 0, 0,
                     state, 0, 0, escape.enemy, rayne, 0, frame, start, escape.first);
        }
        if (escape.armed && (inWindow || bladeActive)) {
            StartFeedEscapeRoll(rayne, state, frame);
            if (escape.rolled) TickFeedEscapeTravel(rayne, gameDeltaMs);
        }
    }
    return true;
}

bool SuppressFeedEscapeHit(BYTE* rayne, BYTE* hit) {
    auto& escape = g_feedEscape;
    if (escape.rayne != rayne || !FeedEscapeEnabled()) return false;
    BYTE* attacker = *(BYTE* const*)(hit + 0x24);
    if (attacker != escape.enemy && (!attacker || CombatCharacter(attacker) != escape.enemy)) return false;
    const int kind = *(const int*)hit;
    const int state = *(int*)(rayne + 0x17C);
    float frame = -1.0f;
    const bool valid = (escape.rolled || escape.armed || CombatTraceEnabled()) && FeedEscapeEnemyFrame(frame);
    const bool followUp = valid && (IsMeleeImpact(kind) || kind == FeedRepelAttack) &&
                          frame >= escape.first && frame <= escape.end;
    if (followUp && escape.bladeFollowUp && escape.armed && !escape.rolled && FeedEscapeRayneState(state))
        StartFeedEscapeRoll(rayne, state, frame);
    const bool block = followUp && escape.rolled && FeedEscapeRayneState(*(int*)(rayne + 0x17C));
    LogParry(block ? "feed-escape-blocked" : "feed-escape-hit-native", 0, kind,
             *(const int*)(hit + 8), state, 0, *(const DWORD*)(hit + 4), escape.enemy, rayne,
             0, frame, escape.first, escape.end);
    if (block) {
        // Report a miss to the sender before the victim plays impact effects.
        *(float*)(hit + 0xC) = 0.0f;
        hit[0x68] = 0;
        *(float*)(hit + 0x6C) = 0.0f;
    }
    return block;
}

void FeedEscapeDamageApplied(BYTE* rayne, const BYTE* hit) {
    const auto& escape = g_feedEscape;
    if (escape.rayne != rayne) return;
    const BYTE* attacker = *(BYTE* const*)(hit + 0x24);
    // A third party can deal damage during F1 without breaking the repel.
    // Keep the opportunity while Rayne stays in the recovery animation.
    const bool cancel = attacker == escape.enemy || !(*(float*)(rayne + 0x39C8) > 0.0f);
    LogParry(cancel ? "feed-escape-damage-cancel" : "feed-escape-damage-unrelated", 0,
             *(const int*)hit, *(const int*)(hit + 8), *(int*)(rayne + 0x17C), 0,
             *(const DWORD*)(hit + 4), attacker, rayne);
    if (cancel) ResetFeedEscape();
}

void __declspec(naked) HookFeedRepelCapture() {
    BR2_SAVE_CPU
    __asm {
        push eax
        push esi
        push edi
        call CaptureFeedEscape
        add esp, 12
    }
    BR2_RESTORE_CPU
    __asm ret
}

void __declspec(naked) HookFeedRepel() {
    __asm {
        push 0x722D74
        push esi
        mov ecx, edi
        call edx
        jmp HookFeedRepelCapture
    }
}

void __declspec(naked) HookFeedRepelAlternate() {
    __asm {
        push 0x723EBC
        push esi
        mov ecx, edi
        call eax
        jmp HookFeedRepelCapture
    }
}
