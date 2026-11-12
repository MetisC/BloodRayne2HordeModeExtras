// The worker reads the INI. The game thread uses one consistent snapshot per frame.
#define ENHANCED_SETTINGS(X) \
    X(int, parryEnabled, 1, 0, 1) \
    X(int, parryWindowMs, 200, 0, 1500) \
    X(int, parryGuardMs, 250, 0, 2000) \
    X(int, feedEscapeWindowMs, 300, 0, 1000) \
    X(float, feedEscapeRollRate, 1.0f, 1.0f, 3.0f) \
    X(int, feedEscapeKeepFacing, 1, 0, 1) \
    X(float, feedEscapeTravelDistance, 3.5f, 0.0f, 8.0f) \
    X(int, feedEscapeTravelMs, 180, 50, 600) \
    X(int, parryMaxInputMs, 1200, 1, 5000) \
    X(int, parryCooldownMs, 240, 0, 5000) \
    X(int, parryRepeatMs, 1400, 0, 5000) \
    X(float, parryClockFloor, 0.25f, 0.01f, 1.0f) \
    X(int, parrySlowHoldMs, 300, 0, 3000) \
    X(int, parrySlowFadeMs, 420, 0, 3000) \
    X(float, parryWorldRate, 0.40f, 0.01f, 1.0f) \
    X(float, parryAttackerRate, 0.20f, 0.01f, 1.0f) \
    X(int, parryMinSeverity, 1, 1, 5) \
    X(int, parryMaxSeverity, 4, 1, 5) \
    X(int, parrySound, 1, 0, 1) \
    X(int, parrySpark, 1, 0, 1) \
    X(float, parrySparkScale, 2.0f, 0.1f, 8.0f) \
    X(int, parrySparkMs, 1000, 1, 5000) \
    X(int, parryFallbackSparkMs, 250, 1, 5000) \
    X(float, parryShake, 4000.0f, 0.0f, 12000.0f) \
    X(int, parryShakeMs, 250, 0, 2000) \
    X(int, hitstopEnabled, 1, 0, 1) \
    X(int, hitstopGlobal, 1, 0, 1) \
    X(float, hitstopWorldRate, 0.015f, 0.01f, 1.0f) \
    X(int, bladeHitstopMs, 115, 0, 400) \
    X(int, kickHitstopMs, 145, 0, 400) \
    X(int, heavyHitstopMs, 175, 0, 400) \
    X(float, ephemeraHitstopScale, 0.50f, 0.0f, 1.0f) \
    X(int, heavyMinSeverity, 4, 1, 5) \
    X(int, hitstopGapMs, 105, 0, 1000) \
    X(float, hitstopRayneRate, 0.015f, 0.01f, 1.0f) \
    X(float, hitstopTargetRate, 0.01f, 0.01f, 1.0f) \
    X(int, receivedHitstopEnabled, 1, 0, 1) \
    X(int, receivedLightHitstopMs, 20, 0, 100) \
    X(int, receivedHitstopMs, 45, 0, 100) \
    X(float, receivedHitstopWorldRate, 0.05f, 0.01f, 1.0f) \
    X(int, receivedHitstopMinSeverity, 4, 1, 5) \
    X(float, receivedHitstopRayneRate, 0.10f, 0.01f, 1.0f) \
    X(float, receivedHitstopAttackerRate, 0.05f, 0.01f, 1.0f) \
    X(int, bladeSound, 1, 0, 1) \
    X(int, kickSound, 1, 0, 1) \
    X(float, bladeShake, 0.0f, 0.0f, 12000.0f) \
    X(float, kickShake, 800.0f, 0.0f, 12000.0f) \
    X(float, heavyShake, 1700.0f, 0.0f, 12000.0f) \
    X(int, meleeShakeMs, 100, 0, 2000) \
    X(int, rangedFeedback, 1, 0, 1) \
    X(int, rangedGapMs, 180, 0, 2000) \
    X(int, rangedHeavyHitstopMs, 38, 0, 300) \
    X(float, rangedLightShake, 300.0f, 0.0f, 12000.0f) \
    X(float, rangedHeavyShake, 1300.0f, 0.0f, 12000.0f) \
    X(int, rangedShakeMs, 120, 0, 2000) \
    X(int, harpoonFeedback, 1, 0, 1) \
    X(int, harpoonSound, 1, 0, 1) \
    X(float, harpoonShake, 1000.0f, 0.0f, 12000.0f) \
    X(int, harpoonShakeMs, 120, 0, 2000) \
    X(int, harpoonComboBufferMs, 230, 0, 1000) \
    X(int, recoveryMs, 300, 0, 2000) \
    X(int, recoveryMelee, 1, 0, 1) \
    X(int, recoveryBullets, 1, 0, 1) \
    X(int, recoveryMaxSeverity, 4, 1, 5) \
    X(int, twistedWindInvulnerabilityEnabled, 1, 0, 1) \
    X(int, twistedWindInvulnerabilityMs, 350, 0, 700) \
    X(int, comboEnabled, 1, 0, 1) \
    X(int, comboWindowMs, 2300, 100, 20000) \
    X(int, comboMinHits, 2, 1, 99) \
    X(int, comboRanged, 1, 0, 1) \
    X(int, comboGroupScatter, 1, 0, 1) \
    X(int, comboHarpoonDamage, 1, 0, 1) \
    X(int, comboBreakOnDamage, 1, 0, 1) \
    X(float, comboX, 0.95f, 0.15f, 0.99f) \
    X(float, comboY, 0.40f, 0.10f, 0.85f) \
    X(int, recoveryEnabled, 1, 0, 1) \
    X(int, harpoonBufferEnabled, 1, 0, 1) \
    X(int, finisherEnabled, 1, 0, 1) \
    X(int, finisherCircle, 1, 0, 1) \
    X(int, finisherFx, 1, 0, 1) \
    X(float, finisherFxScale, 2.0f, 0.1f, 8.0f) \
    X(int, finisherFxMs, 480, 20, 2500) \
    X(int, finisherSound, 1, 0, 1) \
    X(float, finisherShake, 2300.0f, 0.0f, 12000.0f) \
    X(int, finisherHitstopMs, 235, 0, 500) \
    X(int, finisherShakeMs, 180, 0, 2000) \
    X(float, finisherFov, 0.94f, 0.7f, 1.0f) \
    X(int, finisherFovMs, 220, 0, 1200) \
    X(int, finisherKillSlow, 1, 0, 1) \
    X(float, finisherKillRate, 0.48f, 0.1f, 1.0f) \
    X(int, finisherKillHoldMs, 100, 0, 1200) \
    X(int, finisherKillFadeMs, 200, 0, 1200) \
    X(int, finisherCooldownMs, 400, 0, 5000) \
    X(int, riposteEnabled, 1, 0, 1) \
    X(int, riposteWindowMs, 1250, 50, 5000) \
    X(int, riposteFx, 1, 0, 1) \
    X(float, riposteFxScale, 1.65f, 0.1f, 8.0f) \
    X(int, riposteFxMs, 400, 20, 2500) \
    X(int, riposteSound, 1, 0, 1) \
    X(float, riposteShake, 2000.0f, 0.0f, 12000.0f) \
    X(int, riposteShakeMs, 145, 0, 2000) \
    X(int, riposteHitstopMs, 210, 0, 500) \
    X(int, feedExecutionEnabled, 1, 0, 1) \
    X(int, feedExecutionMask, 1023, 0, 1023) \
    X(float, feedExecutionSlowRate, 0.18f, 0.05f, 1.0f) \
    X(int, feedExecutionSlowHoldMs, 1000, 0, 3000) \
    X(int, feedExecutionSpinHoldMs, 1100, 0, 3000) \
    X(int, feedExecutionSlowFadeMs, 250, 0, 1500) \
    X(float, feedExecutionFov, 1.67f, 1.0f, 2.5f) \
    X(int, feedExecutionFovInMs, 200, 0, 1500) \
    X(int, feedExecutionFovHoldMs, 1000, 0, 3000) \
    X(int, feedExecutionFovFadeMs, 350, 0, 1500) \
    X(float, feedHeadStompTiltDeg, 4.0f, 0.0f, 8.0f) \
    X(int, dismemberCinematicEnabled, 1, 0, 1) \
    X(int, groundCinematicEnabled, 1, 0, 1) \
    X(int, combatCinematicGapMs, 1500, 0, 6000) \
    X(int, dodgeOffsetEnabled, 1, 0, 1) \
    X(int, dodgeOffsetSourceMs, 600, 50, 3000) \
    X(int, dodgeOffsetResumeMs, 300, 50, 4000) \
    X(int, dodgeOffsetBufferEnabled, 1, 0, 1) \
    X(int, dodgeOffsetBlade, 1, 0, 1) \
    X(int, dodgeOffsetKick, 1, 0, 1) \
    X(int, styleEnabled, 1, 0, 1) \
    X(int, styleWindowMs, 3200, 100, 15000) \
    X(int, styleRepeatMs, 1800, 0, 12000) \
    X(int, styleRepeatPercent, 20, 0, 100) \
    X(int, styleFinisherRepeatPercent, 20, 0, 100) \
    X(int, styleVarietyStepPercent, 15, 0, 100) \
    X(int, styleVarietyMaxPercent, 60, 0, 300) \
    X(int, styleMaxPoints, 999999, 1, 999999) \
    X(int, styleBlade, 70, 0, 2000) \
    X(int, styleKick, 100, 0, 2000) \
    X(int, styleGun, 55, 0, 2000) \
    X(int, styleHarpoon, 120, 0, 2000) \
    X(int, styleParry, 240, 0, 2000) \
    X(int, styleRiposte, 320, 0, 2000) \
    X(int, styleFinisher, 50, 0, 2000) \
    X(int, styleDodge, 150, 0, 2000) \
    X(int, styleRankC, 220, 1, 50000) \
    X(int, styleRankB, 500, 1, 50000) \
    X(int, styleRankA, 900, 1, 50000) \
    X(int, styleRankS, 1450, 1, 50000) \
    X(int, styleRankSS, 2600, 1, 50000) \
    X(int, styleRankSSS, 4500, 1, 50000) \
    X(int, styleRankV, 8000, 1, 50000) \
    X(int, styleBreakOnDamage, 1, 0, 1) \
    X(int, styleShowPoints, 1, 0, 1) \
    X(int, styleRankSound, 1, 0, 1) \
    X(int, styleFlashMs, 230, 0, 1500) \
    X(float, styleX, 0.95f, 0.15f, 0.99f) \
    X(float, styleY, 0.46f, 0.10f, 0.90f) \
    X(int, bloodShotAmmoPoolL1, 270, 1, 1000) \
    X(int, bloodShotAmmoPoolL2, 300, 1, 1000) \
    X(int, bloodShotAmmoPoolL3, 330, 1, 1000) \
    X(int, bloodStreamAmmoPoolL1, 135, 1, 1000) \
    X(int, bloodStreamAmmoPoolL2, 180, 1, 1000) \
    X(int, bloodStreamAmmoPoolL3, 225, 1, 1000) \
    X(int, bloodSprayAmmoPoolL1, 45, 1, 1000) \
    X(int, bloodSprayAmmoPoolL2, 53, 1, 1000) \
    X(int, bloodSprayAmmoPoolL3, 60, 1, 1000) \
    X(int, bloodBombAmmoPoolL1, 30, 1, 1000) \
    X(int, bloodBombAmmoPoolL2, 38, 1, 1000) \
    X(int, bloodBombAmmoPoolL3, 45, 1, 1000) \
    X(int, bloodFlameAmmoPoolL1, 23, 1, 1000) \
    X(int, bloodFlameAmmoPoolL2, 27, 1, 1000) \
    X(int, bloodFlameAmmoPoolL3, 33, 1, 1000) \
    X(int, bloodHammerAmmoPoolL1, 18, 1, 1000) \
    X(int, bloodHammerAmmoPoolL2, 21, 1, 1000) \
    X(int, bloodHammerAmmoPoolL3, 24, 1, 1000) \
    X(float, conventionalPickupRadius, 5.f, 1.f, 10.f) \
    X(int, conventionalLowAmmo, 3, 0, 30) \
    X(float, conventionalDiscardSeconds, 5.f, 1.f, 30.f) \
    X(float, conventionalWheelRate, 0.05f, 0.01f, 1.0f) \
    X(float, conventionalWheelDim, 0.35f, 0.0f, 1.0f) \
    COMBAT_TRACE_SETTING(X)

#ifdef BR2_COMBAT_TEST
#define COMBAT_TRACE_SETTING(X) X(int, trace, 0, 0, 1)
#else
#define COMBAT_TRACE_SETTING(X)
#endif

struct EnhancedSettings {
#define FIELD(type, key, value, low, high) type key = value;
    ENHANCED_SETTINGS(FIELD)
#undef FIELD
};

SRWLOCK g_enhancedLock = SRWLOCK_INIT;
EnhancedSettings g_pendingEnhanced, g_enhanced;
unsigned long long g_pendingCombatProfiles[2], g_combatProfile;

unsigned long long CombatProfile(const EnhancedSettings& settings, bool enhancedGuns) {
    // Version the rules and hash effective values, independent of INI formatting.
    unsigned long long hash = 14695981039346656037ull ^ 10ull;
    if (enhancedGuns) hash = (hash ^ 9u) * 1099511628211ull;
    EnhancedSettings rules = settings;
    // Framing is visual only. Keep its original v4 hash contribution so
    // records already saved with the default framing are still visible.
    rules.feedHeadStompTiltDeg = 3.0f;
    // Keep existing records for menu-grouped feed escape and display thresholds.
#define MIX(type, key, value, low, high) \
    if (strcmp(#key, "trace") != 0 && strcmp(#key, "styleRankSS") != 0 && \
        strcmp(#key, "styleRankSSS") != 0 && strcmp(#key, "styleRankV") != 0 && \
        strcmp(#key, "ephemeraHitstopScale") != 0 && strcmp(#key, "parryGuardMs") != 0 && \
        strcmp(#key, "conventionalLowAmmo") != 0 && \
        strcmp(#key, "conventionalWheelDim") != 0 && \
        strncmp(#key, "feedEscape", 10) != 0 && \
        (enhancedGuns || (strstr(#key, "AmmoPool") == nullptr && \
                         strncmp(#key, "conventional", 12) != 0))) { \
        const type field = rules.key == 0 ? type(0) : rules.key; \
        DWORD bits; \
        memcpy(&bits, &field, sizeof bits); \
        hash = (hash ^ bits) * 1099511628211ull; \
    }
    ENHANCED_SETTINGS(MIX)
#undef MIX
    return hash;
}

#ifdef BR2_COMBAT_TEST
template<class Write> void ForEachCombatSetting(Write write) {
#define WRITE(type, key, value, low, high) write(#key, (float)g_enhanced.key);
    ENHANCED_SETTINGS(WRITE)
#undef WRITE
}
#endif
char g_enhancedIni[MAX_PATH];
FILETIME g_enhancedWriteTime;
DWORD g_enhancedFileSize;
bool g_enhancedLoaded;

void ReadEnhancedSettings() {
    if (!g_enhancedIni[0]) {
        const DWORD length = GetFullPathNameA("br2mod_combat.ini", MAX_PATH, g_enhancedIni, NULL);
        if (!length || length >= MAX_PATH) {
            g_enhancedIni[0] = 0;
            return;
        }
    }
    WIN32_FILE_ATTRIBUTE_DATA file;
    if (!GetFileAttributesExA(g_enhancedIni, GetFileExInfoStandard, &file) ||
        (file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return;
    if (g_enhancedLoaded && file.nFileSizeLow == g_enhancedFileSize &&
        CompareFileTime(&file.ftLastWriteTime, &g_enhancedWriteTime) == 0)
        return;
    char values[16384];
    const DWORD length = GetPrivateProfileSectionA("Enhanced", values, sizeof values, g_enhancedIni);
    if (!length || length >= sizeof values - 2) return;
    EnhancedSettings settings;
    for (const char* entry = values; *entry; entry += strlen(entry) + 1) {
        const char* eq = strchr(entry, '=');
        if (!eq) continue;
        char* end;
        const double parsed = strtod(eq + 1, &end);
        if (end == eq + 1 || !_finite(parsed)) continue;
        while (isspace((unsigned char)*end)) ++end;
        if (*end && *end != ';' && *end != '#') continue;
        const size_t keyLength = eq - entry;
#define SET(type, key, value, low, high) \
        if (keyLength == sizeof #key - 1 && !_strnicmp(entry, #key, keyLength)) { \
            settings.key = (type)max((double)(low), min((double)(high), parsed)); \
            continue; \
        }
        ENHANCED_SETTINGS(SET)
#undef SET
    }
    settings.parryMaxSeverity = max(settings.parryMaxSeverity, settings.parryMinSeverity);
    settings.styleRankB = max(settings.styleRankB, settings.styleRankC + 1);
    settings.styleRankA = max(settings.styleRankA, settings.styleRankB + 1);
    settings.styleRankS = max(settings.styleRankS, settings.styleRankA + 1);
    settings.styleRankSS = max(settings.styleRankSS, settings.styleRankS + 1);
    settings.styleRankSSS = max(settings.styleRankSSS, settings.styleRankSS + 1);
    settings.styleRankV = max(settings.styleRankV, settings.styleRankSSS + 1);
    AcquireSRWLockExclusive(&g_enhancedLock);
    g_pendingEnhanced = settings;
    g_pendingCombatProfiles[0] = CombatProfile(settings, false);
    g_pendingCombatProfiles[1] = CombatProfile(settings, true);
    ReleaseSRWLockExclusive(&g_enhancedLock);
    g_enhancedWriteTime = file.ftLastWriteTime;
    g_enhancedFileSize = file.nFileSizeLow;
    g_enhancedLoaded = true;
    Trace("Enhanced combat settings loaded from %s", g_enhancedIni);
}
#undef ENHANCED_SETTINGS
#undef COMBAT_TRACE_SETTING

void SampleEnhancedSettings() {
    if (TryAcquireSRWLockShared(&g_enhancedLock)) {
        const bool enhancedGuns = g_combat == CombatEnhanced &&
                                  GunCapacityEnhanced();
        g_enhanced = g_pendingEnhanced;
        g_combatProfile = g_pendingCombatProfiles[enhancedGuns];
        ReleaseSRWLockShared(&g_enhancedLock);
        if (!g_combatProfile) g_combatProfile = CombatProfile(g_enhanced, enhancedGuns);
        if (g_runBars >= 0 && !g_runDone && g_runCombat == CombatEnhanced &&
            g_runCombatProfile != g_combatProfile)
            g_runCombatChanged = true;
        // Menu switches only mask the live settings. Records still use the INI profile.
        if (!CombatEffectEnabled(EffectCinematics)) {
            g_enhanced.feedExecutionEnabled = 0;
            g_enhanced.dismemberCinematicEnabled = 0;
            g_enhanced.groundCinematicEnabled = 0;
            g_enhanced.finisherKillSlow = 0;
            g_enhanced.finisherFovMs = 0;
        }
        if (!CombatEffectEnabled(EffectHitstop)) g_enhanced.hitstopEnabled = 0;
        if (!CombatEffectEnabled(EffectParry)) {
            g_enhanced.parryEnabled = 0;
            g_enhanced.riposteEnabled = 0;
        }
        if (!CombatEffectEnabled(EffectDodge)) g_enhanced.dodgeOffsetEnabled = 0;
    }
}
