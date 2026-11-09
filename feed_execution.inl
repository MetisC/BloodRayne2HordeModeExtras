// Rayne's timeline keeps advancing even when the paired victim is driven by a feed sync path.
struct FeedExecutionCue {
    int state, event, number;
    float frame;
};
constexpr int FeedHeadStomp = 10;
constexpr FeedExecutionCue FeedExecutionCues[] = {
    {0xE9, 0x644, 1, 16},  // front throw
    {0xEB, 0x641, 2, 65},  // synchronized with the victim's head cut
    {0xEC, 0x641, 3, 38},  // stab and release
    {0xED, 0,     4, 60},  // head cut authored on the paired victim clip
    {0xEE, 0,     5, 21},  // head hits the ground on the paired victim clip
    {0xF7, 0x644, 6, 15},  // rear kick
    {0xF8, 0x644, 6, 15},  // alternate rear release clip
    {0xF9, 0,     7, 39},  // final torso split on the paired victim clip
    {0xFA, 0x517, 8, 35},  // second blade extends into the spin
    {0xFB, 0,     9, 36},  // let the native release at frame 32 finish first
    {0xFC, 0x64A, FeedHeadStomp, 38}, // rear head stomp (fatal_headstomp)
};

const FeedExecutionCue* FeedCueForState(int state) {
    for (const auto& cue : FeedExecutionCues)
        if (cue.state == state && (g_enhanced.feedExecutionMask & (1 << (cue.number - 1)))) return &cue;
    return NULL;
}

struct FeedExecutionPulse {
    DWORD at;
    int number;
    LARGE_INTEGER cameraAt;
#ifdef BR2_COMBAT_TEST
    bool tracedWorld, tracedFov;
#endif
};
constexpr int DismemberCinematic = 11, GroundCinematic = 12;
FeedExecutionPulse g_feedExecution;
#ifdef BR2_COMBAT_TEST
int g_tracedFeedState;
bool g_tracedFeedCue;
#endif

void ResetFeedExecution() {
    g_feedExecution = {};
#ifdef BR2_COMBAT_TEST
    g_tracedFeedState = 0;
    g_tracedFeedCue = false;
#endif
}

int NativeCombatPower(const BYTE* rayne) {
    const auto* system = *(const BYTE**)0x7C5928;
    const int global = system ? *(const int*)(system + 0x18) : 0x8000;
    return global == 0x8000 ? *(const int*)(rayne + 0x20C) : global;
}

bool NativeBulletTime(int power) {
    return power >= 0x2001 && power <= 0x2003;
}

bool FeedExecutionActive() {
    if (!g_feedExecution.number) return false;
    const BYTE* rayne = *(BYTE**)RayneActor;
    const bool enabled = g_feedExecution.number == DismemberCinematic ? g_enhanced.dismemberCinematicEnabled :
        g_feedExecution.number == GroundCinematic ? g_enhanced.groundCinematicEnabled : g_enhanced.feedExecutionEnabled;
    if (enabled && rayne && !NativeBulletTime(NativeCombatPower(rayne))) return true;
    LogParry("feed-execution-cancelled", GetTickCount() - g_feedExecution.at, g_feedExecution.number);
    g_feedExecution = {};
    return false;
}

DWORD FeedExecutionSlowHold() {
    return g_feedExecution.number == 8 ? g_enhanced.feedExecutionSpinHoldMs : g_enhanced.feedExecutionSlowHoldMs;
}

DWORD FeedExecutionCameraDuration() {
    return g_enhanced.feedExecutionFovInMs + g_enhanced.feedExecutionFovHoldMs + g_enhanced.feedExecutionFovFadeMs;
}

float FeedExecutionCameraAge() {
    static const double msPerCount = [] {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return 1000.0 / frequency.QuadPart;
    }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (float)((now.QuadPart - g_feedExecution.cameraAt.QuadPart) * msPerCount);
}

float FeedExecutionWorldRate(DWORD now) {
    if (!FeedExecutionActive()) return 1.0f;
    const DWORD age = now - g_feedExecution.at;
    const DWORD hold = FeedExecutionSlowHold(), fade = g_enhanced.feedExecutionSlowFadeMs;
    if (age >= hold + fade && FeedExecutionCameraAge() >= FeedExecutionCameraDuration()) {
        LogParry("feed-execution-end", age, g_feedExecution.number);
        g_feedExecution = {};
        return 1.0f;
    }
    if (age >= hold + fade) return 1.0f;
    const float rate = age < hold ? g_enhanced.feedExecutionSlowRate :
        g_enhanced.feedExecutionSlowRate + (1.0f - g_enhanced.feedExecutionSlowRate) * (age - hold) / fade;
#ifdef BR2_COMBAT_TEST
    if (rate < 1.0f && !g_feedExecution.tracedWorld) {
        g_feedExecution.tracedWorld = true;
        LogParry("feed-execution-world", age, g_feedExecution.number, 0, 0, 0, 0, NULL, NULL, 0, rate);
    }
#endif
    return rate;
}

float FeedExecutionZoomWeight(float age, DWORD in, DWORD hold, DWORD out) {
    const DWORD exitAt = in + hold;
    if (age >= exitAt + out) return 0.0f;
    const float t = age < in ? age / in : age <= exitAt ? 1.0f : 1.0f - (age - exitAt) / out;
    return t * t * (3.0f - 2.0f * t);
}

float FeedExecutionCameraFov(float fov, float& pitch) {
    if (!EnhancedCombatActive() || !FeedExecutionActive()) return fov;
    const float age = FeedExecutionCameraAge();
    const float amount = FeedExecutionZoomWeight(age, g_enhanced.feedExecutionFovInMs,
        g_enhanced.feedExecutionFovHoldMs, g_enhanced.feedExecutionFovFadeMs);
    const float adjusted = fov * (1.0f + (g_enhanced.feedExecutionFov - 1.0f) * amount);
    const float tilt = g_feedExecution.number == FeedHeadStomp ?
        g_enhanced.feedHeadStompTiltDeg * 0.01745329252f * amount : 0.0f;
    pitch += tilt; // Positive pitch points down in the render camera.
#ifdef BR2_COMBAT_TEST
    if ((adjusted != fov || tilt != 0.0f) && !g_feedExecution.tracedFov) {
        g_feedExecution.tracedFov = true;
        LogParry("feed-execution-camera", (DWORD)age, g_feedExecution.number, 0, 0, 0, 0, NULL, NULL, 0, fov, adjusted, amount);
        if (tilt != 0.0f)
            LogParry("feed-head-stomp-framing", (DWORD)age, g_feedExecution.number, 0, 0, 0,
                     0, NULL, NULL, 0, g_enhanced.feedHeadStompTiltDeg, tilt, amount);
    }
#endif
    return adjusted;
}

struct CombatAnimationEvent {
    short frame, type;
    int value;
};

void BeginCombatCinematic(int number) {
    g_feedExecution = {GetTickCount(), number};
    QueryPerformanceCounter(&g_feedExecution.cameraAt);
}

void StartFeedExecution(BYTE* rayne, const FeedExecutionCue& cue, float frame) {
#ifdef BR2_COMBAT_TEST
    g_tracedFeedCue = true;
#endif
    const int power = NativeCombatPower(rayne);
    const auto* victim = *(BYTE**)(rayne + 0x83C0);
    if (NativeBulletTime(power)) {
        LogParry("feed-execution-bullet-time", 0, cue.number, 0, cue.state, 0,
                 0, victim, rayne, cue.event, frame, (float)power);
        return;
    }
    BeginCombatCinematic(cue.number);
    LogParry("feed-execution-start", FeedExecutionSlowHold(), cue.number, 0, cue.state, 0,
             0, victim, rayne, cue.event, frame, g_enhanced.feedExecutionSlowRate,
             g_enhanced.feedExecutionFov, cue.event ? "native-event" : "paired-clip-frame");
}

const CombatAnimationEvent* __fastcall HookCombatAnimationStep(BYTE* controller, void*, float* seconds) {
    const auto advance = (const CombatAnimationEvent* (__thiscall*)(BYTE*, float*))0x607C20;
    if (!EnhancedCombatActive()) return advance(controller, seconds);
    BYTE* rayne = *(BYTE**)RayneActor;
    if (!rayne) return advance(controller, seconds);
    const auto* actor = (BYTE*)((DWORD)controller - 0x158);
    const bool isRayne = actor == rayne;
    const bool hitstopActor = !g_enhanced.hitstopGlobal &&
        (isRayne || actor == g_hitstop.target) && HitstopActive(GetTickCount());
    const bool feedActor = isRayne && g_enhanced.feedExecutionEnabled;
    const float escapeRate = isRayne ? FeedEscapeAnimationRate(rayne) : 1.0f;
    if (!hitstopActor && !feedActor && escapeRate == 1.0f) return advance(controller, seconds);

    const int state = *(const int*)(rayne + 0x17C);
    const auto* cue = feedActor ? FeedCueForState(state) : NULL;
    const unsigned bit = isRayne ? 1 : 2;
    const bool alreadyScaled = (g_hitstop.sampled & bit) != 0;
    const float rate = escapeRate * (hitstopActor && !alreadyScaled ? HitstopRateFor(isRayne) : 1.0f);
    const float incoming = *seconds;
    const float beforeFrame = *(const float*)(controller + 0x28);
    if (rate != 1.0f) *seconds *= rate;
#ifdef BR2_COMBAT_TEST
    if (feedActor && g_tracedFeedState != state) {
        const auto* previous = FeedCueForState(g_tracedFeedState);
        if (previous && !g_tracedFeedCue)
            LogParry("feed-execution-missed", 0, previous->number, 0, g_tracedFeedState, 0, 0,
                     NULL, rayne, previous->event, previous->frame);
        if (cue)
            LogParry("feed-execution-armed", 0, cue->number, 0, state, 0, 0,
                     *(BYTE**)(rayne + 0x83C0), rayne, cue->event, cue->frame);
        g_tracedFeedState = state;
        g_tracedFeedCue = false;
    }
    const int beforeState = *(int*)(controller + 0x24);
#endif
    const auto* event = advance(controller, seconds);
    // Keep the remainder in the caller's units when it loops over events.
    if (rate != 1.0f) *seconds /= rate;
#ifdef BR2_COMBAT_TEST
    if (hitstopActor && !(g_hitstop.tracedPose & bit)) {
        g_hitstop.tracedPose |= bit;
        LogParry(isRayne ? "hitstop-pose-rayne" : "hitstop-pose-target", GetTickCount() - g_hitstop.startAt,
                 0, 0, beforeState, 0, g_hitstop.id, actor, isRayne ? g_hitstop.target : rayne, g_hitstop.group,
                 incoming, beforeFrame, *(float*)(controller + 0x28),
                 alreadyScaled ? "actor-delta" : "animation-delta");
    }
#endif
    if (cue) {
        if (cue->event) {
            if (event && event->type == 0 && event->value == cue->event)
                StartFeedExecution(rayne, *cue, (float)event->frame);
        } else if (*(const int*)(controller + 0x24) == state && beforeFrame < cue->frame &&
                   *(const float*)(controller + 0x28) >= cue->frame) {
            StartFeedExecution(rayne, *cue, cue->frame);
        }
    }
    return event;
}
