namespace {
constexpr DWORD ConventionalGunVtable = 0x6FAA44;
constexpr DWORD LiveWeaponTag = 0x78A123;
constexpr DWORD InactiveWeaponTag = 0x6F3874E;
constexpr int ChallengeStartingRounds = 999;
// Native CGun types 0..2: Automag, sawed-off shotgun and Uzi.
constexpr unsigned ConventionalTypes = 3;
constexpr unsigned BloodModes = 6;
constexpr unsigned WheelEntries = ConventionalTypes + BloodModes;
constexpr unsigned BloodSlot = 0;
constexpr int HandSlots[] = {2, 0};
constexpr int RightGunHand = 2;
constexpr int LeftGunHand = 0;
constexpr unsigned LeftWeaponController = 0x47A4;
constexpr unsigned RightWeaponController = 0x481C;

struct ArsenalInput {
    bool wheel, fire, previous, next, equipRight, equipLeft;
    float horizontal, vertical;
};
struct DroppedGun {
    BYTE* gun;
    float remaining;
    bool waitForExit;
};

struct Arsenal {
    BYTE* rayne;
    void* situation;
    BYTE* guns[ConventionalTypes];
    float shotCooldown[ConventionalTypes][2];
    ArsenalInput previous;
    unsigned hands[2], preview;
    bool wheel, drawn, primed, fireBlocked, bloodInitialized;
    bool aimRequested;
    bool startingGunsChecked;
    bool dimmed;
    BYTE* fadeObject;
    float fadeSaved[5];
    DWORD holdStart[2];
    bool holdDropped[2];
    DroppedGun dropped[64];
    DWORD lastPoseSample;
    DWORD noticeUntil;
    const char* notice;
    int noticeRounds;
};
Arsenal g_arsenal;
bool g_arsenalReady;

template<class T> T& ArsenalField(BYTE* object, unsigned offset) {
    return *reinterpret_cast<T*>(object + offset);
}

// Native screen fade owned by the script object (PhotoScript), as written by
// the fade(visibility,r,g,b,duration) script command. +0x20C is visibility
// (1.0 = clear screen), the other floats are the cover color and duration.
constexpr unsigned FadeStateOffset = 0x200;

void RestoreArsenalFade() {
    if (!g_arsenal.dimmed) return;
    // Only the object that produced the saved state is safe to write back;
    // a scene change may have recycled the script global meanwhile.
    if (g_arsenal.fadeObject && g_arsenal.fadeObject == *(BYTE**)PhotoScript)
        memcpy(g_arsenal.fadeObject + FadeStateOffset, g_arsenal.fadeSaved,
               sizeof g_arsenal.fadeSaved);
    g_arsenal.dimmed = false;
    g_arsenal.fadeObject = nullptr;
}

void SyncArsenalFade() {
    if (!g_arsenal.wheel) { RestoreArsenalFade(); return; }
    auto* object = *(BYTE**)PhotoScript;
    if (!object) { g_arsenal.dimmed = false; g_arsenal.fadeObject = nullptr; return; }
    if (!g_arsenal.dimmed || g_arsenal.fadeObject != object) {
        memcpy(g_arsenal.fadeSaved, object + FadeStateOffset, sizeof g_arsenal.fadeSaved);
        g_arsenal.fadeObject = object;
        g_arsenal.dimmed = true;
    }
    auto* fade = reinterpret_cast<float*>(object + FadeStateOffset);
    fade[0] = fade[1] = fade[2] = 0.f;
    fade[3] = g_enhanced.conventionalWheelDim;
    fade[4] = 0.f;
}

bool ArsenalEnabled() {
    return g_arsenalReady && g_combat == CombatEnhanced &&
           CombatEffectEnabled(EffectGunCombat);
}

bool ArsenalWheelOpen() { return g_arsenalReady && g_arsenal.wheel; }

void AlignConventionalGrip(BYTE* gun, BYTE* rayne, int hand) {
    // Grip offsets in model space. The native attachment supplies
    // the palm and orientation for each side.
    constexpr PhotoVec grips[] = {
        {0.f, .034f, .155f}, // Automag
        {0.f, -.06f, .155f}, // sawed-off
        {0.f, .006f, .155f}, // Uzi
    };
    static_assert(_countof(grips) == ConventionalTypes, "Grip per supported model");
    const unsigned type = ArsenalField<unsigned>(gun, 0x1CC);
    if (type >= ConventionalTypes) return;
    const auto& grip = grips[type];
    auto& position = ArsenalField<PhotoVec>(gun, 0x38);
    const auto* basis = reinterpret_cast<const float*>(gun + 0x50);
    // Subtract the local grip from the hand anchor. Increasing its depth
    // moves the model back toward the palm, along its own barrel axis.
    const float gripDepth = grip.z + .145f;
    const float gripHeight = grip.y - .033f;
    position.x -= basis[0]*grip.x + basis[1]*gripHeight + basis[2]*gripDepth;
    position.y -= basis[3]*grip.x + basis[4]*gripHeight + basis[5]*gripDepth;
    position.z -= basis[6]*grip.x + basis[7]*gripHeight + basis[8]*gripDepth;
    if (type == 0) {
        // Use Rayne's sideways axis so wrist roll doesn't turn this into
        // a vertical adjustment. Mirror the offset for the left hand.
        const auto* actorBasis = reinterpret_cast<const float*>(rayne + 0x50);
        const float outward = hand == RightGunHand ? .007f : -.007f;
        position.x += outward * actorBasis[0];
        position.z += outward * actorBasis[6];
    }
}

BYTE* SelectedConventionalGun(unsigned slot) {
    return slot && slot <= ConventionalTypes ? g_arsenal.guns[slot - 1] : nullptr;
}

BYTE* EquippedConventional(unsigned hand) {
    return SelectedConventionalGun(g_arsenal.hands[hand]);
}

int EquippedGunHand(BYTE* gun) {
    for (unsigned hand = 0; gun && hand < 2; ++hand)
        if (gun == EquippedConventional(hand)) return HandSlots[hand];
    return -1;
}

bool MountConventionalGun(BYTE* rayne, BYTE* gun, int hand) {
    if (!rayne || !gun || hand < 0 || hand >= ArsenalField<int>(rayne, 0x3954)) return false;
    BYTE* hands = ArsenalField<BYTE*>(rayne, 0x3958);
    if (!hands) return false;
    // CCharacter's own hand transform, shared with pickupObjectNow.
    // Loan the slot for the transform only. Stock weapon controllers retain
    // their CMultigun pointers, state, links and ammunition.
    BYTE* record = hands + hand * 0x28;
    float transform[12];
    if (!((BYTE (__thiscall*)(BYTE*, BYTE*, float*, int))0x608C10)
            (rayne + 0x158, record, transform, 0)) return false;
    BYTE*& occupant = ArsenalField<BYTE*>(record, 0x24);
    BYTE* original = occupant;
    __try {
        occupant = gun;
        ((void (__thiscall*)(BYTE*, int, float, int))0x4450A0)(rayne, hand, 0.f, 0);
    } __finally {
        occupant = original;
    }
    return true;
}

void DropConventionalGun(BYTE* gun) {
    if (!gun || ArsenalField<DWORD>(gun, 0) != ConventionalGunVtable ||
        ArsenalField<BYTE*>(gun, 0x1F4) != g_arsenal.rayne) return;
    const int hand = EquippedGunHand(gun);
    MountConventionalGun(g_arsenal.rayne, gun, hand < 0 ? RightGunHand : hand);
    // CWeapon::drop clears carriedByActor and resets its native physics.
    ((void (__thiscall*)(BYTE*, const PhotoVec*))0x683DC0)(gun, nullptr);
}

void ClearConventionalArsenal(bool release) {
    if (release && g_arsenal.rayne && g_arsenal.rayne == *(BYTE**)0x7DCD68 &&
        g_arsenal.situation && g_arsenal.situation == *(void**)G_SITUATION) {
        for (auto* gun : g_arsenal.guns) {
            __try { DropConventionalGun(gun); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
    RestoreArsenalFade();
    g_arsenal = {};
}

void __fastcall OnConventionalGunDestroyed(BYTE* gun, void*) {
    for (auto& dropped : g_arsenal.dropped)
        if (dropped.gun == gun) dropped = {};
    for (auto& stored : g_arsenal.guns)
        if (stored == gun) stored = nullptr;
    for (auto& slot : g_arsenal.hands)
        if (slot != BloodSlot && !SelectedConventionalGun(slot)) slot = BloodSlot;
    ((void (__thiscall*)(BYTE*))0x507940)(gun);
}

bool ArsenalSceneMatches() {
    return g_arsenal.rayne && g_arsenal.rayne == *(BYTE**)0x7DCD68 &&
           g_arsenal.situation && g_arsenal.situation == *(void**)G_SITUATION;
}

BYTE* ArsenalWeaponController(BYTE* rayne, int hand) {
    return rayne + (hand == RightGunHand ? RightWeaponController : LeftWeaponController);
}

int __fastcall DrawConventionalGun(BYTE* gun, void*) {
    __try {
        if (ArsenalEnabled() && ArsenalSceneMatches()) {
            for (auto* stored : g_arsenal.guns) {
                if (stored != gun) continue;
                if (ArsenalField<BYTE*>(gun, 0x1F4) != g_arsenal.rayne) break;
                return 0; // player-owned models are submitted with Rayne
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // Original CGun draw function at 0x507FF0 before the CWeapon draw.
    const DWORD* methods = ArsenalField<DWORD*>(gun, 0);
    if (((BYTE (__thiscall*)(BYTE*))methods[0x100 / 4])(gun))
        ((void (__thiscall*)(BYTE*))methods[0xCC / 4])(gun);
    return ((int (__thiscall*)(BYTE*))0x6830B0)(gun);
}

void DrawConventionalPlayerWeapons(BYTE* rayne) {
    if (!ArsenalEnabled() || rayne != g_arsenal.rayne ||
        !ArsenalSceneMatches() || !g_arsenal.drawn) return;
    for (unsigned index = 0; index < 2; ++index) {
        auto* gun = EquippedConventional(index);
        if (!gun || ArsenalField<BYTE*>(gun, 0x1F4) != rayne ||
            !MountConventionalGun(rayne, gun, HandSlots[index])) continue;
        const auto savedPosition = ArsenalField<PhotoVec>(gun, 0x38);
        AlignConventionalGrip(gun, rayne, HandSlots[index]);
        // Submit the mounted model just like the native blood gun renderer,
        // without depending on the ground weapon's visibility bounds.
        ((void (__thiscall*)(BYTE*))0x4031C0)(gun);
        __try {
            ((void (__thiscall*)(BYTE*, int))0x57D5F0)(gun + 0x158, 0);
        } __finally {
            ((void (__thiscall*)(BYTE*))0x4031E0)(gun);
            ArsenalField<PhotoVec>(gun, 0x38) = savedPosition;
        }
    }
}

BYTE* ArsenalBloodGun(BYTE* rayne, unsigned hand) {
    auto* gun = ((BYTE* (__thiscall*)(BYTE*))0x666F10)(
        ArsenalWeaponController(rayne, HandSlots[hand]));
    return gun && ArsenalField<DWORD>(gun, 0) == 0x70BE4C ? gun : nullptr;
}

bool BloodModeAvailable(BYTE* rayne, unsigned mode);

unsigned CurrentBloodMode(BYTE* rayne) {
    const unsigned mode = ArsenalField<unsigned>(rayne, 0x1139C);
    return mode < BloodModes ? mode : 0;
}

bool ArsenalGameplayInput(BYTE* game, BYTE* rayne) {
    auto* script = *(BYTE**)PhotoScript;
    return ArsenalEnabled() && game && rayne && *(void**)G_SITUATION &&
        ArsenalField<DWORD>(rayne, 0) == 0x7034E4 &&
        (!IsStandaloneChallengeLevel((const char*)G_CURRENT_LEVEL) ||
         (CurrentChallengeAllowsArsenal() && ActiveChallengeCharacter() == ChallengeRayne)) &&
        !g_photoBlockingMenu && !*(int*)G_MENU_STATE && !*(BYTE*)0x6030408 &&
        (!script || !ArsenalField<DWORD>(script, 8)) &&
        !ArsenalField<int>(game, 0x324) && !ArsenalField<int>(game, 0x39C) &&
        !ArsenalField<int>(game, 0x390) && ArsenalField<int>(rayne, 0x3A48) >= 0 &&
        !ArsenalField<void*>(rayne, 0x3A44) &&
        BloodModeAvailable(rayne, CurrentBloodMode(rayne));
}

bool ArsenalManagesBlood(BYTE* rayne) {
    return ArsenalEnabled() && ArsenalSceneMatches() && rayne == g_arsenal.rayne &&
           g_arsenal.bloodInitialized && BloodModeAvailable(rayne, CurrentBloodMode(rayne));
}

bool BloodInHand(unsigned hand) { return g_arsenal.hands[hand] == BloodSlot; }

enum ArsenalTraceStage { TracePoseBefore, TracePoseAfter, TraceArsenalEvent, ArsenalTraceStages };
static char g_arsenalTracePath[MAX_PATH];

void InitArsenalPoseTrace() {
    if (!GetPrivateProfileIntA("Diagnostics", "ArsenalPoseTrace", 0, ".\\br2mod.ini")) return;
    const DWORD length = GetModuleFileNameA(nullptr, g_arsenalTracePath, MAX_PATH);
    if (!length || length >= MAX_PATH) { g_arsenalTracePath[0] = 0; return; }
    auto* name = strrchr(g_arsenalTracePath, '\\');
    if (!name || strcpy_s(name + 1, MAX_PATH - (name + 1 - g_arsenalTracePath),
                          "br2mod_arsenal_pose.log")) {
        g_arsenalTracePath[0] = 0;
        return;
    }
    FILE* out = nullptr;
    if (fopen_s(&out, g_arsenalTracePath, "w")) { g_arsenalTracePath[0] = 0; return; }
    fprintf(out, "Arsenal pose trace pid=%lu. Hand values: 0=blood, 1=pistol, 2=shotgun, 3=Uzi.\n",
            GetCurrentProcessId());
    fclose(out);
}

void LogArsenalPose(BYTE* rayne, ArsenalTraceStage stage, const char* reason, float delta = 0.f) {
    if (!g_arsenalTracePath[0] || rayne != g_arsenal.rayne || !ArsenalSceneMatches()) return;
    static char last[ArsenalTraceStages][1024];
    char state[1024];
    // Diagnostics must not affect gameplay if an object becomes unreadable.
    __try {
        auto* right = ArsenalWeaponController(rayne, RightGunHand);
        auto* left = ArsenalWeaponController(rayne, LeftGunHand);
        auto* rightBlood = ArsenalBloodGun(rayne, 0);
        auto* leftBlood = ArsenalBloodGun(rayne, 1);
        auto* rightGun = EquippedConventional(0);
        auto* leftGun = EquippedConventional(1);
        auto* script = *(BYTE**)PhotoScript;
        auto* game = *(BYTE**)G_GAME_STATE;
        const int count = sprintf_s(state,
            "%s level=%s rayne=%p situation=%p anim=0x%X menu=%d photo=%d script=%lu dtPositive=%d "
            "hands(R,L)=%u,%u ammo=%d,%d bloodMode=%u drawn=%d pending=%d wheel=%d primed=%d "
            "lock=%d fire=%d blade=%d fireBlocked=%d ready=%.2f blend=%.1f,%.1f "
            "queued=%d,%d bloodState=%d,%d targets=%p,%p candidates=%d specialTarget=%p "
            "gameGates=%d,%d,%d nativePause=%u actorGate=%d actorBlock=%p",
            reason, LevelBasename((const char*)G_CURRENT_LEVEL), rayne, g_arsenal.situation,
            ArsenalField<unsigned>(rayne, 0x17C), *(int*)G_MENU_STATE, (int)g_photoBlockingMenu,
            script ? ArsenalField<DWORD>(script, 8) : 0ul, delta > 0.f,
            g_arsenal.hands[0], g_arsenal.hands[1],
            rightGun ? ArsenalField<int>(rightGun, 0x29C) : -1,
            leftGun ? ArsenalField<int>(leftGun, 0x29C) : -1, CurrentBloodMode(rayne),
            g_arsenal.drawn, g_arsenal.aimRequested, g_arsenal.wheel, g_arsenal.primed,
            ArsenalField<int>(rayne, 0x3A70), ArsenalField<int>(rayne, 0x3A98),
            ArsenalField<int>(rayne, 0x3A88), g_arsenal.fireBlocked,
            ArsenalField<float>(rayne, 0x82B0),
            ArsenalField<float>(right, 0x5C), ArsenalField<float>(left, 0x5C),
            ArsenalField<int>(right, 8), ArsenalField<int>(left, 8),
            rightBlood ? ArsenalField<int>(rightBlood, 0x7F4) : -1,
            leftBlood ? ArsenalField<int>(leftBlood, 0x7F4) : -1,
            ArsenalField<void*>(right, 0x24), ArsenalField<void*>(left, 0x24),
            ArsenalField<int>(rayne, 0x8180), ArsenalField<void*>(rayne, 0x4908),
            game ? ArsenalField<int>(game, 0x324) : -1,
            game ? ArsenalField<int>(game, 0x39C) : -1,
            game ? ArsenalField<int>(game, 0x390) : -1, (unsigned)*(BYTE*)0x6030408,
            ArsenalField<int>(rayne, 0x3A48), ArsenalField<void*>(rayne, 0x3A44));
        if (count < 0 || (stage != TraceArsenalEvent && !strcmp(last[stage], state))) return;
        FILE* out = nullptr;
        if (fopen_s(&out, g_arsenalTracePath, "a")) return;
        fprintf(out, "%lu %s\n", GetTickCount(), state);
        fclose(out);
        if (stage != TraceArsenalEvent) strcpy_s(last[stage], state);
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void __fastcall UpdateArsenalWeaponPose(BYTE* rayne, void*, float delta) {
    const auto update = (void (__thiscall*)(BYTE*, float))0x6711A0;
    const bool lockOn = ArsenalField<int>(rayne, 0x3A70) == 1;
    const bool ready = ArsenalField<float>(rayne, 0x82B0) > 0.f;
    // The stock lock-on path can lower an already raised weapon. Keep
    // the request pending until it can be honored outside lock-on.
    if (lockOn && ready && g_arsenal.drawn && rayne == g_arsenal.rayne &&
        (EquippedConventional(0) || EquippedConventional(1)))
        g_arsenal.aimRequested = true;
    const char* reason = g_arsenal.aimRequested ? "pose-wait-gate" : "pose-no-request";
    if (g_arsenal.aimRequested && ArsenalManagesBlood(rayne) && !g_arsenal.wheel &&
        delta > 0.f && !lockOn &&
        ArsenalGameplayInput(*(BYTE**)G_GAME_STATE, rayne)) {
        if (!g_arsenal.drawn || (!EquippedConventional(0) && !EquippedConventional(1)) ||
            ArsenalField<int>(rayne, 0x3A88) == 1) {
            g_arsenal.aimRequested = false;
            reason = "pose-cancelled";
        } else if (ready) {
            g_arsenal.aimRequested = false;
            reason = "pose-already-ready";
        } else {
            reason = "pose-native-denied";
            const auto* methods = ArsenalField<DWORD*>(rayne, 0);
            const auto canStart = (BYTE (__thiscall*)(BYTE*))methods[0x210 / 4];
            if (canStart(rayne)) {
                LogArsenalPose(rayne, TraceArsenalEvent, "pose-accepted", delta);
                g_arsenal.aimRequested = false;
                ((void (__thiscall*)(BYTE*))0x677F60)(rayne);
                // The native pose update raises the arms when either
                // controller has a queued shot. Scope that signal to the
                // pose update, before the separate projectile updates.
                auto& queued = ArsenalField<int>(ArsenalWeaponController(rayne, RightGunHand), 8);
                const int original = queued;
                queued = 1;
                LogArsenalPose(rayne, TracePoseBefore, "pose-before-queued", delta);
                __try { update(rayne, delta); }
                __finally { if (queued == 1) queued = original; }
                LogArsenalPose(rayne, TracePoseAfter, "pose-after-queued", delta);
                return;
            }
        }
    }
    LogArsenalPose(rayne, TracePoseBefore, reason, delta);
    update(rayne, delta);
    LogArsenalPose(rayne, TracePoseAfter, "pose-after", delta);
}

BYTE* SharedBloodReserve(BYTE* gun) {
    auto* next = ArsenalField<BYTE*>(gun, 0x68C);
    return next && next != gun && ArsenalField<DWORD>(next, 0) == 0x70BE4C &&
           ArsenalField<BYTE*>(next, 0x1F4) == ArsenalField<BYTE*>(gun, 0x1F4)
        ? next : gun;
}

bool UsesSplitBloodCost(BYTE* gun) {
    return ArsenalManagesBlood(ArsenalField<BYTE*>(gun, 0x1F4));
}

float ConfiguredBloodPairCost(BYTE* gun) {
    const unsigned mode = ArsenalField<unsigned>(gun, 0x7EC);
    const unsigned level = ArsenalField<unsigned>(gun, 0x694);
    if (mode >= BloodModes || level >= 3) return 0.f;
    const float cost = ArsenalField<float>(gun, GUN_MODE_OFFSETS[mode] + 0x20 + level * 4);
    return _finite(cost) && cost > 0.f && cost <= 1000.f ? cost : 0.f;
}

int NativeBloodCost(BYTE* gun) {
    return ((int (__thiscall*)(BYTE*))0x584B00)(gun);
}

int NativeBloodReserve(BYTE* gun) {
    auto* vtable = ArsenalField<DWORD*>(gun, 0);
    return ((int (__thiscall*)(BYTE*))vtable[0x15C / 4])(gun);
}

float __fastcall BloodProjectileCost(BYTE* gun, void*) {
    // The native debit has already followed the link to the shared reserve.
    if (UsesSplitBloodCost(gun)) {
        const float pair = ConfiguredBloodPairCost(gun);
        if (pair > 0.f) return pair * .5f;
    }
    return (float)NativeBloodCost(gun);
}

float __fastcall BloodShotShortfall(BYTE* gun, void*) {
    if (UsesSplitBloodCost(gun)) {
        auto* reserve = SharedBloodReserve(gun);
        const float pair = ConfiguredBloodPairCost(reserve);
        if (pair > 0.f) return pair * .5f - ArsenalField<float>(reserve, 0x7FC);
    }
    auto* next = ArsenalField<BYTE*>(gun, 0x68C);
    const int available = NativeBloodReserve(next ? next : gun);
    return (float)(NativeBloodCost(gun) - available);
}

void __declspec(naked) LoadBloodShotShortfall() {
    __asm {
        mov ecx, esi
        jmp BloodShotShortfall
    }
}

float __fastcall BloodHudReserve(BYTE* gun, void*) {
    if (UsesSplitBloodCost(gun))
        return ArsenalField<float>(SharedBloodReserve(gun), 0x7FC);
    return (float)NativeBloodReserve(gun);
}

void __declspec(naked) LoadBloodHudReserve() {
    __asm {
        mov ecx, edi
        call BloodHudReserve
        mov ecx, edi
        ret
    }
}

float __fastcall BloodHudCost(BYTE* gun, void*) {
    if (UsesSplitBloodCost(gun)) {
        const float pair = ConfiguredBloodPairCost(SharedBloodReserve(gun));
        if (pair > 0.f) return BloodInHand(0) && BloodInHand(1) ? pair : pair * .5f;
    }
    return (float)NativeBloodCost(gun);
}

void __fastcall DrawBloodGunModel(BYTE* model, void*, int flags) {
    auto* gun = model - 0x158;
    auto* rayne = ArsenalField<BYTE*>(gun, 0x1F4);
    if (ArsenalManagesBlood(rayne)) {
        for (unsigned hand = 0; hand < 2; ++hand)
            if (gun == ArsenalBloodGun(rayne, hand) && !BloodInHand(hand)) return;
    }
    ((void (__thiscall*)(BYTE*, int))0x57D5F0)(model, flags);
}

void __fastcall StartBloodGunFireOrSuppress(BYTE* rayne, void*) {
    const auto start = (void (__thiscall*)(BYTE*))0x66DCE0;
    if (!ArsenalManagesBlood(rayne)) { start(rayne); return; }
    const bool right = BloodInHand(0), left = BloodInHand(1);
    if (g_arsenal.wheel || g_arsenal.fireBlocked || (!right && !left)) {
        LogArsenalPose(rayne, TraceArsenalEvent, "blood-start-suppressed");
        return;
    }
    LogArsenalPose(rayne, TraceArsenalEvent, "blood-start-native");
    start(rayne);
    if (right == left) return;
    auto& equipped = ArsenalField<int>(ArsenalWeaponController(rayne, right ? RightGunHand : LeftGunHand), 8);
    auto& stowed = ArsenalField<int>(ArsenalWeaponController(rayne, right ? LeftGunHand : RightGunHand), 8);
    if (stowed == 1) {
        if (!equipped) equipped = 1;
        stowed = 0;
    }
}

void __fastcall FireBloodGunOrSuppress(BYTE* gun, void*, int trigger, int mode) {
    auto* rayne = ArsenalField<BYTE*>(gun, 0x1F4);
    if (ArsenalManagesBlood(rayne)) {
        bool suppress = g_arsenal.wheel || g_arsenal.fireBlocked;
        for (unsigned hand = 0; hand < 2; ++hand)
            if (gun == ArsenalBloodGun(rayne, hand) && !BloodInHand(hand)) suppress = true;
        if (suppress) {
            // Drop the deferred shot that the native draw/update loop would retry.
            ArsenalField<BYTE>(gun, 0x6BC) = 0;
            return;
        }
    }
    ((void (__thiscall*)(BYTE*, int, int))0x5863B0)(gun, trigger, mode);
}

bool BloodModeAvailable(BYTE* rayne, unsigned mode) {
    auto* progress = *(BYTE**)0x7BF4BC;
    return mode < BloodModes && progress &&
        (ArsenalField<unsigned>(progress, 0x34) & (1u << mode)) &&
        ArsenalBloodGun(rayne, 0) && ArsenalBloodGun(rayne, 1);
}

void SelectBloodMode(unsigned mode) {
    auto* rayne = g_arsenal.rayne;
    if (mode != CurrentBloodMode(rayne))
        ((BYTE (__thiscall*)(BYTE*, unsigned, int))0x5B5070)(rayne + 0x11380, mode, 1);
    ((void (__thiscall*)(BYTE*))0x54D4D0)(rayne);
}

float GunDistanceSquared(BYTE* gun, const PhotoVec& position) {
    const auto& point = ArsenalField<PhotoVec>(gun, 0x38);
    const float x = point.x - position.x, y = point.y - position.y, z = point.z - position.z;
    return x*x + y*y + z*z;
}

int AvailableAmmoSpace(BYTE* gun) {
    // CGun::reload (0x507E50) copies the configured full load into rounds.
    const int capacity = ArsenalField<int>(gun, 0x2A0);
    const int rounds = max(0, ArsenalField<int>(gun, 0x29C));
    return capacity > rounds ? capacity - rounds : 0;
}

BYTE* NearestGroundGun(BYTE* rayne) {
    auto* world = *(void**)0x7C5D50;
    if (!world) return nullptr;
    const auto& position = ArsenalField<PhotoVec>(rayne, 0x38);
    const float radius = g_enhanced.conventionalPickupRadius;
    float best = radius * radius;
    BYTE* nearest = nullptr;
    BYTE* nearby[64];
    const auto query = (int (__thiscall*)(void*, const PhotoVec*, float, BYTE**, int, const char*))0x5E27D0;
    const int count = max(0, min((int)_countof(nearby),
        query(world, &position, radius, nearby, _countof(nearby), "CWeapon")));
    for (int i = 0; i < count; ++i) {
        auto* gun = nearby[i];
        if (!gun || ArsenalField<DWORD>(gun, 0) != ConventionalGunVtable ||
            ArsenalField<DWORD>(gun, 0x28) != LiveWeaponTag ||
            ArsenalField<BYTE*>(gun, 0x1F4) || ArsenalField<int>(gun, 0x1C8) ||
            ArsenalField<unsigned>(gun, 0x1CC) >= ConventionalTypes ||
            ArsenalField<int>(gun, 0x29C) <= 0) continue;
        const float distance = GunDistanceSquared(gun, position);
        if (!_finite(distance) || distance > best) continue;
        bool discarded = false;
        for (const auto& item : g_arsenal.dropped)
            if (item.gun == gun && item.waitForExit) { discarded = true; break; }
        if (discarded) continue;
        best = distance;
        nearest = gun;
    }
    return nearest;
}

void ProbeArsenalGuns(BYTE* rayne) {
    auto* world = *(void**)0x7C5D50;
    // Anchor to the exe dir: the launcher's CWD may not be the game dir.
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, sizeof(path));
    char* last = strrchr(path, '\\');
    if (last) last[1] = 0;
    strcat_s(path, "MODDING\\arsenal_probe.txt");
    FILE* out = fopen(path, "a");
    if (!out) out = fopen("arsenal_probe.txt", "a");
    if (!out) {
        out = fopen("C:\\arsenal_probe.txt", "a");
        if (!out) return;
    }
    char cwd[MAX_PATH] = "";
    GetCurrentDirectoryA(sizeof(cwd), cwd);
    fprintf(out, "(cwd=%s probe_path=%s)\n", cwd, path);
    const auto& pos = ArsenalField<PhotoVec>(rayne, 0x38);
    fprintf(out, "=== rayne=%p pos=%g,%g,%g world=%p radius=%g ===\n",
            rayne, pos.x, pos.y, pos.z, world, g_enhanced.conventionalPickupRadius);
    BYTE* nearby[64];
    const auto query = (int (__thiscall*)(void*, const PhotoVec*, float, BYTE**, int, const char*))0x5E27D0;
    const int count = world ? max(0, min((int)_countof(nearby),
        query(world, &pos, 20.f, nearby, _countof(nearby), "CWeapon"))) : -1;
    fprintf(out, "query CWeapon r=20 -> %d\n", count);
    for (int i = 0; i < count; ++i) {
        auto* gun = nearby[i];
        if (!gun) continue;
        const float d = sqrtf(GunDistanceSquared(gun, pos));
        fprintf(out, "  %p vt=%08X tag=%08X carrier=%p wstate=%d type=%u ammo=%d cap=%d dist=%g\n",
                gun, ArsenalField<DWORD>(gun, 0), ArsenalField<DWORD>(gun, 0x28),
                ArsenalField<BYTE*>(gun, 0x1F4), ArsenalField<int>(gun, 0x1C8),
                ArsenalField<unsigned>(gun, 0x1CC), ArsenalField<int>(gun, 0x29C),
                ArsenalField<int>(gun, 0x2A0), d);
    }
    for (unsigned i = 0; i < ConventionalTypes; ++i)
        if (g_arsenal.guns[i]) {
            auto* gun = g_arsenal.guns[i];
            fprintf(out, "  stored[%u]=%p carrier=%p ammo=%d cap=%d tag=%08X\n", i, gun,
                    ArsenalField<BYTE*>(gun, 0x1F4), ArsenalField<int>(gun, 0x29C),
                    ArsenalField<int>(gun, 0x2A0), ArsenalField<DWORD>(gun, 0x28));
        }
    fprintf(out, "hands=%u,%u wheel=%d\n\n", g_arsenal.hands[0], g_arsenal.hands[1], g_arsenal.wheel);
    fclose(out);
}

void SetArsenalNotice(const char* label, DWORD now, int rounds = 0) {
    g_arsenal.notice = label;
    g_arsenal.noticeUntil = now + 1800;
    g_arsenal.noticeRounds = rounds;
}

bool StoreConventionalGun(BYTE* gun, unsigned index) {
    auto* rayne = g_arsenal.rayne;
    if (!MountConventionalGun(rayne, gun, RightGunHand)) return false;
    ((void (__thiscall*)(BYTE*, BYTE*))0x507FA0)(gun, rayne);
    if (ArsenalField<BYTE*>(gun, 0x1F4) != rayne) return false;
    for (auto& item : g_arsenal.dropped)
        if (item.gun == gun) item = {};
    g_arsenal.guns[index] = gun;
    auto& cooldowns = g_arsenal.shotCooldown[index];
    cooldowns[0] = cooldowns[1] = max(0.f, ArsenalField<float>(gun, 0x2C0));
    return true;
}

void GrantChallengeStartingGuns() {
    if (g_arsenal.startingGunsChecked) return;
    g_arsenal.startingGunsChecked = true;
    if (!g_allGuns || !CurrentChallengeGrantsAllGuns()) return;

    // Resolve the level's authored guns by name, including dormant actors
    // outside the spatial pickup query. Validate the whole set before granting.
    const auto findActor = (BYTE* (__thiscall*)(void*, const char*))0x578CF0;
    const auto setPresent = (void (__thiscall*)(void*, BYTE*, int))0x578EF0;
    BYTE* guns[ConventionalTypes] = {};
    for (unsigned index = 0; index < ConventionalTypes; ++index) {
        char name[32];
        // The level provides two authored actors per type. Use one for the pair.
        sprintf_s(name, "ArsenalGun%u", index * 2);
        auto* gun = findActor(g_arsenal.situation, name);
        if (!gun || ArsenalField<DWORD>(gun, 0) != ConventionalGunVtable ||
            ArsenalField<unsigned>(gun, 0x1CC) != index ||
            ArsenalField<BYTE*>(gun, 0x1F4)) return;
        const DWORD tag = ArsenalField<DWORD>(gun, 0x28);
        if (tag != LiveWeaponTag && tag != InactiveWeaponTag) return;
        guns[index] = gun;
    }
    for (unsigned index = 0; index < ConventionalTypes; ++index) {
        auto* gun = guns[index];
        const bool inactive = ArsenalField<DWORD>(gun, 0x28) == InactiveWeaponTag;
        if (inactive) setPresent(g_arsenal.situation, gun, 1);
        if (!StoreConventionalGun(gun, index)) {
            if (inactive) setPresent(g_arsenal.situation, gun, 0);
            continue;
        }
        auto& capacity = ArsenalField<int>(gun, 0x2A0);
        capacity = max(capacity, ChallengeStartingRounds);
        ArsenalField<int>(gun, 0x29C) = capacity;
    }
}

bool PickUpConventionalGun(BYTE* gun, DWORD now) {
    const unsigned type = ArsenalField<unsigned>(gun, 0x1CC);
    if (type >= ConventionalTypes) return false;
    auto* stored = g_arsenal.guns[type];
    if (!stored) {
        if (!StoreConventionalGun(gun, type)) return false;
        const bool autoEquip = BloodInHand(0) || BloodInHand(1);
        if (autoEquip) {
            for (auto& hand : g_arsenal.hands) hand = type + 1;
            g_arsenal.drawn = g_arsenal.aimRequested = true;
        }
        SetArsenalNotice(autoEquip ? "Arsenal Equipped" : "Arsenal Stored", now);
        LogArsenalPose(g_arsenal.rayne, TraceArsenalEvent, autoEquip ? "pickup-equipped" : "pickup-stored");
        return true;
    }
    auto* manager = *(void**)0x7C2478;
    if (!manager) return false;
    const int initial = max(0, ArsenalField<int>(gun, 0x29C));
    const int moved = min(initial, AvailableAmmoSpace(stored));
    if (moved) {
        auto& rounds = ArsenalField<int>(stored, 0x29C);
        rounds = max(0, rounds) + moved;
    }
    ArsenalField<int>(gun, 0x29C) = 0;
    ((void (__thiscall*)(void*, BYTE*, int))0x578EF0)(manager, gun, 0);
    if (initial) SetArsenalNotice(moved ? "Arsenal Ammo" : "Arsenal Ammo Full", now, moved);
    return true;
}

void TickDroppedGuns(float delta) {
    if (!_finite(delta) || delta <= 0.f) return;
    void* manager = *(void**)0x7C2478;
    const auto& position = ArsenalField<PhotoVec>(g_arsenal.rayne, 0x38);
    const float radius = g_enhanced.conventionalPickupRadius;
    for (auto& item : g_arsenal.dropped) {
        BYTE* gun = item.gun;
        if (!gun) continue;
        if (ArsenalField<DWORD>(gun, 0x28) != LiveWeaponTag ||
            ArsenalField<BYTE*>(gun, 0x1F4)) {
            item = {};
            continue;
        }
        if (item.waitForExit && GunDistanceSquared(gun, position) > radius * radius)
            item.waitForExit = false;
        if (item.remaining < 0.f) {
            if (!item.waitForExit) item = {};
            continue;
        }
        if (ArsenalField<int>(gun, 0x29C) > 0) {
            item = {};
            continue;
        }
        if (!manager) continue;
        item.remaining = max(0.f, item.remaining - min(delta, .1f));
        if (item.remaining > 0.f) continue;
        item = {};
        // Same world-manager deactivation used by CWeapon's ground update.
        ((void (__thiscall*)(void*, BYTE*, int))0x578EF0)(manager, gun, 0);
    }
}

bool DropArsenalSlot(unsigned slot) {
    auto* gun = SelectedConventionalGun(slot);
    if (!gun) return false;
    DroppedGun* drop = nullptr;
    for (auto& item : g_arsenal.dropped)
        if (!item.gun) { drop = &item; break; }
    if (!drop) return false;
    const bool empty = ArsenalField<int>(gun, 0x29C) <= 0;
    DropConventionalGun(gun);
    if (ArsenalField<BYTE*>(gun, 0x1F4) == g_arsenal.rayne) return false;
    *drop = {gun, empty ? g_enhanced.conventionalDiscardSeconds : -1.f, true};
    g_arsenal.guns[slot - 1] = nullptr;
    for (auto& equipped : g_arsenal.hands)
        if (equipped == slot) equipped = BloodSlot;
    return true;
}

ArsenalInput ReadArsenalInput() {
    ArsenalInput input = {};
    input.wheel = PhotoKey(VK_F7);
    input.equipLeft = PhotoKey(VK_F6);
    input.equipRight = PhotoKey(VK_F9);
    input.previous = PhotoKey(VK_LEFT);
    input.next = PhotoKey(VK_RIGHT);
    using PadState = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
    const auto* settings = *(const int**)0x7C07F8;
    const int selected = settings ? settings[0x200 / 4] : -1;
    for (DWORD slot = 0; slot < 4; ++slot) {
        if (selected >= 0 && selected < 4 && (int)slot != selected) continue;
        XINPUT_STATE state = {};
        if (((PadState)0x6CB192)(slot, &state) != ERROR_SUCCESS) continue;
        const auto& pad = state.Gamepad;
        input.wheel |= pad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
        input.equipRight |= (pad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
        input.equipLeft |= (pad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
        input.previous |= (pad.wButtons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
        input.next |= (pad.wButtons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
        input.horizontal = pad.sThumbRX / 32768.f;
        input.vertical = pad.sThumbRY / 32768.f;
        return input;
    }
    for (int slot = 0; slot < 4; ++slot) {
        if (selected >= 0 && selected < 4 && slot != selected) continue;
        PhotoPad sticks = {};
        if (!ReadPhotoPad(slot, sticks)) continue;
        const DWORD* buttons = (const DWORD*)(0x7E10A0 + slot * 0x1E4 + 0xE0);
        input.wheel |= buttons[7] != 0;
        input.equipRight |= buttons[11] != 0;
        input.equipLeft |= buttons[10] != 0;
        input.previous |= buttons[28] != 0;
        input.next |= buttons[31] != 0;
        input.horizontal = sticks.yaw;
        input.vertical = sticks.pitch;
        break;
    }
    return input;
}

void FilterArsenalWheelActions(BYTE* actions, bool wheelHeld, bool pad = false) {
    auto& fire = ArsenalField<int>(actions, 0x4C);
    if (!wheelHeld && !g_arsenal.wheel && !g_arsenal.fireBlocked) return;
    if (fire == 1) {
        g_arsenal.fireBlocked = true;
        // Leave the mapped state intact for its next held/release update.
        // Both firing paths consult fireBlocked before using it.
    } else if (fire == 2) g_arsenal.fireBlocked = false;
    if (wheelHeld && pad) {
        // Native follow still runs. Only mute the manual yaw/pitch axes
        // in the mapped actions; the wheel reads the raw pad separately.
        ArsenalField<float>(actions, 0x10) = 0.f;
        ArsenalField<float>(actions, 0x18) = 0.f;
    }
    if (g_arsenal.wheel) {
        // No mapped button reaches the character while the wheel is open.
        for (unsigned action = 0; action < 33; ++action)
            ArsenalField<int>(actions, 0x1C + action * 8) = 0;
        return;
    }
    if (!wheelHeld) return;
    constexpr unsigned usedActions[] = {0, 2, 4, 5, 8, 10};
    for (unsigned action : usedActions) {
        auto& state = ArsenalField<int>(actions, 0x1C + action * 8);
        if (state == 1) state = 2;
    }
}

void __fastcall MapArsenalPad(BYTE* game, void*, BYTE* actions, int player) {
    const auto native = (void (__thiscall*)(BYTE*, BYTE*, int))0x4F7E50;
    auto* rayne = *(BYTE**)0x7DCD68;
    if (!ArsenalGameplayInput(game, rayne) || player != 0 || actions != rayne + 0x3A4C) {
        native(game, actions, player);
        return;
    }
    auto* settings = *(BYTE**)0x7C07F8;
    const int slot = settings ? ArsenalField<int>(settings, 0x200) : -1;
    if (slot < 0 || slot >= 4) {
        native(game, actions, player);
        FilterArsenalWheelActions(actions, ReadArsenalInput().wheel, true);
        return;
    }
    using PadState = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
    XINPUT_STATE state = {};
    const bool xbox = ((PadState)0x6CB192)(slot, &state) == ERROR_SUCCESS;
    PhotoPad sony = {};
    if (!xbox && !ReadPhotoPad(slot, sony)) {
        native(game, actions, player);
        FilterArsenalWheelActions(actions, ReadArsenalInput().wheel, true);
        return;
    }
    const int trigger = xbox ? 5 : 7;
    auto* bindings = (int*)(game + 0x138);
    const int savedFire = bindings[6], savedCycle = bindings[11];
    const bool moveFire = savedFire == trigger;
    const bool muteCycle = moveFire || savedCycle == trigger;
    __try {
        if (moveFire) {
            // When R2 was bound to fire, use the freed gun-mode button.
            // A missing or invalid cycle binding falls back to R1.
            bindings[6] = savedCycle >= 0 && savedCycle < 32 && savedCycle != trigger
                ? savedCycle : xbox ? 7 : 5;
        }
        if (muteCycle) bindings[11] = -1;
        native(game, actions, player);
    } __finally {
        bindings[6] = savedFire;
        bindings[11] = savedCycle;
    }
    if (muteCycle) ArsenalField<int>(actions, 0x74) = 0;
    FilterArsenalWheelActions(actions, ReadArsenalInput().wheel, true);
}

void __fastcall MapArsenalKeyboard(BYTE* game, void*, BYTE* actions) {
    ((void (__thiscall*)(BYTE*, BYTE*))0x4F5250)(game, actions);
    auto* rayne = *(BYTE**)0x7DCD68;
    if (ArsenalGameplayInput(game, rayne) && actions == rayne + 0x3A4C)
        FilterArsenalWheelActions(actions, ReadArsenalInput().wheel);
}

void FireConventionalGun(BYTE* gun, unsigned hand) {
    const int mount = HandSlots[hand];
    const unsigned type = ArsenalField<unsigned>(gun, 0x1CC);
    auto& cooldown = g_arsenal.shotCooldown[type][hand];
    if (cooldown > 0.f || ArsenalField<int>(gun, 0x29C) <= 0 ||
        !MountConventionalGun(g_arsenal.rayne, gun, mount)) return;
    // Both hands share the native gun and its ammo, but each has its own cadence.
    auto& nativeCooldown = ArsenalField<float>(gun, 0x2C0);
    const float savedCooldown = nativeCooldown;
    float savedBasis[9];
    const auto savedPosition = ArsenalField<PhotoVec>(gun, 0x38);
    auto* basis = reinterpret_cast<float*>(gun + 0x50);
    memcpy(savedBasis, basis, sizeof savedBasis);
    __try {
        nativeCooldown = cooldown;
        AlignConventionalGrip(gun, g_arsenal.rayne, mount);
        ((int (__thiscall*)(BYTE*, float))0x508460)(gun, 1.f);
    } __finally {
        cooldown = nativeCooldown;
        nativeCooldown = savedCooldown;
        memcpy(basis, savedBasis, sizeof savedBasis);
        ArsenalField<PhotoVec>(gun, 0x38) = savedPosition;
    }
}

bool ArsenalEntryInHand(unsigned entry, unsigned hand) {
    const unsigned slot = g_arsenal.hands[hand];
    if (entry < BloodModes)
        return slot == BloodSlot && entry == CurrentBloodMode(g_arsenal.rayne);
    return slot == entry - BloodModes + 1;
}

// Tapping the equipped conventional type in that hand returns it to blood.
void TapArsenalHand(unsigned hand) {
    const unsigned entry = g_arsenal.preview;
    if (entry < BloodModes) {
        if (!BloodModeAvailable(g_arsenal.rayne, entry)) return;
        SelectBloodMode(entry);
        g_arsenal.hands[hand] = BloodSlot;
        LogArsenalPose(g_arsenal.rayne, TraceArsenalEvent, "wheel-blood");
        return;
    }
    const unsigned slot = entry - BloodModes + 1;
    auto* gun = SelectedConventionalGun(slot);
    if (!gun || ArsenalField<int>(gun, 0x29C) <= 0) return;
    auto& equipped = g_arsenal.hands[hand];
    equipped = equipped == slot ? BloodSlot : slot;
    if (equipped != BloodSlot) g_arsenal.drawn = g_arsenal.aimRequested = true;
    LogArsenalPose(g_arsenal.rayne, TraceArsenalEvent, equipped == BloodSlot ? "wheel-stow" : "wheel-equip");
}

const char* ArsenalEntryName(unsigned entry) {
    static const char* const names[] = {
        "Blood Shot", "Blood Stream", "Blood Spray", "Blood Bomb", "Blood Flame", "Blood Hammer",
        "Pistol", "Sawed Off shotgun", "Uzi"
    };
    static_assert(_countof(names) == WheelEntries, "Complete weapon catalog");
    return ModLocalize(names[entry], true);
}

void DrawArsenalHud(DWORD now) {
    BYTE* font = *(BYTE**)0x7E643C;
    const int width = *(int*)0x7BF140, height = *(int*)0x7BF144;
    if (!font || width <= 0 || height <= 0) return;
    const float base = ArsenalField<float>(font, 0x2494);
    if (!_finite(base) || base <= 0.f) return;
    const float oldScale = ArsenalField<float>(font, 0x2490);
    const int oldHeight = ArsenalField<int>(font, 0x2468), oldSpacing = ArsenalField<int>(font, 0x2470);
    const auto draw = (void (__thiscall*)(void*, int, int, DWORD, DWORD, const char*))0x4E6C60;
    const auto scale = (void (__thiscall*)(BYTE*, float))0x4F9690;
    constexpr DWORD rightColor = 0xFFFFC979, leftColor = 0xFF83CFFF;
    constexpr DWORD bloodColor = 0xFFFFE066;
    __try {
        scale(font, ((g_arsenal.wheel ? 12.f : 16.f) * height / 480.f) / base);
        char label[128];
        if (g_arsenal.wheel) {
            for (unsigned entry = 0; entry < WheelEntries; ++entry) {
                const bool blood = entry < BloodModes;
                auto* gun = blood ? nullptr : SelectedConventionalGun(entry - BloodModes + 1);
                const int rounds = gun ? max(0, ArsenalField<int>(gun, 0x29C)) : 0;
                const bool available = blood ? BloodModeAvailable(g_arsenal.rayne, entry) : rounds > 0;
                const bool cursor = entry == g_arsenal.preview;
                const float angle = entry * (6.2831853f / WheelEntries);
                const int x = (int)(width * (.50f + .37f * sinf(angle)));
                const int y = (int)(height * (.45f - .32f * cosf(angle)));
                if (gun)
                    sprintf_s(label, "%s%s  %d%s", cursor ? "[ " : "", ArsenalEntryName(entry),
                              rounds, cursor ? " ]" : "");
                else
                    sprintf_s(label, "%s%s%s", cursor ? "[ " : "", ArsenalEntryName(entry),
                              cursor ? " ]" : "");
                const DWORD color = !available ? 0xFF898989 : blood ? bloodColor : 0xFFFFFFFF;
                draw(font, x, y, color, 0xFF000000, label);
                const int markerY = y + height * 16 / 480;
                if (ArsenalEntryInHand(entry, 0)) draw(font, x - width / 125, markerY,
                    rightColor, 0xFF000000, ModLocalize("Arsenal Right Hand", false));
                if (ArsenalEntryInHand(entry, 1)) draw(font, x + width / 125, markerY,
                    leftColor, 0xFF000000, ModLocalize("Arsenal Left Hand", false));
            }
            draw(font, width / 2, height * 84 / 100, 0xFFFFFFFF, 0xFF000000,
                 ModLocalize("Arsenal Wheel Controls", false));
            draw(font, width / 2, height * 89 / 100, 0xFFCCCCCC, 0xFF000000,
                 ModLocalize("Arsenal Wheel Keyboard", false));
        } else {
            for (unsigned hand = 0; hand < 2; ++hand) {
                auto* gun = EquippedConventional(hand);
                if (!gun || (hand && gun == EquippedConventional(0))) continue;
                const bool shared = !hand && gun == EquippedConventional(1);
                const int rounds = ArsenalField<int>(gun, 0x29C);
                const auto* name = ArsenalEntryName(BloodModes + ArsenalField<unsigned>(gun, 0x1CC));
                const auto* marker = ModLocalize(hand ? "Arsenal Left Hand" : "Arsenal Right Hand", false);
                if (shared)
                    sprintf_s(label, "%s/%s %s  %d", marker, ModLocalize("Arsenal Left Hand", false),
                              name, rounds);
                else
                    sprintf_s(label, "%s %s  %d", marker, name, rounds);
                const DWORD color = rounds <= g_enhanced.conventionalLowAmmo ?
                    0xFFFF947F : hand ? leftColor : rightColor;
                draw(font, width * 87 / 100, height * (73 + hand * 5) / 100, color, 0xFF000000, label);
            }
            if (g_arsenal.notice && (LONG)(g_arsenal.noticeUntil - now) > 0) {
                const char* notice = ModLocalize(g_arsenal.notice, false);
                if (g_arsenal.noticeRounds) sprintf_s(label, "+%d %s", g_arsenal.noticeRounds, notice);
                else sprintf_s(label, "%s", notice);
                draw(font, width * 65 / 100, height * 83 / 100, rightColor, 0xFF000000, label);
            }
        }
    } __finally {
        ArsenalField<float>(font, 0x2490) = oldScale;
        ArsenalField<int>(font, 0x2468) = oldHeight;
        ArsenalField<int>(font, 0x2470) = oldSpacing;
    }
}

void TickConventionalArsenal(BYTE* game) {
    SyncArsenalFade();
    if (!ArsenalEnabled() ||
        (IsStandaloneChallengeLevel((const char*)G_CURRENT_LEVEL) &&
         (!CurrentChallengeAllowsArsenal() || ActiveChallengeCharacter() != ChallengeRayne))) {
        if (g_arsenal.rayne) ClearConventionalArsenal(true);
        return;
    }
    BYTE* rayne = *(BYTE**)0x7DCD68;
    void* situation = *(void**)G_SITUATION;
    if (rayne != g_arsenal.rayne || situation != g_arsenal.situation)
        ClearConventionalArsenal(false);
    if (!rayne || !situation || ArsenalField<DWORD>(rayne, 0) != 0x7034E4) return;
    g_arsenal.rayne = rayne;
    g_arsenal.situation = situation;
    for (auto& gun : g_arsenal.guns)
        if (gun && ArsenalField<BYTE*>(gun, 0x1F4) != rayne) gun = nullptr;
    for (auto& slot : g_arsenal.hands)
        if (slot != BloodSlot && !SelectedConventionalGun(slot)) slot = BloodSlot;
    if (ArsenalField<float>(rayne, 0x39C8) <= 0.f) {
        ClearConventionalArsenal(true);
        return;
    }
    if (!BloodModeAvailable(rayne, CurrentBloodMode(rayne))) {
        if (g_arsenal.bloodInitialized) ClearConventionalArsenal(true);
        return;
    }
    if (g_arsenalTracePath[0]) {
        const DWORD now = GetTickCount();
        if (!g_arsenal.lastPoseSample || now - g_arsenal.lastPoseSample >= 500) {
            g_arsenal.lastPoseSample = now;
            LogArsenalPose(rayne, TraceArsenalEvent, "pose-sample", game ? ArsenalField<float>(game, 0x3D8) : 0.f);
        }
    }
    if (!ArsenalGameplayInput(game, rayne)) {
        g_arsenal.wheel = g_arsenal.primed = false;
        return;
    }
    const DWORD now = GetTickCount();
    DWORD foreground = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground);
    if (foreground != GetCurrentProcessId()) {
        g_arsenal.wheel = g_arsenal.primed = false;
        return;
    }
    g_arsenal.bloodInitialized = true;
    GrantChallengeStartingGuns();
    const float delta = ArsenalField<float>(game, 0x3D8);
    TickDroppedGuns(delta);
    if (_finite(delta) && delta > 0.f)
        for (auto& pair : g_arsenal.shotCooldown)
            for (auto& cooldown : pair) cooldown = max(0.f, cooldown - delta);
    auto input = ReadArsenalInput();
    input.fire = ArsenalField<int>(rayne, 0x3A98) == 1;
    const auto old = g_arsenal.primed ? g_arsenal.previous : input;
    if (!input.fire) g_arsenal.fireBlocked = false;
    else if (!g_arsenal.primed || input.wheel) g_arsenal.fireBlocked = true;
    g_arsenal.primed = true;
    g_arsenal.previous = input;

    if (input.wheel && !old.wheel) {
        g_arsenal.wheel = true;
        for (auto& started : g_arsenal.holdStart) started = 0;
        auto* right = EquippedConventional(0);
        g_arsenal.preview = right ? BloodModes + ArsenalField<unsigned>(right, 0x1CC) : CurrentBloodMode(rayne);
        ProbeArsenalGuns(rayne); // temp diag: dump on wheel open
    }
    if (g_arsenal.wheel) {
        ArsenalField<int>(rayne, 0x3A88) = 0;
        ArsenalField<int>(rayne, 0x3A90) = 0;
        if (input.wheel) {
            if (input.previous && !old.previous)
                g_arsenal.preview = (g_arsenal.preview + WheelEntries - 1) % WheelEntries;
            if (input.next && !old.next)
                g_arsenal.preview = (g_arsenal.preview + 1) % WheelEntries;
            if (input.horizontal * input.horizontal + input.vertical * input.vertical > .38f) {
                float angle = atan2f(input.horizontal, input.vertical);
                if (angle < 0.f) angle += 6.2831853f;
                g_arsenal.preview = (unsigned)(angle * (WheelEntries / 6.2831853f) + .5f) % WheelEntries;
            }
            for (unsigned hand = 0; hand < 2; ++hand) {
                const bool held = hand ? input.equipLeft : input.equipRight;
                const bool was = hand ? old.equipLeft : old.equipRight;
                if (held && !was) {
                    g_arsenal.holdStart[hand] = now;
                    g_arsenal.holdDropped[hand] = false;
                }
                if (!g_arsenal.holdStart[hand]) continue;
                if (held && !g_arsenal.holdDropped[hand] &&
                    now - g_arsenal.holdStart[hand] >= 450) {
                    if (g_arsenal.preview >= BloodModes)
                        DropArsenalSlot(g_arsenal.preview - BloodModes + 1);
                    g_arsenal.holdDropped[hand] = true;
                }
                if (!held && was) {
                    if (!g_arsenal.holdDropped[hand]) TapArsenalHand(hand);
                    g_arsenal.holdStart[hand] = 0;
                }
            }
        } else g_arsenal.wheel = false;
        if (PhotoKey(VK_F10)) ProbeArsenalGuns(rayne);
        return;
    }

    if (auto* gun = NearestGroundGun(rayne)) {
        const unsigned right = g_arsenal.hands[0], left = g_arsenal.hands[1];
        if (PickUpConventionalGun(gun, now) &&
            (right != g_arsenal.hands[0] || left != g_arsenal.hands[1]))
            g_arsenal.fireBlocked |= input.fire;
    }

    const bool conventional = EquippedConventional(0) || EquippedConventional(1);
    const bool bladeAttack = ArsenalField<int>(rayne, 0x3A88) == 1;
    if (conventional && bladeAttack) {
        if (g_arsenal.drawn || g_arsenal.aimRequested)
            LogArsenalPose(rayne, TraceArsenalEvent, "blade-holster");
        g_arsenal.drawn = g_arsenal.aimRequested = false;
        g_arsenal.fireBlocked |= input.fire;
    }
    const bool firing = conventional && !bladeAttack && input.fire && !g_arsenal.fireBlocked &&
                        (g_arsenal.drawn || !old.fire);
    if (firing) {
        g_arsenal.drawn = true;
        if (ArsenalField<float>(rayne, 0x82B0) <= 0.f) g_arsenal.aimRequested = true;
        if (!old.fire) LogArsenalPose(rayne, TraceArsenalEvent, "conventional-fire-press");
        for (unsigned hand = 0; hand < 2; ++hand) {
            auto* gun = EquippedConventional(hand);
            if (!gun) continue;
            const int type = ArsenalField<int>(gun, 0x1CC);
            const bool automatic = type == 2;
            if (!old.fire || automatic)
                FireConventionalGun(gun, hand);
        }
        // Let both hands try to fire before dropping an empty pair.
        for (auto& slot : g_arsenal.hands) {
            auto* gun = SelectedConventionalGun(slot);
            if (!gun || ArsenalField<int>(gun, 0x29C) > 0) continue;
            DropArsenalSlot(slot);
            slot = BloodSlot;
        }
    }
    if (g_arsenal.drawn)
        for (unsigned hand = 0; hand < 2; ++hand)
            if (auto* gun = EquippedConventional(hand)) MountConventionalGun(rayne, gun, HandSlots[hand]);
}

void SuspendArsenalWheel() {
    g_arsenal.wheel = false;
    g_arsenal.primed = false;
    g_arsenal.holdDropped[0] = g_arsenal.holdDropped[1] = false;
    RestoreArsenalFade();
}

void DrawConventionalArsenalHud(BYTE* game) {
    if (!g_photoBlockingMenu && !g_photoDrawing && !*(int*)G_MENU_STATE &&
        ArsenalManagesBlood(*(BYTE**)0x7DCD68) && g_arsenal.primed &&
        !ArsenalField<int>(game, 0x390))
        DrawArsenalHud(GetTickCount());
}
} // namespace
