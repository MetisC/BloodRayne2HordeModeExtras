// Presentation and optional follow-ups on top of confirmed native combat events.
enum StyleAction { StyleBlade, StyleKick, StyleGun, StyleHarpoon, StyleParry,
                   StyleRiposte, StyleFinisher, StyleDodge, StyleActionCount };
struct StyleStreak {
    int points, flashRank, variety;
    DWORD at, flashAt, seen;
    DWORD last[StyleActionCount];
    bool moves[0x45], finishers[0x75];
};
StyleStreak g_style;

struct ShowcasePulse {
    DWORD at;
    bool fov, slow, started;
#ifdef BR2_COMBAT_TEST
    DWORD id, group;
    const BYTE* target;
    bool tracedSlow, tracedFov;
#endif
};
ShowcasePulse g_showcasePulse;
const BYTE* g_riposteTarget;
DWORD g_riposteAt;

struct DodgeOffset {
    int lastMove, savedMove;
    DWORD lastAt, startedAt, pressedAt;
    unsigned button;
    bool evading;
};
DodgeOffset g_dodgeOffset;

void ResetShowcase() {
    g_style = {};
    g_showcasePulse = {};
    g_riposteTarget = NULL;
    g_dodgeOffset = {};
}

int StyleRank(int points) {
    return (points >= g_enhanced.styleRankC) + (points >= g_enhanced.styleRankB) +
           (points >= g_enhanced.styleRankA) + (points >= g_enhanced.styleRankS) +
           (points >= g_enhanced.styleRankSS) + (points >= g_enhanced.styleRankSSS) +
           (points >= g_enhanced.styleRankV);
}

const char* StyleGrade(int points) {
    static const char* const grades[] = {"D", "C", "B", "A", "S", "SS", "SSS", "V"};
    return grades[StyleRank(points)];
}

void AddStyle(BYTE* rayne, StyleAction action, int points, DWORD now, int variant = 0) {
    if (!g_enhanced.styleEnabled || !points) return;
    if (now - g_style.at >= (DWORD)g_enhanced.styleWindowMs) g_style = {};
    const int previousPoints = g_style.points;
    const DWORD bit = 1u << action;
    const bool seenAction = (g_style.seen & bit) != 0;
    const bool finisher = action == StyleFinisher;
    bool* seenVariant = nullptr;
    if (finisher && variant > 0 && variant < (int)_countof(g_style.finishers))
        seenVariant = &g_style.finishers[variant];
    else if ((action == StyleBlade || action == StyleKick) && variant > 0 && variant < (int)_countof(g_style.moves))
        seenVariant = &g_style.moves[variant];
    const bool fresh = seenVariant ? !*seenVariant : !seenAction;
    const bool repeated = !fresh && (seenVariant ||
        now - g_style.last[action] < (DWORD)g_enhanced.styleRepeatMs);
    int percent = 100;
    if (repeated) {
        percent = finisher ? g_enhanced.styleFinisherRepeatPercent : g_enhanced.styleRepeatPercent;
    } else if (fresh && !finisher) {
        percent += min(g_enhanced.styleVarietyMaxPercent, g_style.variety * g_enhanced.styleVarietyStepPercent);
        ++g_style.variety;
    }
    if (seenVariant) *seenVariant = true;
    g_style.seen |= bit;
    g_style.last[action] = now;
    points = points * percent / 100;
    if (!points) return;
    g_style.at = now;
    g_style.points = min(g_enhanced.styleMaxPoints, g_style.points + points);
    if (g_runArmed && !g_runDone && g_runBars >= 0 && !g_runCombatChanged &&
        g_runCombat == CombatEnhanced && g_runSituation == *(void**)G_SITUATION)
        g_runStylePeak = max(g_runStylePeak, g_style.points);
    const int rank = StyleRank(g_style.points);
    LogParry("style-awarded", 0, action, rank, points, 0, 0, NULL, NULL, 0,
             (float)previousPoints, (float)g_style.points, (float)variant,
             repeated ? "repeat" : percent > 100 ? "variety" : "base");
    if (rank > StyleRank(previousPoints)) {
        const bool play = g_style.flashRank == 0 || g_style.flashAt != now;
        g_style.flashAt = now;
        g_style.flashRank = rank;
        if (play && g_enhanced.styleRankSound) CombatSound(rayne, "imp_blade_ME_1.wav");
        LogParry("style-rank", 0, action, rank, g_style.points);
    }
}

void MarkShowcaseParry(BYTE* rayne, const BYTE* attacker, DWORD now) {
    if (g_enhanced.riposteEnabled) {
        g_riposteAt = now;
        g_riposteTarget = attacker;
    }
    AddStyle(rayne, StyleParry, g_enhanced.styleParry, now);
}

float ShowcaseWorldRate(DWORD now) {
    if (!g_showcasePulse.slow || !g_enhanced.finisherEnabled || !g_enhanced.finisherKillSlow)
        return 1.0f;
    const DWORD age = now - g_showcasePulse.at;
    const DWORD hold = g_enhanced.finisherKillHoldMs, fade = g_enhanced.finisherKillFadeMs;
    if (age >= hold + fade) {
        g_showcasePulse.slow = false;
        LogParry("finisher-slow-end", age, 0, 0, 0, 0, g_showcasePulse.id, g_showcasePulse.target,
                 NULL, g_showcasePulse.group);
        return 1.0f;
    }
#ifdef BR2_COMBAT_TEST
    if (!g_showcasePulse.tracedSlow) {
        g_showcasePulse.tracedSlow = true;
        LogParry("finisher-slow-start", age, 0, 0, 0, 0, g_showcasePulse.id, g_showcasePulse.target,
                 NULL, g_showcasePulse.group, g_enhanced.finisherKillRate, (float)hold, (float)fade);
    }
#endif
    if (age <= hold) return g_enhanced.finisherKillRate;
    return g_enhanced.finisherKillRate + (1.0f - g_enhanced.finisherKillRate) * (age - hold) / fade;
}

float ShowcaseCameraFov(float fov) {
    if (!EnhancedCombatActive() || !g_showcasePulse.fov || !g_enhanced.finisherEnabled ||
        !g_enhanced.finisherFovMs) return fov;
    const DWORD age = GetTickCount() - g_showcasePulse.at;
    if (age >= (DWORD)g_enhanced.finisherFovMs) {
        g_showcasePulse.fov = false;
        LogParry("finisher-fov-end", age, 0, 0, 0, 0, g_showcasePulse.id, g_showcasePulse.target,
                 NULL, g_showcasePulse.group);
        return fov;
    }
    const float t = float(age) / g_enhanced.finisherFovMs;
    const float amount = min(t * 5.0f, (1.0f - t) * 1.25f);
#ifdef BR2_COMBAT_TEST
    if (amount > 0.0f && !g_showcasePulse.tracedFov) {
        g_showcasePulse.tracedFov = true;
        LogParry("finisher-fov-start", age, 0, 0, 0, 0, g_showcasePulse.id, g_showcasePulse.target,
                 NULL, g_showcasePulse.group, fov, g_enhanced.finisherFov, (float)g_enhanced.finisherFovMs);
    }
#endif
    return fov * (1.0f - (1.0f - g_enhanced.finisherFov) * amount);
}

bool ShowcaseFinisherState(int state) {
    switch (state) {
    case 0x1E: case 0x23: case 0x28: case 0x2D: // native three-hit endings
    case 0x47: case 0x4C: case 0x51: case 0x56: // mirrored variants
    case 0x5B: case 0x60: case 0x65: case 0x6A: case 0x6F: case 0x74:
        return true;
    case 0x2E: case 0x2F:
        return g_enhanced.finisherCircle != 0;
    default:
        return false;
    }
}

bool DodgeAnimation(int state) {
    switch (state) {
    case 0x18D: case 0x190: case 0x193: case 0x19A:
    case 0x1B1: case 0x1B4: case 0x1B7: case 0x1BC:
        return true; // stock base directional evades, including mirrors
    default:
        return false;
    }
}

bool ResumeStance(int state) {
    return state == 0 || state == 0x14 || state == 0x17 || state == 0x180 ||
           state == 0x181 || state == 0x1AA || (state >= 0x1A5 && state <= 0x1A8);
}

bool OffsetSource(int state, int move) {
    if (move < 9 || move > 0x3C) return false;
    return ((state >= 0x1A && state <= 0x2D) || (state >= 0x43 && state <= 0x74)) &&
           !ShowcaseFinisherState(state);
}

void TickShowcase(BYTE* rayne, DWORD now) {
    if (!g_enhanced.finisherEnabled) g_showcasePulse = {};
    if (!g_enhanced.riposteEnabled || now - g_riposteAt > (DWORD)g_enhanced.riposteWindowMs)
        g_riposteTarget = NULL;
    if (g_style.points && (!g_enhanced.styleEnabled ||
        now - g_style.at >= (DWORD)g_enhanced.styleWindowMs)) g_style = {};

    if (!g_enhanced.dodgeOffsetEnabled || *(void**)(rayne + 0x83C0) ||
        *(void**)(rayne + 0x8374)) {
        g_dodgeOffset = {};
        return;
    }
    const int state = *(int*)(rayne + 0x17C);
    const bool evading = DodgeAnimation(state);
    if (evading && !g_dodgeOffset.evading) {
        g_dodgeOffset.savedMove = now - g_dodgeOffset.lastAt <=
            (DWORD)g_enhanced.dodgeOffsetSourceMs ? g_dodgeOffset.lastMove : 0;
        g_dodgeOffset.startedAt = now;
        LogParry("dodge-enter", now - g_dodgeOffset.lastAt, 0, 0, state, 0, 0,
                 rayne, NULL, 0, (float)g_dodgeOffset.savedMove);
        g_dodgeOffset.button = 0;
    }
    g_dodgeOffset.evading = evading;
    if (!g_dodgeOffset.savedMove) return;
    if (now - g_dodgeOffset.startedAt >= (DWORD)g_enhanced.dodgeOffsetWindowMs ||
        (!evading && !ResumeStance(state))) {
        g_dodgeOffset.savedMove = 0;
        g_dodgeOffset.button = 0;
        return;
    }

    if (evading && g_enhanced.dodgeOffsetBufferMs) {
        for (unsigned i = 0; i < _countof(AttackButtons); ++i) {
            if (*(int*)(rayne + AttackButtons[i]) != 1 || g_harpoonHeld[i]) continue;
            g_dodgeOffset.button = AttackButtons[i];
            g_dodgeOffset.pressedAt = now;
            LogParry("dodge-buffer-armed", 0, 0, 0, AttackButtons[i], 0, 0, rayne);
        }
    } else if (g_dodgeOffset.button) {
        if (g_enhanced.dodgeOffsetBufferMs &&
            now - g_dodgeOffset.pressedAt < (DWORD)g_enhanced.dodgeOffsetBufferMs &&
            !*(int*)(rayne + AttackButtons[0]) && !*(int*)(rayne + AttackButtons[1]))
            *(int*)(rayne + g_dodgeOffset.button) = 1;
        g_dodgeOffset.button = 0;
    }
}

int ChooseOffsetMove(BYTE* rayne, DWORD selector, bool enabled) {
    const auto choose = (int (__thiscall*)(BYTE*))selector;
    if (!EnhancedCombatActive() || !g_enhanced.dodgeOffsetEnabled || !enabled ||
        rayne != *(BYTE**)RayneActor || !g_dodgeOffset.savedMove ||
        GetTickCount() - g_dodgeOffset.startedAt >= (DWORD)g_enhanced.dodgeOffsetWindowMs ||
        !ResumeStance(*(int*)(rayne + 0x17C)) || *(int*)(rayne + 0x8354) ||
        *(int*)(rayne + 0x3A70) != 1) return choose(rayne);

    // Temporarily present the previous step to the stock chooser. It still
    // enforces move unlocks, the selected attack and the actual input state.
    int& current = *(int*)(rayne + 0x8350);
    const int savedCurrent = current;
    current = g_dodgeOffset.savedMove;
    const int next = choose(rayne);
    current = savedCurrent;
    const int source = g_dodgeOffset.savedMove;
    g_dodgeOffset.savedMove = 0;
    if (next <= 0 || next > 0x44) return choose(rayne);
    AddStyle(rayne, StyleDodge, g_enhanced.styleDodge, GetTickCount());
    LogParry("dodge-offset", GetTickCount() - g_dodgeOffset.startedAt, 0, 0, next, 0, 0,
             rayne, NULL, 0, (float)source);
    return next;
}

int __fastcall HookDodgeBlade(BYTE* rayne, void*) {
    return ChooseOffsetMove(rayne, 0x54C3D0, g_enhanced.dodgeOffsetBlade != 0);
}
int __fastcall HookDodgeKick(BYTE* rayne, void*) {
    return ChooseOffsetMove(rayne, 0x54C730, g_enhanced.dodgeOffsetKick != 0);
}

bool ShowcaseConfirmedHit(BYTE* rayne, BYTE* victim, const BYTE* hit, int type, int severity,
                           DWORD now, DWORD group, bool firstTarget, bool special) {
    const DWORD id = *(const DWORD*)(hit + 4);
    if (!IsMeleeImpact(type)) {
        if (!firstTarget) return false;
        if (type == DamageHarpoon) {
            if (g_harpoonTarget != victim) AddStyle(rayne, StyleHarpoon, g_enhanced.styleHarpoon, now);
        } else {
            AddStyle(rayne, StyleGun, g_enhanced.styleGun, now);
        }
        return false;
    }

    const bool kick = type == DamageKick;
    const int state = *(int*)(rayne + 0x17C), move = *(int*)(rayne + 0x8350);
    if (firstTarget) {
        AddStyle(rayne, kick ? StyleKick : StyleBlade, kick ? g_enhanced.styleKick : g_enhanced.styleBlade, now, move);
        if (OffsetSource(state, move)) {
            g_dodgeOffset.lastMove = move;
            g_dodgeOffset.lastAt = now;
        } else if (!DodgeAnimation(state)) {
            g_dodgeOffset.lastMove = 0;
        }
    }

    const bool riposte = g_enhanced.riposteEnabled && g_enhanced.parryEnabled &&
        g_riposteTarget == victim && now - g_riposteAt <= (DWORD)g_enhanced.riposteWindowMs;
    if (riposte) {
        g_riposteTarget = NULL;
        AddStyle(rayne, StyleRiposte, g_enhanced.styleRiposte, now);
        LogParry("riposte", now - g_riposteAt, type, severity, state, 0, id, victim, rayne, group);
    }
    if (special) {
        if (riposte) LogParry("riposte-feedback-coalesced", 0, type, severity, state, 0, id, victim, rayne, group);
        return false;
    }

    const bool finisher = g_enhanced.finisherEnabled && ShowcaseFinisherState(state) &&
        (!g_showcasePulse.started ||
         now - g_showcasePulse.at >= (DWORD)g_enhanced.finisherCooldownMs);
    if (!finisher && !riposte) return false;

    const ParryVec normal = {*(float*)(rayne + 0x58), 0.0f, *(float*)(rayne + 0x70)};
    ScheduleHitstop(victim, finisher ? g_enhanced.finisherHitstopMs : g_enhanced.riposteHitstopMs,
                    now, id, group, HitstopSpecial);
    if (finisher) {
        const bool killed = *(const float*)(victim + 0x39C8) <= 0.0f;
        g_showcasePulse = {now, true, killed && g_enhanced.finisherKillSlow != 0, true};
#ifdef BR2_COMBAT_TEST
        g_showcasePulse.id = id;
        g_showcasePulse.group = group;
        g_showcasePulse.target = victim;
#endif
        if (g_enhanced.finisherFx) {
            const auto point = CombatImpactPoint(victim, hit);
            if (!SpawnCombatFx(kick ? "impact_blunt.fx" : "blood_arc.fx", point, normal,
                               g_enhanced.finisherFxScale, g_enhanced.finisherFxMs))
                SpawnCombatFx("impact.fx", point, normal, g_enhanced.finisherFxScale, g_enhanced.finisherFxMs);
        }
        if (g_enhanced.finisherSound)
            CombatSound(rayne, kick ? "imp_kick_CO_1.wav" : "imp_blade_ME_2.wav");
        CombatShake(g_enhanced.finisherShake, g_enhanced.finisherShakeMs);
        const int canonicalState = state >= 0x47 && state <= 0x56 ? state - 0x29 : state;
        AddStyle(rayne, StyleFinisher, g_enhanced.styleFinisher, now, canonicalState);
        LogParry(killed ? "finisher-kill" : "finisher", 0, type, severity, state, 0, id, victim, rayne, group);
    } else {
        if (g_enhanced.riposteFx) {
            const auto point = CombatImpactPoint(victim, hit);
            if (!SpawnCombatFx(kick ? "impact_blunt.fx" : "blood_arc.fx", point, normal,
                               g_enhanced.riposteFxScale, g_enhanced.riposteFxMs))
                SpawnCombatFx("impact.fx", point, normal, g_enhanced.riposteFxScale, g_enhanced.riposteFxMs);
        }
        if (g_enhanced.riposteSound)
            CombatSound(rayne, kick ? "imp_kick_CO_2.wav" : "imp_blade_ME_3.wav");
        CombatShake(g_enhanced.riposteShake, g_enhanced.riposteShakeMs);
    }
    return true;
}

void DrawStyleRank(void* font, int width, int height, DWORD now) {
    if (!g_enhanced.styleEnabled || !g_style.points ||
        now - g_style.at >= (DWORD)g_enhanced.styleWindowMs) return;
    const int rank = StyleRank(g_style.points);
    const char* grade = StyleGrade(g_style.points);
    const DWORD colors[] = {0xFFDBDBDB, 0xFFAEDAFF, 0xFFFFA858, 0xFFFF745C, 0xFFFFDE78,
                            0xFFFFAE38, 0xFFFF648A, 0xFFE8A6FF};
    DWORD color = colors[rank];
    const DWORD elapsed = now - g_style.flashAt;
    if (g_style.flashRank == rank && elapsed < (DWORD)g_enhanced.styleFlashMs) {
        const unsigned amount = 255 * (g_enhanced.styleFlashMs - elapsed) / g_enhanced.styleFlashMs;
        color = 0xFF000000;
        for (unsigned shift = 0; shift < 24; shift += 8) {
            const unsigned c = (colors[rank] >> shift) & 255;
            color |= (c + ((255 - c) * amount / 255)) << shift;
        }
    }
    char label[32];
    if (g_enhanced.styleShowPoints) sprintf_s(label, "%s  %d", grade, g_style.points);
    else sprintf_s(label, "%s", grade);
    ((void (__thiscall*)(void*, int, int, DWORD, DWORD, const char*))0x4E6C60)
        (font, (int)(width * g_enhanced.styleX), (int)(height * g_enhanced.styleY),
         color, 0xFF000000, label);
}
