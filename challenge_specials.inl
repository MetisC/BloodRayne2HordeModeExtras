// Native sphere damage query, using the same collision and damage records as a kick.
constexpr float KaganWaveRadius = 5.5f;

static struct {
    BYTE* actor;
    DWORD id;
    bool contacted;
} g_ninjaShoulder;

struct ChallengeImpact {
    BYTE* actor;
    bool wave;
    bool contacted;
};

static BYTE __cdecl ChallengeImpactContact(BYTE* query, BYTE* hit, ChallengeImpact* impact) {
    const int count = *(const int*)(query + 0x44);
    if (count <= 0 || (!impact->wave && impact->contacted)) return 0;
    BYTE* records = *(BYTE**)(query + 0x40);
    if (!records) return 0;
    BYTE* target = *(BYTE**)(records + (count - 1) * 0x48);
    BYTE* actor = impact->actor;
    if (!target || target == actor || !CombatCharacter(target) ||
        *(const float*)(target + 0x39C8) <= 0.0f) return 0;

    const auto& origin = *(const ParryVec*)(actor + 0x38);
    const auto& destination = *(const ParryVec*)(target + 0x38);
    const ParryVec forward{*(const float*)(actor + 0x58), 0.0f, *(const float*)(actor + 0x70)};
    const float x = destination.x - origin.x, z = destination.z - origin.z;
    const float distance = sqrtf(x * x + z * z);
    if (impact->wave) {
        if (distance > KaganWaveRadius || fabsf(destination.y - origin.y) > 2.5f) return 0;
    } else if (x * forward.x + z * forward.z < 0.0f) {
        return 0;
    }

    const float dirX = distance > 0.01f ? x / distance : forward.x;
    const float dirZ = distance > 0.01f ? z / distance : forward.z;
    const float damage = impact->wave ? 100.0f - 35.0f * distance / KaganWaveRadius : 65.0f;
    const auto power = (int (__thiscall*)(BYTE*))(*(DWORD**)actor)[0x230 / 4];
    const float fury = power(actor) == 2 ? *(const float*)0x7C73C8 : 1.0f;
    *(float*)(hit + 0x0C) = damage * fury;
    *(ParryVec*)(hit + 0x3C) = {dirX * 3.0f, 0.0f, dirZ * 3.0f};
    *(ParryVec*)(hit + 0x4C) = {dirX, 0.0f, dirZ};
    impact->contacted = true;
    DispatchCinematicDamage(target, hit);
    if (!impact->wave && hit[0x68] && *(const float*)(hit + 0x6C) > 0.0f) {
        const auto impactSound = (void (__thiscall*)(BYTE*, int, int, float))(*(DWORD**)actor)[0x238 / 4];
        impactSound(actor, 20, *(const int*)(hit + 0x5C), 1.0f);
    }
    ConfirmOutgoingHit(actor, target, hit);
    // We already sent this packet to the victim. The query can visit more actors.
    return 0;
}

static bool ChallengeImpactSphere(BYTE* actor, const ParryVec& position, float radius, bool wave) {
    void* manager = *(void**)0x7C5D50;
    const DWORD id = *(const DWORD*)(actor + 0x82F0);
    if (!manager || !id) return false;

    alignas(16) BYTE query[0x43C]{};
    alignas(16) BYTE hit[0x88]{};
    ((void (__thiscall*)(BYTE*))0x5F7210)(query);
    ((void (__thiscall*)(BYTE*, const ParryVec*, float))0x5EF9C0)(query, &position, radius);
    *(int*)(query + 0x18) = 2;
    *(BYTE**)(query + 0x1C) = actor;
    ((void (__thiscall*)(BYTE*, BYTE*))0x5ECC90)(query, actor);

    ((void (__thiscall*)(BYTE*))0x405790)(hit);
    *(int*)hit = DamageKick;
    *(DWORD*)(hit + 4) = id;
    *(int*)(hit + 8) = 4;
    *(float*)(hit + 0x38) = 1.0f;
    *(BYTE**)(hit + 0x20) = actor;
    *(BYTE**)(hit + 0x24) = actor;
    ChallengeImpact impact{actor, wave, false};
    using Callback = BYTE (__cdecl*)(BYTE*, BYTE*, ChallengeImpact*);
    ((int (__thiscall*)(void*, BYTE*, BYTE*, Callback, ChallengeImpact*))0x5E8020)(
        manager, query, hit, ChallengeImpactContact, &impact);
    ((void (__thiscall*)(BYTE*))0x5F6A20)(query);
    return impact.contacted;
}

static void TryNinjaShoulder(BYTE* actor) {
    const DWORD id = *(const DWORD*)(actor + 0x82F0);
    if (g_ninjaShoulder.actor != actor || g_ninjaShoulder.id != id)
        g_ninjaShoulder = {actor, id, false};
    if (g_ninjaShoulder.contacted) return;

    BYTE* controller = actor + 0x158;
    if (!((BYTE (__thiscall*)(BYTE*, const char*))0x6061B0)(controller, "shoulderhit")) return;
    void* skeleton = ((void* (__thiscall*)(BYTE*))0x605FF0)(controller);
    if (!skeleton) return;
    const int bone = ((int (__thiscall*)(void*, const char*, int))0x605000)(
        skeleton, "Bip01 R UpperArm", 0);
    if (bone < 0) return;
    ParryVec local{}, shoulder{};
    ((void* (__thiscall*)(BYTE*, ParryVec*, int))0x608410)(controller, &local, bone);
    ((void (__thiscall*)(BYTE*, ParryVec*, const ParryVec*))0x402D70)(actor, &shoulder, &local);
    shoulder.x += *(const float*)(actor + 0x58) * 0.45f;
    shoulder.z += *(const float*)(actor + 0x70) * 0.45f;
    g_ninjaShoulder.contacted = ChallengeImpactSphere(actor, shoulder, 1.35f, false);
}

static void PlayKaganWave(BYTE* controller) {
    if (ActiveChallengeCharacter() != ChallengeKagan) return;
    BYTE* actor = *(BYTE**)RayneActor;
    if (!actor || controller != actor + 0x158 ||
        *(const float*)(actor + 0x39C8) <= 0.0f || *(int*)G_MENU_STATE || g_photoBlockingMenu) return;
    const int state = *(const int*)(actor + 0x17C);
    if (state != 46 && state != 453) return;
    auto position = *(const ParryVec*)(actor + 0x38);
    SpawnCombatFx("ground_dust_cloud.fx", position, {0.0f, 1.0f, 0.0f}, 1.8f, 420);
    position.y += 0.9f;
    ChallengeImpactSphere(actor, position, KaganWaveRadius, true);
}
