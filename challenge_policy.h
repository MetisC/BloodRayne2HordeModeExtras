// Save prompts belong to campaign levels, not the self-contained challenges.
// This policy uses the active level name so native campaign launches remain
// unaffected after returning from a challenge. It does not edit save files.
#include <string.h>

static bool IsStandaloneChallengeLevel(const char* name) {
    if (!name) return false;
    const char* basename = name;
    for (const char* p=name; *p; ++p)
        if (*p=='\\' || *p=='/') basename=p+1;
    static const char battlePrefix[] = "A1_MANSION_PART1_BATTLE_";
    if (!_strnicmp(basename,battlePrefix,sizeof battlePrefix-1)) return true;
    if (!_strnicmp(basename,"spawn_",6)) return true;
    static const char* const hubs[] = {"hub1.lvl","hub2.lvl","hub3.lvl","hub4.lvl"};
    for (unsigned i=0;i<sizeof hubs/sizeof hubs[0];++i)
        if (!_stricmp(basename,hubs[i])) return true;
    return false;
}

// A challenge can already have chained here when the automatic save routine
// runs. This name alone is not enough to suppress a campaign save prompt;
// the caller must also retain the challenge-session context.
static bool IsChallengeMenuBridge(const char* name) {
    if (!name) return false;
    const char* base = name;
    for (const char* p=name; *p; ++p)
        if (*p=='\\' || *p=='/') base=p+1;
    return !_stricmp(base,"menu_level.lvl");
}
