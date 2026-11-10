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
    const auto character = ActiveChallengeCharacter();
    if (actor == *(BYTE**)RayneActor && (character == ChallengeNinjaF || character == ChallengeNinjaM) &&
        *(const int*)(actor + 0x17C) == 46) {
        if (!g_photoBlockingMenu && !*(int*)G_MENU_STATE && *(const float*)(actor + 0x39C8) > 0.0f)
            TryNinjaShoulder(actor);
        return 0;
    }
    if (character != ChallengeFerril || actor != *(BYTE**)RayneActor)
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
    if (!ChallengeUsesSpecialInput() || actor != *(BYTE**)RayneActor) {
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
    if (!actor || !ChallengeUsesSpecialInput() || g_photoBlockingMenu ||
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

static char g_dhampirVoice[64];
static unsigned g_dhampirVoiceSpin;

// Map a rayne_vc_* sample to the matching dhampirf_* voice. Numbered Rayne
// samples keep their index so variety is preserved; unnumbered ones rotate.
static const char* DhampirVoiceFor(const char* sample) {
    struct VoiceMap { const char* prefix; int prefixLen; const char* base; int variants; };
    static const VoiceMap map[] = {
        {"rayne_vc_hit_",  13, "hurt",   6},
        {"rayne_vc_burn_", 14, "onfire", 4},
    };
    for (const auto& m : map) {
        if (_strnicmp(sample, m.prefix, m.prefixLen)) continue;
        const int n = atoi(sample + m.prefixLen);
        const int v = n > 0 ? (n - 1) % m.variants + 1
                            : (int)(++g_dhampirVoiceSpin % m.variants) + 1;
        sprintf_s(g_dhampirVoice, "dhampirf_%s_a_%02d.wav", m.base, v);
        return g_dhampirVoice;
    }
    const char* rotating = NULL;
    int variants = 0;
    if (!_strnicmp(sample, "rayne_vc_die_", 13))        { rotating = "die";   variants = 4; }
    else if (!_strnicmp(sample, "rayne_vc_comlev", 15)) { rotating = "taunt"; variants = 4; }
    else if (!_strnicmp(sample, "rayne_vc_groundstrike", 21)) { rotating = "kill"; variants = 3; }
    if (rotating) {
        sprintf_s(g_dhampirVoice, "dhampirf_%s_a_%02d.wav", rotating,
                  (int)(g_dhampirVoiceSpin++ % variants) + 1);
        return g_dhampirVoice;
    }
    return NULL;
}

// Male dhampir voice: rayne_vc_* -> dhampir_* (male set in W32ENSND).
static const char* DhampirMaleVoiceFor(const char* sample) {
    struct VoiceMap { const char* prefix; int prefixLen; const char* base; int variants; };
    static const VoiceMap map[] = {
        {"rayne_vc_hit_",  13, "hurt",   6},
        {"rayne_vc_burn_", 14, "hurt",   6},
    };
    for (const auto& m : map) {
        if (_strnicmp(sample, m.prefix, m.prefixLen)) continue;
        const int n = atoi(sample + m.prefixLen);
        const int v = n > 0 ? (n - 1) % m.variants + 1
                            : (int)(++g_dhampirVoiceSpin % m.variants) + 1;
        sprintf_s(g_dhampirVoice, "dhampir_%s_a_%02d.wav", m.base, v);
        return g_dhampirVoice;
    }
    const char* rotating = NULL;
    int variants = 0;
    if (!_strnicmp(sample, "rayne_vc_die_", 13))        { rotating = "die";   variants = 4; }
    else if (!_strnicmp(sample, "rayne_vc_comlev", 15)) { rotating = "taunt"; variants = 4; }
    else if (!_strnicmp(sample, "rayne_vc_groundstrike", 21)) { rotating = "kill"; variants = 4; }
    if (rotating) {
        sprintf_s(g_dhampirVoice, "dhampir_%s_a_%02d.wav", rotating,
                  (int)(g_dhampirVoiceSpin++ % variants) + 1);
        return g_dhampirVoice;
    }
    return NULL;
}

// Ephemera voice: rayne_vc_* -> her own EPHEMER* set in W32ENSND. The state
// table already carries her attack/hurt samples; this covers the engine-side
// voice requests (hit, burn, die, kill-level, ground-strike).
static const char* EphemeraVoiceFor(const char* sample) {
    // Numbered requests keep their index (like the dhampir maps) so hit types
    // map to distinct barks; unnumbered ones rotate.
    static const struct { const char* prefix; int prefixLen;
                          const char* stem; int variants, lo; } map[] = {
        {"rayne_vc_hit_",         13, "ephemer_hurt",     7, 1},
        {"rayne_vc_burn_",        14, "ephemer_hurt",     4, 4},
        {"rayne_vc_die_",         13, "ephemer_hurt",     3, 5},
        {"rayne_vc_comlev",       15, "ephemer_takethis", 3, 1},
        {"rayne_vc_groundstrike", 21, "ephemer_s_attack", 3, 1},
    };
    for (int i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (_strnicmp(sample, map[i].prefix, map[i].prefixLen)) continue;
        int v = map[i].lo;
        const char* p = sample + map[i].prefixLen;
        if (*p >= '0' && *p <= '9') {
            v += (atoi(p) - 1) % map[i].variants;               // keep the index
        } else {
            v += (int)(g_dhampirVoiceSpin++ % map[i].variants); // rotate
        }
        sprintf_s(g_dhampirVoice, "%s_%d.wav", map[i].stem, v);
        return g_dhampirVoice;
    }
    return NULL;
}

// Kagan voice: rayne_vc_* -> his own KAGAN_* set in W32SOUND. Same
// index-preserving map; comlev rotates through his sparse taunt index list.
static const char* KaganVoiceFor(const char* sample) {
    static const struct { const char* prefix; int prefixLen;
                          const char* stem; int variants, lo; } map[] = {
        {"rayne_vc_hit_",         13, "kagan_hurt",   7, 1},
        {"rayne_vc_burn_",        14, "kagan_hurt",   4, 8},
        {"rayne_vc_die_",         13, "kagan_hurt",   3, 12},
        {"rayne_vc_groundstrike", 21, "kagan_attack", 8, 2},
    };
    for (int i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (_strnicmp(sample, map[i].prefix, map[i].prefixLen)) continue;
        int v = map[i].lo;
        const char* p = sample + map[i].prefixLen;
        if (*p >= '0' && *p <= '9') {
            v += (atoi(p) - 1) % map[i].variants;
        } else {
            v += (int)(g_dhampirVoiceSpin++ % map[i].variants);
        }
        sprintf_s(g_dhampirVoice, "%s_%d.wav", map[i].stem, v);
        return g_dhampirVoice;
    }
    if (!_strnicmp(sample, "rayne_vc_comlev", 15)) {
        // kagan_taunt_2/3/12/17 do not exist; rotate the real indices.
        static const int taunts[] = {1, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 16, 18};
        sprintf_s(g_dhampirVoice, "kagan_taunt_%d.wav",
                  taunts[g_dhampirVoiceSpin++ % (sizeof(taunts) / sizeof(taunts[0]))]);
        return g_dhampirVoice;
    }
    return NULL;
}

static void __fastcall HookRayneTaunt(BYTE* actor, void*, int event) {
    const auto character = ActiveChallengeCharacter();
    if (actor != *(BYTE**)RayneActor || character == ChallengeRayne) {
        ((void (__thiscall*)(BYTE*, int))0x551D30)(actor, event);
        return;
    }
    // This slot also receives Rayne's per-frame low-health hint (0x20)
    // and other automatic dialogue. Only replace the actual taunt event.
    if (event != 0x32) return;
    if (character == ChallengeDhampir || character == ChallengeKestral) {
        char name[64];
        sprintf_s(name, "dhampirf_taunt_a_%02d.wav", (int)(g_dhampirVoiceSpin++ % 4) + 1);
        ((DWORD (__thiscall*)(BYTE*, const char*))0x4032B0)(actor, name);
        return;
    }
    if (character == ChallengeDhampirM) {
        char name[64];
        sprintf_s(name, "dhampir_taunt_a_%02d.wav", (int)(g_dhampirVoiceSpin++ % 4) + 1);
        ((DWORD (__thiscall*)(BYTE*, const char*))0x4032B0)(actor, name);
        return;
    }
    if (character == ChallengeEphemera) {
        char name[64];
        sprintf_s(name, "ephemer_takethis_%d.wav", (int)(g_dhampirVoiceSpin++ % 3) + 1);
        ((DWORD (__thiscall*)(BYTE*, const char*))0x4032B0)(actor, name);
        return;
    }
    if (character == ChallengeKagan) {
        static const char* barks[] = {"kagan_laugh.wav", "kagan_yes.wav"};
        ((DWORD (__thiscall*)(BYTE*, const char*))0x4032B0)(actor,
            barks[g_dhampirVoiceSpin++ % (sizeof(barks) / sizeof(barks[0]))]);
        return;
    }
    if (character == ChallengeNinjaF || character == ChallengeNinjaM) {
        // Silent assassins taunt with their blade ignition, not a bark.
        ((DWORD (__thiscall*)(BYTE*, const char*))0x4032B0)(actor, "ninja_sword_activate.wav");
    }
}

static bool IsRayneVoiceSample(const char* sample) {
    return !_strnicmp(sample, "rayne_", 6) || !_strnicmp(sample, "rayne-", 6) ||
           !_stricmp(sample, "hero_jump.wav") || !_stricmp(sample, "hero_jump");
}

static void __fastcall HookChallengeAnimationSound(BYTE* actor, void*, const BYTE* event) {
    if (event && *(const WORD*)(event + 2) == 1 &&
        actor == *(BYTE**)RayneActor && ActiveChallengeCharacter() != ChallengeRayne) {
        const char* sample = *(const char* const*)(event + 4);
        if (sample && IsRayneVoiceSample(sample)) return;
    }
    ((void (__thiscall*)(BYTE*, const BYTE*))0x446140)(actor, event);
}

static bool ChallengeVoiceThrottled(const char* sample) {
    static char last[64];
    static DWORD lastTick, lastAnyTick;
    const DWORD now = GetTickCount();
    if (now - lastAnyTick < 150 || (!_stricmp(sample, last) && now - lastTick < 700)) return true;
    strcpy_s(last, sample);
    lastTick = lastAnyTick = now;
    return false;
}

static DWORD __fastcall HookChallengeVoice(BYTE* actor, void*, const char* sample) {
    const auto character = ActiveChallengeCharacter();
    if (g_sfxTrace && sample && actor == *(BYTE**)RayneActor)
        SfxTraceLine("voice", "char=%d ret=%08lX req='%s'",
                     (int)character, (DWORD)_ReturnAddress(), sample);
    if (sample && character != ChallengeRayne && actor == *(BYTE**)RayneActor) {
        if (character == ChallengeFerril && !_strnicmp(sample, "rayne_vc_hit_", 13))
            sample = "ferril_hurt_1.wav";
        else if (character == ChallengeFerril && !_strnicmp(sample, "rayne_vc_burn_", 14))
            sample = "ferril_hurt_3.wav";
        else if (character == ChallengeFerril && !_strnicmp(sample, "rayne_vc_die_", 13))
            sample = "ferril_hurt_5.wav";
        else if (IsRayneVoiceSample(sample)) {
            const char* voice = nullptr;
            switch (character) {
            case ChallengeDhampir:
            case ChallengeKestral:  voice = DhampirVoiceFor(sample); break;
            case ChallengeDhampirM: voice = DhampirMaleVoiceFor(sample); break;
            case ChallengeEphemera: voice = EphemeraVoiceFor(sample); break;
            case ChallengeKagan:    voice = KaganVoiceFor(sample); break;
            default: break;
            }
            if (!voice || ChallengeVoiceThrottled(voice)) return 0;
            sample = voice;
        }
    }
    return ((DWORD (__thiscall*)(BYTE*, const char*))0x4032B0)(actor, sample);
}

static bool UsesSingleChallengeSword(ChallengeCharacter character) {
    return character == ChallengeNinjaF || character == ChallengeNinjaM || character == ChallengeKagan;
}

static void* __fastcall HookChallengeLeftBladeQuery(BYTE* slot, void*) {
    BYTE* player = *(BYTE**)RayneActor;
    // Native power moves can request both blades even without a left damage range.
    if (player && slot == player + 0x47A4 && UsesSingleChallengeSword(ActiveChallengeCharacter()))
        return nullptr;
    return ((void* (__thiscall*)(BYTE*))0x666F30)(slot);
}

// CAnimatedMeleeWeapon holds a model instance at +0x158 and a CBladeSwipe
// at +0x484. Resolve ownership through the two native player equipment slots.
static const char* EphemeraBladeMount(const BYTE* weapon, BYTE* player) {
    if (g_activeCharacter != ChallengeEphemera || !player ||
        ActiveChallengeCharacter() != ChallengeEphemera)
        return nullptr;
    const auto blade = (const BYTE* (__thiscall*)(BYTE*))0x666F30;
    if (weapon == blade(player + 0x47A4)) return "bladeL";
    if (weapon == blade(player + 0x481C)) return "bladeR";
    return nullptr;
}

static void __fastcall HookEphemeraBladeMesh(BYTE* model, void*, int pass) {
    if (!EphemeraBladeMount(model - 0x158, *(BYTE**)RayneActor))
        ((void (__thiscall*)(BYTE*, int))0x57D5F0)(model, pass);
}

static void __fastcall HookEphemeraBladeTrail(BYTE* trail, void*, const float* base, const float* tip) {
    const auto add = (void (__thiscall*)(BYTE*, const float*, const float*))0x424150;
    if (trail[0xE0] || *(const float*)(trail + 0xDC) > 0.0f) {
        BYTE* player = *(BYTE**)RayneActor;
        const char* mount = EphemeraBladeMount(trail - 0x484, player);
        if (mount) {
            const auto resolve = (BYTE (__thiscall*)(BYTE*, const char*, float*, int))0x608C10;
            float modelMount[12];
            if (resolve(player + 0x158, mount, modelMount, 0)) {
                const auto actorMatrix = (void (__thiscall*)(float*, const void*, const void*))0x690170;
                const auto compose = (float* (__thiscall*)(const float*, float*, const float*))0x68FAE0;
                float actorToWorld[12], worldMount[12];
                actorMatrix(actorToWorld, player + 0x38, player + 0x44);
                compose(modelMount, worldMount, actorToWorld);

                // CEphemra samples the mount origin and its local +Z endpoint
                // at full attack length (3 units, 0x4AC7E6). Keep the weapon's
                // native swipe timing and render this trail instead of metal.
                constexpr float length = 3.0f;
                const float root[] = {worldMount[3], worldMount[7], worldMount[11]};
                const float end[] = {
                    root[0] + length * worldMount[2],
                    root[1] + length * worldMount[6],
                    root[2] + length * worldMount[10],
                };
                add(trail, root, end);
                return;
            }
        }
    }
    add(trail, base, tip);
}

static int __fastcall HookChallengeBladeRender(void* weapon, void*) {
    // Ferril's claws are baked into her mesh. Her mounted blades drive collision only.
    // Kagan and the ninjas animate one sword. Keep the left mount for
    // the paired player equipment routines and skip its model and trails.
    const auto character = ActiveChallengeCharacter();
    if (character == ChallengeFerril || UsesSingleChallengeSword(character)) {
        BYTE* player = *(BYTE**)RayneActor;
        const auto blade = (void* (__thiscall*)(void*))0x666F30;
        if (player && (weapon == blade(player + 0x47A4) ||
            (character == ChallengeFerril && weapon == blade(player + 0x481C))))
            return 0;
    }
    return ((int (__thiscall*)(void*))0x407390)(weapon);
}
