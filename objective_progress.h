// Read-only view of the exact native CSpawnTrigger victory population.
// Native implementation: 0x62C420, especially 0x62C59F..0x62C741.
// No script flags, actor health, spawn state or victory conditions are written.
struct ObjectiveProgress {
    DWORD trigger;
    int spawned, alive, target;
};

static DWORD ActorWord(DWORD object, unsigned offset) {
    return *(DWORD*)(object + offset);
}

static bool ReadObjectiveProgress(ObjectiveProgress* result) {
    DWORD situation = *(DWORD*)0x7C2478;
    DWORD actors = *(DWORD*)0x7C5D50;
    if (!situation || !actors) return false;
    DWORD goal = 0;
    unsigned visited = 0;
    // 0x1000 is the native enabled bit used by 0x5E1DE9 / 0x5E1E48
    // before invoking actor simulation. Other tutorial spawners are excluded.
    for (DWORD p = ActorWord(situation,0x38C); p && visited++ < 10000;
         p = ActorWord(p,0x148)) {
        if (ActorWord(p,0) != 0x71D164 || !(ActorWord(p,0x24) & 0x1000)) continue;
        bool finiteGoal = strstr((const char*)p+0x220,"WaveDoneIt") != NULL;
        bool infiniteGoal = !_stricmp((const char*)p+0x1BC,"waveComplete");
        if (!finiteGoal && !infiniteGoal) continue;
        if (goal) return false; // ambiguous objectives: never invent a sum
        goal = p;
    }
    if (!goal || visited >= 10000) return false;

    // Build the same emitter-class union as native 0x62C548..0x62C59F.
    char classes[352] = {0};
    unsigned used = 0;
    for (unsigned i=0; i<10; ++i) {
        DWORD emitter = ActorWord(goal,0x16C+i*4);
        if (!emitter) break;
        const char* type = (const char*)emitter + 0x158;
        unsigned length = 0;
        while (length<32 && type[length]) ++length;
        if (used && used+1<sizeof classes) classes[used++] = '|';
        if (used+length >= sizeof classes) return false;
        memcpy(classes+used,type,length);
        used += length;
        classes[used] = 0;
    }
    if (!used) return false;

    typedef int (__thiscall *ActorState_t)(void*);
    typedef int (__thiscall *MatchesClass_t)(void*,const char*);
    typedef bool (__thiscall *InsideTrigger_t)(void*,void*);
    MatchesClass_t matches = (MatchesClass_t)0x4067F0;
    InsideTrigger_t inside = (InsideTrigger_t)0x62C170;
    int count = (int)ActorWord(actors,0x3F08);
    if (count<0 || count>4096) return false;
    int alive = 0;
    for (int i=0; i<count; ++i) {
        DWORD actor = ActorWord(actors,0x3F0C+i*8);
        if (!actor) break; // native loops stop at the first empty list slot
        ActorState_t state = (ActorState_t)ActorWord(ActorWord(actor,0),0x130);
        if (state((void*)actor)==2 || !matches((void*)actor,classes)) continue;
        bool inArea = inside((void*)goal,(void*)actor);
        for (unsigned j=0; !inArea && j<4; ++j) {
            DWORD linked = ActorWord(goal,0x15C+j*4);
            if (!linked) break;
            inArea = inside((void*)linked,(void*)actor);
        }
        if (inArea) ++alive;
    }
    result->trigger = goal;
    result->spawned = (int)ActorWord(goal,0x1B8);
    result->target = (int)ActorWord(goal,0x1B4);
    result->alive = alive;
    return result->spawned>=0;
}
