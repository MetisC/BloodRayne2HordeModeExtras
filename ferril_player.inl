// The challenge character keeps the player controller and its combat clock.
// These windows belong to Ferril's stock clips. Use the player's unarmed
// collision routine with the corresponding claw tip in place of the foot.
struct FerrilAttackWindow {
    const char* name;
    const char* bone;
    bool right;
    int strength;
    float damage;
};

static constexpr FerrilAttackWindow FerrilAttacks[] = {
    {"lclaw2", "Bip01 L Finger22", false, 2, 85.0f},
    {"lclaw3", "Bip01 L Finger22", false, 3, 120.0f},
    {"rclaw1", "Bip01 R Finger22", true, 2, 100.0f},
    {"rclaw2", "Bip01 R Finger22", true, 2, 85.0f},
    {"rclaw3", "Bip01 R Finger22", true, 3, 120.0f},
    {"lkick3", nullptr, false, 3, 90.0f},
};

static const FerrilAttackWindow* g_ferrilContactWindow;
static bool g_ferrilContactFinisher;
static bool g_ferrilSpecialHeld;
static float g_ferrilSpecialCooldown;

static struct {
    BYTE* actor;
    DWORD nativeMoveId;
    int state;
    DWORD windowIds[_countof(FerrilAttacks)];
} g_ferrilAttack;

static void* __fastcall FerrilContactPosition(BYTE* controller, void*, void* position, int bone) {
    if (g_ferrilContactActor && controller == g_ferrilContactActor + 0x158 && g_ferrilContactBone >= 0)
        bone = g_ferrilContactBone;
    return ((void* (__thiscall*)(BYTE*, void*, int))0x608410)(controller, position, bone);
}

static BYTE __fastcall HookFerrilMelee(BYTE* actor, void*) {
    if (!FerrilChallengeActive() || actor != *(BYTE**)RayneActor)
        return ((BYTE (__thiscall*)(BYTE*))0x671F30)(actor);
    if (g_ferrilContactActor || g_photoBlockingMenu || *(int*)G_MENU_STATE ||
        *(float*)(actor + 0x39C8) <= 0.0f) return 0;

    const DWORD moveId = *(DWORD*)(actor + 0x82F0);
    const int state = *(int*)(actor + 0x17C);
    if (g_ferrilAttack.actor != actor || g_ferrilAttack.nativeMoveId != moveId ||
        g_ferrilAttack.state != state) {
        g_ferrilAttack = {};
        g_ferrilAttack.actor = actor;
        g_ferrilAttack.nativeMoveId = moveId;
        g_ferrilAttack.state = state;
    }

    BYTE* controller = actor + 0x158;
    const auto inRange = (BYTE (__thiscall*)(BYTE*, const char*))0x6061B0;
    void* skeleton = ((void* (__thiscall*)(BYTE*))0x605FF0)(controller);
    if (!skeleton) return 0;
    for (unsigned i = 0; i < _countof(FerrilAttacks); ++i) {
        const auto& attack = FerrilAttacks[i];
        if (!inRange(controller, attack.name)) continue;
        int bone = -1;
        if (attack.bone) {
            bone = ((int (__thiscall*)(void*, const char*, int))0x605000)(skeleton, attack.bone, 0);
            if (bone < 0) continue;
        }
        auto& id = g_ferrilAttack.windowIds[i];
        if (!id) id = ((DWORD (__cdecl*)())0x4030D0)();
        g_ferrilContactActor = actor;
        g_ferrilContactId = id;
        g_ferrilContactBone = bone;
        g_ferrilContactWindow = &attack;
        g_ferrilContactFinisher = inRange(controller, "ferrilfinish") != 0;
        ((void (__thiscall*)(BYTE*, int, int, DWORD))0x671440)(actor, attack.right, attack.strength, id);
        g_ferrilContactActor = nullptr;
    }
    return 0;
}

using FerrilQueryCallback = BYTE (__cdecl*)(BYTE*, BYTE*, void*);

static BYTE __cdecl FerrilBladeContact(BYTE* query, BYTE* hit, void* context) {
    const BYTE* contacts = *(const BYTE**)(query + 0x40);
    if (contacts && context) {
        const int index = max(0, *(const int*)(query + 0x44) - 1);
        const BYTE* contact = contacts + index * 0x48;
        // Keep the hit part and material from the same contact as the native blade callback.
        *(int*)(hit + 0x34) = *(const int*)(contact + 0x0C);
        *(int*)(hit + 0x5C) = *(const int*)(contact + 0x10);
    }
    return ((FerrilQueryCallback)0x66D050)(query, hit, context);
}

static int __fastcall HookFerrilMeleeQuery(void* manager, void*, void* query, BYTE* hit,
                                           FerrilQueryCallback callback, void* context) {
    BYTE* actor = g_ferrilContactActor;
    if (actor && *(BYTE**)(hit + 0x24) == actor && *(DWORD*)(hit + 4) == g_ferrilContactId) {
        const auto& attack = *g_ferrilContactWindow;
        const auto power = (int (__thiscall*)(BYTE*))(*(DWORD**)actor)[0x230 / 4];
        const float fury = power(actor) == 2 ? *(const float*)0x7C73C8 : 1.0f;
        *(float*)(hit + 0x0C) = (attack.damage + (g_ferrilContactFinisher ? 35.0f : 0.0f)) * fury;
        if (g_ferrilContactBone >= 0) {
            const auto getBlade = (void* (__thiscall*)(BYTE*))0x666F30;
            if (void* blade = getBlade(actor + (attack.right ? 0x481C : 0x47A4))) {
                const bool heavy = attack.strength >= 3 || g_ferrilContactFinisher;
                // Match a native blade packet while keeping the claw's collision geometry.
                *(int*)hit = heavy ? DamageBladeHeavy : DamageBlade;
                if (heavy) *(int*)(hit + 8) = 4;
                *(int*)(hit + 0x10) = 3;
                *(void**)(hit + 0x20) = blade;
                callback = FerrilBladeContact;
            }
        }
    }
    return ((int (__thiscall*)(void*, void*, BYTE*, FerrilQueryCallback, void*))0x5E8020)(
        manager, query, hit, callback, context);
}

static DWORD __fastcall FerrilMeleeDamage(BYTE* target, BYTE* hit) {
    BYTE* actor = g_ferrilContactActor;
    const bool ferrilHit = actor && *(BYTE**)(hit + 0x24) == actor &&
        *(DWORD*)(hit + 4) == g_ferrilContactId;
    const DWORD result = DispatchCinematicDamage(target, hit);
    if (ferrilHit) ConfirmOutgoingHit(actor, target, hit);
    return result;
}

static void __fastcall HookFerrilFeedOrHarpoon(BYTE* actor, void*) {
    if (!FerrilChallengeActive() || actor != *(BYTE**)RayneActor) {
        ((void (__thiscall*)(BYTE*))0x663690)(actor);
        return;
    }
    if (g_ferrilSpecialHeld) return;
    g_ferrilSpecialHeld = true;
    if (g_ferrilSpecialCooldown > 0.0f || *(float*)(actor + 0x39C8) <= 0.0f ||
        !((int (__thiscall*)(BYTE*))0x4410E0)(actor)) return;

    const int state = *(int*)(actor + 0x17C);
    const auto inRange = (BYTE (__thiscall*)(BYTE*, const char*))0x6061B0;
    BYTE* controller = actor + 0x158;
    if (state == 46 || state == 47 ||
        (!inRange(controller, "allowharpoon") && !inRange(controller, "combocontinue")))
        return;

    // Native circle-blade command dispatches to slot 46, Ferril's corkscrew.
    ((void (__thiscall*)(BYTE*, int))0x54CA30)(actor, 62);
    g_ferrilSpecialCooldown = 1.25f;
    *(int*)(actor + 0x3A68) = 2;
}

static void TickFerrilInput(BYTE* actor, const BYTE* game) {
    if (!actor || !FerrilChallengeActive() || g_photoBlockingMenu ||
        *(int*)G_MENU_STATE || *(const int*)(game + 0x324) || *(const int*)(game + 0x39C))
        return;
    if (!*(const int*)(actor + 0x3A68)) g_ferrilSpecialHeld = false;
    const float delta = *(const float*)(game + 0x3D8);
    if (delta > 0.0f) g_ferrilSpecialCooldown = max(0.0f, g_ferrilSpecialCooldown - delta);
}

static void __fastcall HookRayneFeedWarning(void* hud, void*, const char* message, int line) {
    if (ActiveChallengeCharacter() == ChallengeRayne)
        ((void (__thiscall*)(void*, const char*, int))0x594540)(hud, message, line);
}

static void __fastcall HookRayneTaunt(BYTE* actor, void*, int event) {
    if (actor != *(BYTE**)RayneActor || ActiveChallengeCharacter() == ChallengeRayne)
        ((void (__thiscall*)(BYTE*, int))0x551D30)(actor, event);
}

static DWORD __fastcall HookChallengeVoice(BYTE* actor, void*, const char* sample) {
    const auto character = ActiveChallengeCharacter();
    if (sample && character != ChallengeRayne && actor == *(BYTE**)RayneActor) {
        if (character == ChallengeFerril && !_strnicmp(sample, "rayne_vc_hit_", 13))
            sample = "ferril_hurt_1.wav";
        else if (character == ChallengeFerril && !_strnicmp(sample, "rayne_vc_burn_", 14))
            sample = "ferril_hurt_3.wav";
        else if (character == ChallengeFerril && !_strnicmp(sample, "rayne_vc_die_", 13))
            sample = "ferril_hurt_5.wav";
        else if (!_strnicmp(sample, "rayne_", 6) || !_strnicmp(sample, "rayne-", 6) ||
                 !_stricmp(sample, "hero_jump.wav"))
            return 0;
    }
    return ((DWORD (__thiscall*)(BYTE*, const char*))0x4032B0)(actor, sample);
}

static int __fastcall HookFerrilBladeRender(void* weapon, void*) {
    if (FerrilChallengeActive()) {
        BYTE* player = *(BYTE**)RayneActor;
        const auto blade = (void* (__thiscall*)(void*))0x666F30;
        if (player && (weapon == blade(player + 0x47A4) || weapon == blade(player + 0x481C)))
            return 0;
    }
    return ((int (__thiscall*)(void*))0x407390)(weapon);
}
