// Enhanced combat harpoon follow-up and dive kick contact sweep.

enum HarpoonPullPhase { PullIdle, PullYanking, PullReleased, PullStrike };
enum HarpoonFollowup { HarpoonNoStrike, HarpoonBlade, HarpoonKick };
static struct {
    HarpoonPullPhase phase;
    BYTE* player;
    BYTE* victim;
    DWORD at;
    float step;
    bool pullGesture;
    HarpoonFollowup followup;
    bool followupBlocked;
    int strikeState;
} g_harpoonPull;
static bool g_harpoonTriangleHeld;
static bool g_harpoonBladeHeld;
static bool g_harpoonKickHeld;
static bool g_standingHarpoonHookInstalled;

constexpr int HarpoonHookedState = 0xDB;
constexpr int HarpoonYankTowardState = 0xDF;
constexpr int HarpoonStandingHurtState = 0x21;
constexpr int HarpoonFrontRecoilState = 0x38;
constexpr int HarpoonFrontHoldState = 0x39;
constexpr int HarpoonKickState = 0x195; // Rayne.SKB: locked_evade_front_k (Garuda Cross)
constexpr int HarpoonFallbackKickState = 0x6D; // Turning Dragon, second kick
constexpr int HarpoonBladeState = 0x65; // Rayne.SKB: combo_locked_up_bbb (Mantis Kiss)
constexpr int HarpoonKickMove = 0x1B;
constexpr int HarpoonFallbackKickMove = 0x36;
constexpr int HarpoonBladeMove = 0x2E;
constexpr float HarpoonPullStop = 1.55f;
constexpr float HarpoonStrikeHeight = 1.5f;
constexpr float HarpoonPullGestureFrame = 15.0f; // harpoon_yank_toward event 1502
constexpr float HarpoonPullLastFrame = 37.0f;

static void TraceHarpoonFollowup(const char* decision, int playerState, int victimState, float distance);

static float HarpoonWindupDistance(HarpoonFollowup followup) {
    return followup == HarpoonBlade ? 6.7f : 7.8f;
}

static float HarpoonStepBudget(float gameDeltaMs) {
    return min(0.48f, max(0.0f, gameDeltaMs) * 0.04f);
}

float HarpoonAnimationRate(const BYTE* rayne) {
    const auto& pull = g_harpoonPull;
    if (pull.player == rayne && pull.phase == PullYanking &&
        pull.followup != HarpoonNoStrike && !pull.followupBlocked &&
        !pull.pullGesture && *(const int*)(rayne + 0x17C) == HarpoonYankTowardState &&
        *(const float*)(rayne + 0x180) < HarpoonPullGestureFrame)
        return 1.9f;
    return 1.0f;
}

void ResetHarpoonPull() {
    g_harpoonPull = {};
    g_harpoonTriangleHeld = false;
    g_harpoonBladeHeld = false;
    g_harpoonKickHeld = false;
}

void CancelHarpoonFollowup() {
    auto& pull = g_harpoonPull;
    if (pull.phase == PullIdle) return;
    if (pull.phase != PullStrike && pull.followup != HarpoonNoStrike)
        TraceHarpoonFollowup("cancelled-by-hit", *(int*)(pull.player + 0x17C), -1, -1.0f);
    pull.followup = HarpoonNoStrike;
    pull.followupBlocked = true;
    pull.step = 0.0f;
}

static void PlayHarpoonState(BYTE* actor, int state) {
    ((void (__thiscall*)(BYTE*, int, float, float))0x658EB0)(actor, state, 0.0f, -1.0f);
}

static float HarpoonDistance(const BYTE* player, const BYTE* victim, float& x, float& z) {
    x = *(const float*)(victim + 0x38) - *(const float*)(player + 0x38);
    z = *(const float*)(victim + 0x40) - *(const float*)(player + 0x40);
    return sqrtf(x * x + z * z);
}

// The collision update receives a local-space movement vector. Add one
// bounded step there, so a skipped actor update cannot bank displacement.
static void __fastcall ApplyHarpoonCollisionStep(BYTE* victim, float* movement) {
    auto& pull = g_harpoonPull;
    if (victim != pull.victim || pull.step <= 0.0f) return;
    const float budget = pull.step;
    pull.step = 0.0f;
    BYTE* player = pull.player;
    if (*(BYTE**)RayneActor != player || !(*(float*)(victim + 0x39C8) > 0.0f)) return;
    if (pull.phase == PullYanking) {
        if (*(BYTE**)(player + 0x8374) != victim || *(int*)(player + 0x836C) != 6) return;
    } else if (pull.phase != PullStrike || *(int*)(player + 0x17C) != pull.strikeState ||
               *(int*)(victim + 0x17C) != HarpoonStandingHurtState) {
        return;
    }

    float x, z;
    const float distance = HarpoonDistance(player, victim, x, z);
    const float height = fabsf(*(float*)(victim + 0x3C) - *(float*)(player + 0x3C));
    // Feed the ground solver horizontal movement even on stairs. It owns
    // floor following, step height and wall/ledge collisions.
    const float stop = height < HarpoonStrikeHeight ? HarpoonPullStop : 0.85f;
    if (!(distance > stop)) return;
    const float scale = -min(distance - stop, budget) / distance;
    const float worldX = x * scale, worldZ = z * scale;
    const auto* basis = (const float*)(victim + 0x50);
    for (int axis = 0; axis < 3; ++axis)
        movement[axis] += worldX * basis[axis] + worldZ * basis[axis + 6];
}

void __declspec(naked) HookHarpoonCollisionStep() {
    __asm {
        cmp dword ptr [g_harpoonPull], PullYanking
        je applyStep
        cmp dword ptr [g_harpoonPull], PullStrike
        jne validateMovement
    applyStep:
        mov ecx, esi
        mov edx, ebx
        call ApplyHarpoonCollisionStep
    validateMovement:
        // The caller already pushed the movement vector for the validator.
        push 0x402C00
        ret
    }
}

static void __fastcall FinishNativeHarpoonYank(BYTE* player) {
    const auto& pull = g_harpoonPull;
    if (pull.phase == PullYanking && pull.player == player &&
        *(BYTE**)(player + 0x8374) == pull.victim)
        return;
    *(int*)(player + 0x836C) = 5;
    *(float*)(player + 0x83A4) = -1.0f;
}

void __declspec(naked) HookHarpoonYankEnd() {
    __asm {
        mov ecx, ebx
        jmp FinishNativeHarpoonYank
    }
}

// Called by the native rope release. Keep its cable cleanup, but replace
// the Punk type-4 response, which starts ragdoll and queues getupbackidle.
static void __fastcall ReleaseHarpoonVictim(BYTE* victim, BYTE* event) {
    using Receive = void (__thiscall*)(BYTE*, BYTE*);
    const auto receive = (Receive)(*(DWORD**)victim)[0xB0 / 4];
    BYTE* player = *(BYTE**)(event + 0x1C);
    if (g_harpoonPull.phase != PullYanking || player != g_harpoonPull.player ||
        victim != g_harpoonPull.victim || *(int*)event != 4 ||
        (DWORD)receive != 0x5BBB50 || !EnhancedCombatActive() ||
        !(*(float*)(victim + 0x39C8) > 0.0f) ||
        !ActorHasAnim((DWORD)victim, HarpoonStandingHurtState)) {
        receive(victim, event);
        return;
    }

    *(int*)(victim + 0x409C) = 0;
    *(int*)(victim + 0x3BB8) = 0;
    const auto notify = (void (__thiscall*)(BYTE*, int))(*(DWORD**)victim)[0x1D4 / 4];
    notify(victim, 0xD);
    PlayHarpoonState(victim, HarpoonStandingHurtState);
    g_harpoonPull.phase = PullReleased;
    g_harpoonPull.at = GetTickCount();
    float x, z;
    TraceHarpoonFollowup("released", *(int*)(player + 0x17C), *(int*)(victim + 0x17C),
                         HarpoonDistance(player, victim, x, z));
}

void __declspec(naked) HookStandingHarpoonRelease() {
    __asm {
        mov edx, eax
        jmp ReleaseHarpoonVictim
    }
}

void TickHarpoonPull(BYTE* rayne, DWORD now, float gameDeltaMs) {
    if (!g_standingHarpoonHookInstalled || !EnhancedCombatActive() ||
        !g_enhanced.harpoonPullExecEnabled || ActiveChallengeCharacter() != ChallengeRayne) {
        ResetHarpoonPull();
        return;
    }

    __try {
        const int anim = *(int*)(rayne + 0x17C);
        const int phase = *(int*)(rayne + 0x836C);
        BYTE* target = *(BYTE**)(rayne + 0x8374);
        int& triangle = *(int*)(rayne + 0x3A68);
        int& blade = *(int*)(rayne + AttackButtons[0]);
        int& kickButton = *(int*)(rayne + AttackButtons[1]);
        const bool held = triangle == 1;
        const bool pressed = held && !g_harpoonTriangleHeld;
        const bool bladeHeld = blade == 1;
        const bool bladePressed = bladeHeld && !g_harpoonBladeHeld;
        const bool kickHeld = kickButton == 1;
        const bool kickPressed = kickHeld && !g_harpoonKickHeld;
        g_harpoonTriangleHeld = held;
        g_harpoonBladeHeld = bladeHeld;
        g_harpoonKickHeld = kickHeld;
        auto& pull = g_harpoonPull;

        if (pull.phase == PullStrike) {
            pull.step = 0.0f;
            if (pull.player == rayne && (anim == pull.strikeState || now - pull.at <= 60)) {
                // The selected strike is a single move. Don't let button repeats
                // seed the stock kick/blade combo while it is playing.
                *(int*)(rayne + 0x8354) = 0;
                if (held) triangle = 2;
                if (bladeHeld) blade = 2;
                if (kickHeld) kickButton = 2;
            }
            if (pull.player != rayne ||
                (anim != pull.strikeState && now - pull.at > 60)) {
                pull = {};
            } else if (!pull.followupBlocked && anim == pull.strikeState) {
                const float contactFrame = pull.followup == HarpoonBlade ? 11.0f :
                    (pull.strikeState == HarpoonKickState ? 13.0f : 12.0f);
                if (*(const float*)(rayne + 0x180) < contactFrame)
                    pull.step = HarpoonStepBudget(gameDeltaMs);
            }
            return;
        }

        if (pull.phase == PullIdle) {
            if (!pressed || anim != HarpoonHookedState || (phase != 3 && phase != 4) ||
                !target || fabsf(*(float*)(rayne + 0x3A4C)) > 0.15f ||
                fabsf(*(float*)(rayne + 0x3A50)) > 0.15f ||
                fabsf(*(float*)(rayne + 0x3A54)) > 0.15f ||
                (*(DWORD**)target)[0xB0 / 4] != 0x5BBB50 ||
                !(*(float*)(target + 0x39C8) > 0.0f) ||
                !ActorHasAnim((DWORD)target, HarpoonStandingHurtState))
                return;

            const int victimState = *(int*)(target + 0x17C);
            if (victimState != HarpoonFrontRecoilState && victimState != HarpoonFrontHoldState) return;
            pull = {PullYanking, rayne, target, now};
            PlayHarpoonState(rayne, HarpoonYankTowardState);
            const auto notify = (void (__thiscall*)(BYTE*, int))(*(DWORD**)rayne)[0x23C / 4];
            notify(rayne, 0xB);
            *(int*)(rayne + 0x836C) = 6;
            return;
        }

        if (pull.player != rayne || now - pull.at > (pull.phase == PullYanking ? 8000u : 1200u)) {
            if (pull.followup != HarpoonNoStrike)
                TraceHarpoonFollowup("timeout", anim, -1, -1.0f);
            pull = {};
            return;
        }

        bool releasedForStrike = false;
        if (pull.phase == PullYanking) {
            // Retraction may clear the target without calling the release event.
            if (!target && (phase == 5 || phase == 0)) {
                pull.phase = PullReleased;
                pull.at = now;
                pull.step = 0.0f;
                return;
            }
            if (anim != HarpoonYankTowardState || (phase != 6 && phase != 5)) {
                pull = {};
                return;
            }
            pull.step = 0.0f;
            if (target != pull.victim) {
                pull = {};
                return;
            }
            if (!(*(float*)(target + 0x39C8) > 0.0f)) {
                pull = {};
                return;
            }
            const float frame = *(const float*)(rayne + 0x180);
            float x, z;
            const float distance = HarpoonDistance(rayne, target, x, z);
            const float height = fabsf(*(float*)(target + 0x3C) - *(float*)(rayne + 0x3C));
            if (phase == 6 && !pull.followupBlocked &&
                ((bladePressed && pull.followup != HarpoonBlade) ||
                 (kickPressed && pull.followup == HarpoonNoStrike))) {
                pull.followup = bladePressed ? HarpoonBlade : HarpoonKick;
                TraceHarpoonFollowup(bladePressed ? "queued-blade" : "queued-kick",
                                     anim, *(int*)(target + 0x17C), distance);
            }
            if (bladeHeld) blade = 2;
            if (kickHeld) kickButton = 2;
            if (held) triangle = 2;
            if (pull.followup != HarpoonNoStrike) *(int*)(rayne + 0x8354) = 0;
            if (frame < HarpoonPullGestureFrame) return;
            if (!pull.pullGesture) {
                pull.pullGesture = true;
                const int victimState = *(int*)(target + 0x17C);
                if (victimState == HarpoonFrontHoldState)
                    PlayHarpoonState(target, HarpoonFrontRecoilState);
                TraceHarpoonFollowup("gesture", anim, victimState, distance);
            }
            const bool wantsStrike = pull.followup != HarpoonNoStrike;
            const float arrival = wantsStrike ? HarpoonWindupDistance(pull.followup) : 2.15f;
            const float lastFrame = wantsStrike ? 39.0f : HarpoonPullLastFrame;
            if ((distance <= arrival && height < HarpoonStrikeHeight) || frame >= lastFrame) {
                ((void (__thiscall*)(BYTE*, const float*, const float*, int))0x661350)(
                    rayne, (const float*)(rayne + 0x38), nullptr, 0);
                releasedForStrike = wantsStrike && pull.phase == PullReleased &&
                    !*(BYTE**)(rayne + 0x8374);
                if (!releasedForStrike) return;
            } else {
                const float ramp = min(1.0f, (frame - HarpoonPullGestureFrame + 1.0f) / 3.0f);
                if (phase == 6) pull.step = ramp * HarpoonStepBudget(gameDeltaMs);
                return;
            }
        }

        if (phase != 0 && !releasedForStrike) return;
        BYTE* victim = pull.victim;
        const HarpoonFollowup followup = pull.followup;
        pull = {};
        if (followup == HarpoonNoStrike) return;
        if (anim != HarpoonYankTowardState && anim != 0xD9 &&
            anim != 0 && anim != 0x180 && anim != 0x79 && anim != 0x7A &&
            !((BYTE (__cdecl*)(int))0x5498A0)(anim)) {
            TraceHarpoonFollowup("interrupted", anim, -1, -1.0f);
            return;
        }
        const bool useBlade = followup == HarpoonBlade;
        if ((*(DWORD**)victim)[0xB0 / 4] != 0x5BBB50 ||
            !(*(float*)(victim + 0x39C8) > 0.0f)) return;
        float x, z;
        const float distance = HarpoonDistance(rayne, victim, x, z);
        const int victimState = *(int*)(victim + 0x17C);
        if (distance > HarpoonWindupDistance(followup) + 0.15f ||
            fabsf(*(float*)(victim + 0x3C) - *(float*)(rayne + 0x3C)) >= HarpoonStrikeHeight) {
            TraceHarpoonFollowup("out-of-reach", anim, victimState, distance);
            return;
        }
        if (victimState == HarpoonFrontRecoilState || victimState == HarpoonFrontHoldState)
            PlayHarpoonState(victim, HarpoonStandingHurtState);
        const bool useGaruda = !useBlade && ActorHasAnim((DWORD)rayne, HarpoonKickState);
        const int strikeState = useBlade ? HarpoonBladeState :
            (useGaruda ? HarpoonKickState : HarpoonFallbackKickState);
        const int strikeMove = useBlade ? HarpoonBladeMove :
            (useGaruda ? HarpoonKickMove : HarpoonFallbackKickMove);
        if (!ActorHasAnim((DWORD)rayne, strikeState)) {
            TraceHarpoonFollowup("missing-animation", anim, strikeState, distance);
            return;
        }
        pull = {PullStrike, rayne, victim, now};
        pull.followup = followup;
        pull.strikeState = strikeState;
        *(int*)(rayne + 0x8354) = 0;
        // Start a native move so damage IDs, target selection and combo state
        // are initialized together with the clip.
        ((void (__thiscall*)(BYTE*, int))0x54CA30)(rayne, strikeMove);
        int startedState = *(int*)(rayne + 0x17C);
        if (useGaruda && startedState != HarpoonKickState &&
            (startedState == anim || startedState == 0xD9) &&
            ActorHasAnim((DWORD)rayne, HarpoonFallbackKickState)) {
            TraceHarpoonFollowup("garuda-rejected", startedState, victimState, distance);
            ((void (__thiscall*)(BYTE*, int))0x54CA30)(rayne, HarpoonFallbackKickMove);
            pull.strikeState = HarpoonFallbackKickState;
            startedState = *(int*)(rayne + 0x17C);
        }
        if (useBlade && startedState == 0x89) pull.strikeState = startedState;
        if (!useBlade && startedState == 0x91) pull.strikeState = startedState;
        if (startedState == pull.strikeState) {
            ((void (__thiscall*)(BYTE*, float))0x446300)(rayne, 1.0f);
            ((void (__thiscall*)(BYTE*, const float*))0x440DD0)(
                rayne, (const float*)(victim + 0x38));
            pull.step = HarpoonStepBudget(gameDeltaMs);
        }
        TraceHarpoonFollowup(useBlade ? "blade" :
            (pull.strikeState == HarpoonKickState ? "kick" : "kick-fallback"),
            startedState, victimState, distance);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ResetHarpoonPull(); }
}

// ---- dive kick sweep --------------------------------------------------------

constexpr int DiveKickState = 0x057;
constexpr int DiveKickMirrorState = 0x080; // base + 0x29 like other locked moves

static int g_diveSerial;
static BYTE* g_diveHits[64];
static unsigned g_diveHitCount;
static bool g_diveWasActive;

void ResetDiveKick() { g_diveHitCount = 0; g_diveWasActive = false; }

// Delivers a kick packet identical in shape to the native harpoon packet
// (ctor 0x405790; type/id/severity/damage/attacker; +0x10 part=-1; +0x70 flag).
void DeliverDiveKickHit(BYTE* rayne, BYTE* victim, DWORD id) {
    BYTE hit[0x80];
    memset(hit, 0, sizeof hit);
    ((void (__thiscall*)(BYTE*))0x405790)(hit);
    *(int*)(hit) = 0x84;                                     // kick damage type
    *(DWORD*)(hit + 4) = id;
    *(int*)(hit + 8) = g_enhanced.diveKickSeverity;
    *(float*)(hit + 0xC) = g_enhanced.diveKickDamage;
    *(float*)(hit + 0x6C) = g_enhanced.diveKickDamage;
    *(int*)(hit + 0x10) = -1;
    *(DWORD*)(hit + 0x24) = (DWORD)rayne;
    hit[0x70] = 1;
    memcpy(hit + 0x14, rayne + 0x38, 12);                    // source = Rayne
    memcpy(hit + 0x28, victim + 0x38, 12);                   // point = victim
    __try {
        const auto vtbl = *(DWORD**)victim;
        ((void (__thiscall*)(BYTE*, BYTE*))vtbl[0x3C / 4])(victim, hit);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void TickDiveKick(BYTE* rayne) {
    const int anim = *(int*)(rayne + 0x17C);
    const bool active = anim == DiveKickState || anim == DiveKickMirrorState;
    if (!EnhancedCombatActive() || !g_enhanced.diveKickEnabled || !active ||
        (g_harpoonPull.phase == PullStrike && g_harpoonPull.player == rayne)) {
        if (g_diveWasActive) ResetDiveKick();
        return;
    }
    if (!g_diveWasActive) {
        g_diveWasActive = true;
        g_diveHitCount = 0;
        ++g_diveSerial;
    }
    const float rx = *(const float*)(rayne + 0x38);
    const float rz = *(const float*)(rayne + 0x40);
    __try {
        const DWORD mgr = *(DWORD*)0x7C5D50;
        const int count = *(int*)(mgr + 0x3F08);
        for (int i = 0; i < count && i < 2048; ++i) {
            BYTE* actor = *(BYTE**)(mgr + 0x3F0C + i * 8);
            if (!actor) break;
            if (actor == rayne) continue;
            if (!CombatCharacter(actor)) continue;
            if (*(const float*)(actor + 0x39C8) <= 0.0f) continue;   // dead
            const float dx = *(const float*)(actor + 0x38) - rx;
            const float dz = *(const float*)(actor + 0x40) - rz;
            if (dx * dx + dz * dz > g_enhanced.diveKickRadius * g_enhanced.diveKickRadius)
                continue;
            bool seen = false;
            for (unsigned j = 0; j < g_diveHitCount; ++j)
                if (g_diveHits[j] == actor) { seen = true; break; }
            if (seen) continue;
            if (g_diveHitCount < _countof(g_diveHits)) g_diveHits[g_diveHitCount++] = actor;
            DeliverDiveKickHit(rayne, actor,
                               0xD1BE0000u + (DWORD)g_diveSerial * 256u + (DWORD)i);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
