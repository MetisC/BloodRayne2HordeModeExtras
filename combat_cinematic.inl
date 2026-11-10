// Observe the native damage transaction so a lethal cut still qualifies,
// while a cut to a victim that was already dead cannot start a pulse.
struct CinematicContact {
    BYTE* target;
    int part = -1;
};

CinematicContact* g_cinematicContact;
DWORD g_cinematicAt, g_cinematicHit;
bool g_hasCombatCinematic;

void ResetCombatCinematic() {
    g_hasCombatCinematic = false;
}

BYTE __fastcall HookCinematicDetach(BYTE* target, void*, void* limb, int part, int mode, int children) {
    const BYTE detached = ((BYTE (__thiscall*)(BYTE*, void*, int, int, int))0x44C6B0)(
        target, limb, part, mode, children);
    if (detached && g_cinematicContact && g_cinematicContact->target == target &&
        g_cinematicContact->part < 0)
        g_cinematicContact->part = part;
    return detached;
}

static void RecordOutgoingHit(BYTE* rayne, BYTE* victim, const BYTE* hit,
                              float damage, bool confirmedMeleeContact);

static DWORD DispatchCinematicDamageNative(BYTE* target, BYTE* hit) {
    using ReceiveDamage = DWORD (__thiscall*)(BYTE*, BYTE*);
    const auto receive = (*(ReceiveDamage**)target)[0x3C / sizeof(DWORD)];
    if (SuppressFeedEscapeHit(target, hit)) return 0;
    if (!EnhancedCombatActive() || (!g_enhanced.dismemberCinematicEnabled &&
        !g_enhanced.groundCinematicEnabled))
        return receive(target, hit);

    BYTE* rayne = *(BYTE**)RayneActor;
    const DWORD now = GetTickCount();
    const DWORD id = *(DWORD*)(hit + 4);
    const int type = *(int*)hit;
    const bool canFilm = rayne && target != rayne && *(BYTE**)(hit + 0x24) == rayne &&
        !*(BYTE**)(rayne + 0x83C0) && !NativeBulletTime(NativeCombatPower(rayne)) &&
        (!g_hasCombatCinematic || ((!id || id != g_cinematicHit) &&
            now - g_cinematicAt >= (DWORD)g_enhanced.combatCinematicGapMs)) &&
        !FeedExecutionActive() && CombatCharacter(target) && *(float*)(target + 0x39C8) > 0.0f;
    const int state = canFilm ? *(int*)(rayne + 0x17C) : 0;
    // Head Stomp and both orientations of the blade and kick ground strikes.
    const bool ground = g_enhanced.groundCinematicEnabled &&
        (state == 0x41 || (state >= 0x74 && state <= 0x77)) &&
        (IsMeleeImpact(type) || type == 0x7E || type == 0x78);
    CinematicContact contact{target};
    CinematicContact* previous = g_cinematicContact;
    g_cinematicContact = canFilm && g_enhanced.dismemberCinematicEnabled ? &contact : nullptr;
    const DWORD result = receive(target, hit);
    g_cinematicContact = previous;

    const bool severed = contact.part >= 0;
    if (canFilm && (severed || (ground && hit[0x68] && *(float*)(hit + 0x6C) > 0.0f)) &&
        EnhancedCombatActive() && rayne == *(BYTE**)RayneActor && !FeedExecutionActive()) {
        BeginCombatCinematic(severed ? DismemberCinematic : GroundCinematic);
        g_hasCombatCinematic = true;
        g_cinematicAt = now;
        g_cinematicHit = id;
        LogParry(severed ? "dismember-cinematic" : "ground-cinematic", 0, type,
                 *(int*)(hit + 8), state, 0, id, target, rayne, 0, (float)contact.part,
                 *(float*)(hit + 0x6C));
    }
    return result;
}

DWORD __fastcall DispatchCinematicDamage(BYTE* target, BYTE* hit) {
    BYTE* player = *(BYTE**)RayneActor;
    if (!player || target == player || *(BYTE**)(hit + 0x24) != player)
        return DispatchCinematicDamageNative(target, hit);

    const auto character = ActiveChallengeCharacter();
    const int type = *(const int*)hit;
    const bool melee = IsMeleeImpact(type);
    const bool ephemera = melee && character == ChallengeEphemera;
    const bool triangle = *(const int*)(player + 0x17C) == ChallengeTriangleState;
    const bool observe = ephemera || (melee && character == ChallengeKestral && triangle);
    const float scale = ChallengeDamage[character].dealt;
    if ((!observe && scale == 1.0f) || !CombatCharacter(target))
        return DispatchCinematicDamageNative(target, hit);

    const float healthBefore = *(const float*)(target + 0x39C8);
    if (!(healthBefore > 0.0f)) return DispatchCinematicDamageNative(target, hit);

    float& damage = *(float*)(hit + 0x0C);
    const float base = damage;
    if (ephemera) damage *= triangle ? 1.35f : 1.25f;
    const bool scaleHit = scale != 1.0f && damage > 0.0f && type != 0x87;
    if (scaleHit) damage *= scale;
    const DWORD result = DispatchCinematicDamageNative(target, hit);
    if (ephemera || scaleHit) damage = base;

    // These moves can apply damage without the native outgoing notification.
    if (observe) {
        const float applied = healthBefore - *(const float*)(target + 0x39C8);
        const float reported = hit[0x68] ? *(const float*)(hit + 0x6C) : 0.0f;
        RecordOutgoingHit(player, target, hit, max(applied, reported), true);
    }
    return result;
}

// Replay each collision path's argument setup and return to the instruction
// after its virtual call. The native callee still owns the damage packet.
void __declspec(naked) HookCinematicCollision() {
    __asm {
        mov ecx, edi
        lea edx, [ebp - 0x88]
        jmp DispatchCinematicDamage
    }
}

void __declspec(naked) HookCinematicKick() {
    __asm {
        mov edx, edi
        jmp FerrilMeleeDamage
    }
}

void __declspec(naked) HookCinematicRay() {
    __asm {
        mov edx, esi
        jmp DispatchCinematicDamage
    }
}

void __declspec(naked) HookCinematicWeapon() {
    __asm {
        mov [ebp - 0x4B8], eax
        mov ecx, ebx
        lea edx, [ebp - 0x514]
        jmp DispatchCinematicDamage
    }
}

void __declspec(naked) HookCinematicHarpoon() {
    __asm {
        lea edx, [ebp - 0x93C]
        jmp DispatchCinematicDamage
    }
}
