// Diagnostic-only instrumentation. No fallback names, registry resets or asset edits.
#include <stdarg.h>

static HANDLE g_trace = INVALID_HANDLE_VALUE;
static LONG g_traceSeq;
static DWORD g_attachThreadId;
static DWORD g_watchedThreadId;
static LONG g_glassNumber;
// Normal play does not install per-file/per-effect instrumentation.
static int g_traceEnabled = 0;
static int g_watchEnabled = 0;

// Inline callbacks must preserve live x87/SSE state as well as GPRs/flags.
// EBX holds the aligned save area and is callee-saved by our C callbacks.
#define BR2_SAVE_CPU \
    __asm pushfd \
    __asm pushad \
    __asm sub esp, 528 \
    __asm mov ebx, esp \
    __asm add ebx, 15 \
    __asm and ebx, 0xfffffff0 \
    __asm fxsave [ebx] \
    __asm fninit
#define BR2_RESTORE_CPU \
    __asm fxrstor [ebx] \
    __asm add esp, 528 \
    __asm popad \
    __asm popfd

static int SafeRead(const void* address, void* out, SIZE_T size) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, out, size, &got) && got == size;
}
static DWORD ReadWord(DWORD address) {
    DWORD value = 0; SafeRead((void*)address, &value, sizeof value); return value;
}
static float ReadFloat(DWORD address) {
    float value = 0; SafeRead((void*)address, &value, sizeof value); return value;
}
static void SafeText(const char* source, char* dest, unsigned capacity) {
    if (!capacity) return;
    if (!source) { strcpy_s(dest, capacity, "<NULL>"); return; }
    unsigned n = 0;
    // Read one byte at a time: valid NUL strings near an unreadable page are OK.
    for (; n + 1 < capacity; ++n) {
        char c;
        if (!SafeRead(source + n, &c, 1)) {
            if (!n) strcpy_s(dest, capacity, "<unreadable>");
            else dest[n] = 0;
            return;
        }
        if (!c) { dest[n] = 0; return; }
        dest[n] = (c >= 32 && c != 127) ? c : '?';
    }
    dest[n] = 0;
}
static void Trace(const char* format, ...) {
    if (g_trace == INVALID_HANDLE_VALUE) return;
    DWORD lastError = GetLastError();
    char line[4096];
    int prefix = _snprintf_s(line, sizeof line, _TRUNCATE, "%06ld tick=%lu tid=%lu ",
                            InterlockedIncrement(&g_traceSeq), GetTickCount(), GetCurrentThreadId());
    if (prefix < 0) return;
    va_list args; va_start(args, format);
    _vsnprintf_s(line + prefix, sizeof line - prefix, _TRUNCATE, format, args);
    va_end(args);
    unsigned length = (unsigned)strlen(line);
    if (length + 2 < sizeof line) { line[length++] = '\r'; line[length++] = '\n'; }
    DWORD written; WriteFile(g_trace, line, length, &written, NULL);
    SetLastError(lastError);
}
static void TraceRegistry(const char* reason) {
    if (g_trace == INVALID_HANDLE_VALUE) return;
    Trace("REGISTRY %s (effect cache, 150 slots; readable != live allocation)", reason);
    for (unsigned i = 0; i < 150; ++i) {
        DWORD p = ReadWord(0x5DCDD30 + 4 * i);
        if (p) { char text[160]; SafeText((char*)p, text, sizeof text);
            Trace("SLOT %u object=%08lX name='%s'", i, p, text); }
    }
}
static void TraceGlobals(const char* reason) {
    if (g_trace == INVALID_HANDLE_VALUE) return;
    DWORD glass = ReadWord(0x7C0BD0), font = ReadWord(0x7E6438);
    char name[160], current[80], picked[80], pending[80];
    SafeText((char*)glass,name,sizeof name);
    SafeText((char*)0x5E33258,current,sizeof current);
    SafeText((char*)G_PICKED_LVL,picked,sizeof picked);
    SafeText((char*)0x6030408,pending,sizeof pending);
    Trace("STATE %s glass=%08lX '%s' current='%s' picked='%s' pending='%s' handlers=%08lX,%08lX count=%lu",
          reason,glass,name,current,picked,pending,ReadWord(G_HANDLER_ARRAY),ReadWord(G_HANDLER_ARRAY+4),ReadWord(0x5DCC0B4));
    Trace("MENU %s id=%lu count=%lu selected=%lu stack=%lu scroll=%lu body=%g title=%g font=%08lX font.scale=%g font.unit=%g cell=%lu line=%lu",
          reason,ReadWord(G_MENU_STATE),ReadWord(G_ITEM_COUNT),ReadWord(G_SELIDX),ReadWord(0x5EF48AC),ReadWord(0x5EF4C24),
          (double)ReadFloat(0x5EF5664),(double)ReadFloat(0x5EF565C),font,
          (double)ReadFloat(font+0x2490),(double)ReadFloat(font+0x2494),ReadWord(font+0x2468),ReadWord(font+0x2470));
}
static void InitTrace() {
    g_traceEnabled = GetPrivateProfileIntA("Diagnostics","LoaderTrace",0,".\\br2mod.ini");
    g_watchEnabled = GetPrivateProfileIntA("Diagnostics","WatchGlassPointer",0,".\\br2mod.ini");
    if (!g_traceEnabled) return;
    char path[MAX_PATH], suffix[100], cwd[MAX_PATH];
    GetModuleFileNameA(NULL,path,sizeof path);
    char* last = strrchr(path,'\\'); if (!last) return; last[1] = 0;
    _snprintf_s(suffix,sizeof suffix,_TRUNCATE,"MODDING\\investigations\\2026-11-04-loader\\loader-%lu.log",GetCurrentProcessId());
    strcat_s(path,suffix);
    g_trace = CreateFileA(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                         NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH,NULL);
    if (g_trace == INVALID_HANDLE_VALUE) { g_traceEnabled = 0; return; }
    GetCurrentDirectoryA(sizeof cwd,cwd);
    Trace("BUILD ui-fix-4-light %s %s pid=%lu cwd='%s' attachThread=%lu",__DATE__,__TIME__,GetCurrentProcessId(),cwd,g_attachThreadId);
    TraceGlobals("worker-start");
}
static void ArmGlassWatch() {
    if (!g_traceEnabled || !g_watchEnabled) return;
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT|THREAD_SET_CONTEXT,FALSE,g_attachThreadId);
    if (!thread) { Trace("WATCH OpenThread failed %lu",GetLastError()); return; }
    if (SuspendThread(thread) == (DWORD)-1) { Trace("WATCH suspend failed %lu",GetLastError()); CloseHandle(thread); return; }
    CONTEXT c = {}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(thread,&c)) {
        // Preserve any debugger's existing breakpoints instead of replacing them.
        if (c.Dr7 & 0xff) Trace("WATCH not armed: thread already has debug breakpoints Dr7=%08lX",c.Dr7);
        else {
            c.Dr0 = 0x7C0BD0; c.Dr6 = 0; c.Dr7 = (c.Dr7 & ~0x000f0003u) | 0x000d0001u;
            g_watchedThreadId = g_attachThreadId;
            if (!SetThreadContext(thread,&c)) { g_watchedThreadId = 0; Trace("WATCH set failed %lu",GetLastError()); }
            else Trace("WATCH armed write/4 address=007C0BD0 on main thread=%lu",g_attachThreadId);
        }
    } else Trace("WATCH get failed %lu",GetLastError());
    ResumeThread(thread); CloseHandle(thread);
}
static LONG TraceException(EXCEPTION_POINTERS* ep) {
    CONTEXT* c = ep->ContextRecord;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && GetCurrentThreadId() == g_watchedThreadId && (c->Dr6 & 1)) {
        DWORD value = ReadWord(0x7C0BD0); char text[160]; SafeText((char*)value,text,sizeof text);
        Trace("GLASS-WRITE nextEIP=%08lX value=%08lX '%s' eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX",
              c->Eip,value,text,c->Eax,c->Ebx,c->Ecx,c->Edx,c->Esi,c->Edi,c->Ebp,c->Esp);
        DWORD frame = c->Ebp;
        for (unsigned i=0;i<10 && frame;++i) {
            DWORD words[10]; if (!SafeRead((void*)frame,words,sizeof words)) break;
            Trace("WRITE-FRAME %u ebp=%08lX ret=%08lX args=%08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX",
                  i,frame,words[1],words[2],words[3],words[4],words[5],words[6],words[7],words[8],words[9]);
            if (words[0] <= frame || words[0]-frame > 0x100000) break; frame=words[0];
        }
        c->Dr6 &= ~1u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    Trace("AV eip=%08lX access=%lu address=%08lX eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX ebp=%08lX esp=%08lX",
          c->Eip,(DWORD)ep->ExceptionRecord->ExceptionInformation[0],(DWORD)ep->ExceptionRecord->ExceptionInformation[1],
          c->Eax,c->Ebx,c->Ecx,c->Edx,c->Esi,c->Edi,c->Ebp,c->Esp);
    DWORD frame=c->Ebp;
    for(unsigned i=0;i<24 && frame;++i) {
        DWORD w[8]; if(!SafeRead((void*)frame,w,sizeof w)) break;
        Trace("AV-FRAME %u ebp=%08lX ret=%08lX args=%08lX %08lX %08lX %08lX %08lX %08lX",i,frame,w[1],w[2],w[3],w[4],w[5],w[6],w[7]);
        if(w[0]<=frame || w[0]-frame>0x100000) break; frame=w[0];
    }
    TraceGlobals("exception");
    return EXCEPTION_CONTINUE_SEARCH;
}
static void __cdecl TraceOpen(DWORD caller,const char* dir,const char* name,DWORD flags) {
    char d[80],n[200]; SafeText(dir,d,sizeof d); SafeText(name,n,sizeof n);
    Trace("OPEN caller=%08lX dir@%08lX='%s' name@%08lX='%s' flags=%lu",caller,(DWORD)dir,d,(DWORD)name,n,flags);
}
static void __cdecl TraceExists(DWORD caller,const char* path,DWORD result) {
    if (result & 0xff) return;
    char p[240]; SafeText(path,p,sizeof p); Trace("NOT-FOUND caller=%08lX path='%s'",caller,p);
}
static void __cdecl TraceOpened(DWORD caller,const char* path,DWORD result) {
    char p[240]; SafeText(path,p,sizeof p); Trace("OPEN-RESULT caller=%08lX result=%08lX path='%s'",caller,result,p);
}
static void __cdecl TraceGlass() {
    TraceGlobals("CGlass-before-effect");
    if (InterlockedIncrement(&g_glassNumber)==1) TraceRegistry("before first CGlass");
}
static void __cdecl TraceEffectName(const char* stage,const char* name,DWORD object,DWORD slot) {
    char text[200]; SafeText(name,text,sizeof text);
    Trace("EFFECT %s name@%08lX='%s' object=%08lX slot=%lu glass=%08lX",stage,(DWORD)name,text,object,slot,ReadWord(0x7C0BD0));
}
static const char g_effectRegister[] = "register-4A2B20";
static const char g_effectLoad[] = "load-4A2953";
static void __cdecl TraceSituation(DWORD caller,DWORD flags) {
    g_glassNumber=0; Trace("SITUATION caller=%08lX flags=%lu",caller,flags);
    TraceGlobals("situation-build"); TraceRegistry("situation-build");
}

// 0x64D230 stolen: push ebp; mov ebp,esp; sub esp,22Ch (9 bytes).
static void __declspec(naked) HookOpenTrace() {
    __asm { push ebp }
    __asm { mov ebp,esp }
    __asm { sub esp,0x22c }
    BR2_SAVE_CPU
    __asm { push dword ptr [ebp+0x10] }
    __asm { push dword ptr [ebp+0xc] }
    __asm { push dword ptr [ebp+8] }
    __asm { push dword ptr [ebp+4] }
    __asm { call TraceOpen }
    __asm { add esp,16 }
    BR2_RESTORE_CPU
    __asm { push 0x64D239 }
    __asm { ret }
}
// 0x64D26E stolen: mov edx,[esi]; add esp,4; test al,al (7 bytes).
static void __declspec(naked) HookExistsTrace() {
    BR2_SAVE_CPU
    __asm { push eax }
    __asm { lea eax,[ebp-0x22c] }
    __asm { push eax }
    __asm { push dword ptr [ebp+4] }
    __asm { call TraceExists }
    __asm { add esp,12 }
    BR2_RESTORE_CPU
    __asm { mov edx,[esi] }
    __asm { add esp,4 }
    __asm { test al,al }
    __asm { push 0x64D275 }
    __asm { ret }
}
// 0x64D2A5 stolen: mov ecx,[ebp-4]; pop edi; pop esi; xor ecx,ebp (7B).
static void __declspec(naked) HookOpenedTrace() {
    BR2_SAVE_CPU
    __asm { push eax }
    __asm { lea eax,[ebp-0x22c] }
    __asm { push eax }
    __asm { push dword ptr [ebp+4] }
    __asm { call TraceOpened }
    __asm { add esp,12 }
    BR2_RESTORE_CPU
    __asm { mov ecx,[ebp-4] }
    __asm { pop edi }
    __asm { pop esi }
    __asm { xor ecx,ebp }
    __asm { push 0x64D2AC }
    __asm { ret }
}
// 0x5038EC stolen: mov ecx,[7C0BD0h] (6B).
static void __declspec(naked) HookGlassTrace() {
    BR2_SAVE_CPU
    __asm { call TraceGlass }
    BR2_RESTORE_CPU
    __asm { mov ecx,dword ptr ds:[0x7c0bd0] }
    __asm { push 0x5038F2 }
    __asm { ret }
}
// 0x57A200 stolen: push ebp; mov ebp,esp; push -1; push SEH-handler (10B).
static void __declspec(naked) HookSituationTrace() {
    __asm { push ebp }
    __asm { mov ebp,esp }
    BR2_SAVE_CPU
    __asm { push dword ptr [ebp+8] }
    __asm { push dword ptr [ebp+4] }
    __asm { call TraceSituation }
    __asm { add esp,8 }
    BR2_RESTORE_CPU
    __asm { push -1 }
    __asm { push 0x6D2B72 }
    __asm { push 0x57A20A }
    __asm { ret }
}
// 0x4A2B20 stolen: push ebp; mov ebp,esp; sub esp,104h (9B).
static void __declspec(naked) HookEffectRegisterTrace() {
    __asm { push ebp }
    __asm { mov ebp,esp }
    __asm { sub esp,0x104 }
    BR2_SAVE_CPU
    __asm { push -1 }
    __asm { push 0 }
    __asm { push dword ptr [ebp+8] }
    __asm { push offset g_effectRegister }
    __asm { call TraceEffectName }
    __asm { add esp,16 }
    BR2_RESTORE_CPU
    __asm { push 0x4A2B29 }
    __asm { ret }
}
// Dedicated sfx trace: own flag + own file, independent from LoaderTrace.
static int g_sfxTrace = 0;
static FILE* g_sfxLog;
static void SfxTraceLine(const char* tag, const char* format, ...) {
    if (!g_sfxTrace) return;
    DWORD lastError = GetLastError();
    if (!g_sfxLog) {
        if (fopen_s(&g_sfxLog, "br2mod_sfx.log", "a") || !g_sfxLog) { SetLastError(lastError); return; }
        fprintf(g_sfxLog, "--- build %s %s pid=%lu ---\n", __DATE__, __TIME__, GetCurrentProcessId());
    }
    fprintf(g_sfxLog, "%08lu %-5s ", GetTickCount(), tag);
    va_list args; va_start(args, format);
    vfprintf(g_sfxLog, format, args);
    va_end(args);
    fputc('\n', g_sfxLog);
    fflush(g_sfxLog);
    SetLastError(lastError);
}
// 0x621CC0 stolen: push ebp; mov ebp,esp; sub esp,100h (9B).
// Engine sample-start chokepoint: every public startSfx*(name,...) wrapper
// funnels here. [ebp+8] = sample name, [ebp+4] = wrapper return address.
static void __cdecl TraceSfxStart(DWORD ret, const char* name) {
    char text[160]; SafeText(name, text, sizeof text);
    SfxTraceLine("sfx", "caller=%08lX name='%s'", ret, text);
}
static void __declspec(naked) HookSfxStartTrace() {
    __asm { push ebp }
    __asm { mov ebp,esp }
    __asm { sub esp,0x100 }
    BR2_SAVE_CPU
    __asm { push dword ptr [ebp+8] }
    __asm { push dword ptr [ebp+4] }
    __asm { call TraceSfxStart }
    __asm { add esp,8 }
    BR2_RESTORE_CPU
    __asm { push 0x621CC9 }
    __asm { ret }
}
// 0x4A2953 stolen: mov eax,[ebp+8]; push eax; push ebx; mov ecx,esi (7B).
static void __declspec(naked) HookEffectLoadTrace() {
    BR2_SAVE_CPU
    // saved EBX is at [esp+528+16]; EBX itself holds the FXSAVE area.
    __asm { mov eax,[esp+544] }
    __asm { push edi }
    __asm { push esi }
    __asm { push eax }
    __asm { push offset g_effectLoad }
    __asm { call TraceEffectName }
    __asm { add esp,16 }
    BR2_RESTORE_CPU
    __asm { mov eax,[ebp+8] }
    __asm { push eax }
    __asm { push ebx }
    __asm { mov ecx,esi }
    __asm { push 0x4A295A }
    __asm { ret }
}
