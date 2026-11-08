// Last words in HSMMod.log when the game faults: a crash otherwise just ends the log mid-session. A vectored handler
// sees the fault first (before the engine's crash reporter), writes code, address and module+offset, and lets the
// fault carry on unchanged. It never handles anything. Faults inside KernelBase are skipped: IsBadReadPtr probes fault
// there by design and are caught.
//
// The game often runs on for seconds after a fault: the engine's crash reporter takes its time before it ends the process,
// and some faults the game survives. NoteGameCarriedOn, called every game tick, logs that the game still runs
// kFaultSurvivedMs past the last fault, and lets the next fault write a dump again. Whether the session crashed is decided
// at the next start (CrashLineOf, shared/session/hsm_report.h): a fault with no normal exit after it (log.cpp LogExit).
//
// The first fault that is logged also writes a small minidump, HSMMod.crash.dmp next to HSMMod.log (the one before it
// becomes HSMMod.crash.prev.dmp). The game's own crash reporter may not leave a dump, and the log alone cannot say who
// called through a bad pointer. dbghelp.dll is loaded from System32 only, and the dump is written by a helper thread the
// handler waits for: dbghelp needs far more stack than a faulting thread may have left, and a hung dump (the faulting
// thread held a lock the helper needs) gives up after kDumpWaitMs instead of hanging the game on top of its crash.
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cwchar>
#include <string>
#include "log.h"
#include "paths.h"

namespace hsm {
namespace {
std::atomic<int> g_logged{0};
char g_lastBlow[160] = {};                // the last blow from another player: what was happening if this is the crash
std::atomic<uint64_t> g_lastBlowAtMs{0};
std::atomic<uintptr_t> g_last{0};   // the same fault again (a crash reporter retrying it): logged once
std::atomic<bool> g_dumped{false};  // one minidump per process: the first fault logged (later ones are the crash reporter's)
std::atomic<uint32_t> g_faultSeq{0};      // faults logged so far; the last one's time and thread below
std::atomic<uint64_t> g_faultAtMs{0};
std::atomic<DWORD> g_faultThread{0};
uint32_t g_carriedOnSeq = 0;              // game thread only: the last fault already marked "carried on"
constexpr uint64_t kFaultSurvivedMs = 10000;
constexpr DWORD kDumpWaitMs = 15000;
// The few dbghelp.h declarations used, as dbghelp.dll lays them out (every one of them is packed to 4 bytes there). The
// header is left out: dbghelp.dll is loaded by hand, so nothing else of it is needed.
#pragma pack(push, 4)
struct MiniDumpException { DWORD thread; EXCEPTION_POINTERS* pointers; BOOL clientPointers; };
struct MiniDumpCallbackInput {
    ULONG process; HANDLE processHandle; ULONG type;
    ULONG includeThreadId;   // the union's first member for IncludeThreadCallback (MINIDUMP_INCLUDE_THREAD_INFO)
};
struct MiniDumpCallbackOutput { ULONG writeFlags; };   // first member of the union (Module/ThreadWriteFlags)
struct MiniDumpCallbackInfo {
    BOOL(CALLBACK* routine)(PVOID, const MiniDumpCallbackInput*, MiniDumpCallbackOutput*);
    PVOID param;
};
#pragma pack(pop)
constexpr ULONG kIncludeThreadCallback = 3, kThreadWriteStack = 2;
// MiniDumpNormal | WithIndirectlyReferencedMemory | WithUnloadedModules | WithThreadInfo | IgnoreInaccessibleMemory
constexpr DWORD kDumpType = 0x40 | 0x20 | 0x1000 | 0x20000;

wchar_t g_dumpPath[MAX_PATH], g_prevPath[MAX_PATH];   // set by InstallCrashLog; the handler allocates nothing

struct DumpJob {
    EXCEPTION_POINTERS* ep = nullptr;
    DWORD thread = 0;               // the faulting thread
    DWORD error = 0;                // 0: written
    unsigned long long bytes = 0;
};
DumpJob g_job;

bool Fatal(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: case EXCEPTION_ILLEGAL_INSTRUCTION: case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_INT_DIVIDE_BY_ZERO: case EXCEPTION_STACK_OVERFLOW: case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: case 0xC0000374 /* heap corruption */: case 0xC0000409 /* stack buffer overrun */:
            return true;
        default: return false;
    }
}

// A jump through a bad pointer leaves no module at the fault address, so name the code addresses on the stack instead:
// the first few that land inside a loaded module are the likely callers (a scan, not an unwind; good enough to say
// whether HSMMod or the game made the call).
void ModuleOf(uintptr_t a, char* name, size_t n, uintptr_t* off) {
    HMODULE m = nullptr;
    name[0] = 0;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(a), &m) || !m) return;
    char path[MAX_PATH];
    DWORD k = GetModuleFileNameA(m, path, MAX_PATH);
    if (!k || k >= MAX_PATH) return;
    const char* base = path;
    for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
    std::snprintf(name, n, "%s", base);
    *off = a - uintptr_t(m);
}

void LogCallers(uintptr_t rsp) {
    char line[512]; size_t used = 0; int found = 0;
    line[0] = 0;
    for (int i = 0; i < 256 && found < 6; ++i) {
        uintptr_t v = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(rsp + uintptr_t(i) * 8), &v, sizeof v, nullptr)) break;
        if (v < 0x10000) continue;
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<const void*>(v), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
            !(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) continue;
        char mod[MAX_PATH]; uintptr_t off = 0;
        ModuleOf(v, mod, sizeof mod, &off);
        if (!mod[0]) continue;
        int w = std::snprintf(line + used, sizeof line - used, " %s+0x%llX", mod, (unsigned long long)off);
        if (w <= 0 || size_t(w) >= sizeof line - used) break;
        used += size_t(w); ++found;
    }
    if (found) LogTry("crash: stack code addresses:%s", line);
}

// Only the faulting thread keeps its stack: the others' stacks are most of a dump of a game with ~100 threads, and the
// fault is on the thread whose stack matters.
BOOL CALLBACK DumpFilter(PVOID, const MiniDumpCallbackInput* in, MiniDumpCallbackOutput* out) {
    if (in && out && in->type == kIncludeThreadCallback && in->includeThreadId != g_job.thread)
        out->writeFlags &= ~kThreadWriteStack;
    return TRUE;
}

DWORD WINAPI DumpThread(void*) {
    using WriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, DWORD, MiniDumpException*, void*, MiniDumpCallbackInfo*);
    wchar_t dir[MAX_PATH];
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n + 14 >= MAX_PATH) { g_job.error = ERROR_PATH_NOT_FOUND; return 1; }
    std::wcscpy(dir + n, L"\\dbghelp.dll");
    HMODULE m = LoadLibraryW(dir);
    WriteDump write = m ? reinterpret_cast<WriteDump>(reinterpret_cast<void*>(GetProcAddress(m, "MiniDumpWriteDump"))) : nullptr;
    if (!write) { g_job.error = GetLastError() ? GetLastError() : ERROR_PROC_NOT_FOUND; return 1; }
    MoveFileExW(g_dumpPath, g_prevPath, MOVEFILE_REPLACE_EXISTING);   // keep the last two
    HANDLE f = CreateFileW(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { g_job.error = GetLastError(); return 1; }
    MiniDumpException ex{g_job.thread, g_job.ep, FALSE};   // the pointers are in this process
    MiniDumpCallbackInfo cb{DumpFilter, nullptr};
    const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, kDumpType, &ex, nullptr, &cb);
    g_job.error = ok ? 0 : (GetLastError() ? GetLastError() : ERROR_WRITE_FAULT);
    LARGE_INTEGER size{};
    if (GetFileSizeEx(f, &size)) g_job.bytes = static_cast<unsigned long long>(size.QuadPart);
    CloseHandle(f);
    return ok ? 0 : 1;
}

void WriteMinidump(EXCEPTION_POINTERS* ep) {
    if (!g_dumpPath[0] || g_dumped.exchange(true)) return;
    g_job.ep = ep;
    g_job.thread = GetCurrentThreadId();
    HANDLE t = CreateThread(nullptr, 0, DumpThread, nullptr, 0, nullptr);
    if (!t) { LogTry("crash: no minidump (CreateThread failed, error %lu)", GetLastError()); return; }
    if (WaitForSingleObject(t, kDumpWaitMs) != WAIT_OBJECT_0) {
        LogTry("crash: no minidump (gave up after %lu ms)", kDumpWaitMs);
        CloseHandle(t);
        return;
    }
    CloseHandle(t);
    char path[MAX_PATH * 3] = "?";
    WideCharToMultiByte(CP_UTF8, 0, g_dumpPath, -1, path, sizeof path, nullptr, nullptr);
    if (g_job.error) LogTry("crash: no minidump (error %lu)", g_job.error);
    else LogTry("crash: minidump %s (%llu KB)", path, g_job.bytes / 1024);
}

}  // namespace

// The calling thread's stack as module+offset, innermost first (an unwind, unlike LogCallers' scan).
void LogCallStack(const char* what) {
    void* frames[12];
    const USHORT n = RtlCaptureStackBackTrace(1, 12, frames, nullptr);
    char line[768]; size_t used = 0;
    line[0] = 0;
    for (USHORT i = 0; i < n; ++i) {
        char mod[MAX_PATH]; uintptr_t off = 0;
        ModuleOf(reinterpret_cast<uintptr_t>(frames[i]), mod, sizeof mod, &off);
        const int w = mod[0] ? std::snprintf(line + used, sizeof line - used, " %s+0x%llX", mod, (unsigned long long)off) : 0;
        if (w <= 0 || size_t(w) >= sizeof line - used) break;
        used += size_t(w);
    }
    Log("%s: stack%s", what, line);
}

namespace {
LONG CALLBACK OnFault(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD& r = *ep->ExceptionRecord;
    if (!Fatal(r.ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(r.ExceptionAddress), &mod) && mod) {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(mod, path, MAX_PATH);
        if (n && n < MAX_PATH) {
            const char* base = path;
            for (const char* p = path; *p; ++p) if (*p == '\\' || *p == '/') base = p + 1;
            std::snprintf(name, sizeof name, "%s", base);
        }
    }
    if (_stricmp(name, "KernelBase.dll") == 0) return EXCEPTION_CONTINUE_SEARCH;
    if (g_last.exchange(uintptr_t(r.ExceptionAddress)) == uintptr_t(r.ExceptionAddress)) return EXCEPTION_CONTINUE_SEARCH;
    if (g_logged.fetch_add(1) >= 8) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t off = mod ? uintptr_t(r.ExceptionAddress) - uintptr_t(mod) : uintptr_t(r.ExceptionAddress);
    unsigned long long target = r.NumberParameters >= 2 ? (unsigned long long)r.ExceptionInformation[1] : 0;
    const char* op = r.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 1
        ? (r.ExceptionInformation[0] == 0 ? " reading" : r.ExceptionInformation[0] == 1 ? " writing" : " executing") : "";
    LogTry("crash: fault 0x%08lX at %s+0x%llX%s 0x%llX (thread %lu); the game may close now",
           r.ExceptionCode, name, (unsigned long long)off, op, target, GetCurrentThreadId());
    if (const uint64_t blow = g_lastBlowAtMs.load(); blow && GetTickCount64() - blow < 5000) LogTry("crash: the last blow from another player, %llu ms ago: %s", (unsigned long long)(GetTickCount64() - blow), g_lastBlow);
    g_faultAtMs.store(GetTickCount64());
    g_faultThread.store(GetCurrentThreadId());
    g_faultSeq.fetch_add(1);
    if (ep->ContextRecord) {
        const CONTEXT& c = *ep->ContextRecord;
        LogTry("crash: rax %llX rcx %llX rdx %llX r8 %llX rsp %llX", (unsigned long long)c.Rax, (unsigned long long)c.Rcx,
               (unsigned long long)c.Rdx, (unsigned long long)c.R8, (unsigned long long)c.Rsp);
        // Executing at an address no module owns is a call through a bad pointer: [rsp] is then the return address, the
        // code that made the call.
        uintptr_t ret = 0;
        char caller[MAX_PATH]; uintptr_t callerOff = 0;
        if (name[0] == '?' && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(c.Rsp), &ret, sizeof ret, nullptr)) {
            ModuleOf(ret, caller, sizeof caller, &callerOff);
            if (caller[0]) LogTry("crash: return address on the stack: %s+0x%llX", caller, (unsigned long long)callerOff);
        }
        LogCallers(c.Rsp);
    }
    WriteMinidump(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void NoteGameCarriedOn() {
    const uint32_t seq = g_faultSeq.load();
    if (seq == g_carriedOnSeq || GetTickCount64() - g_faultAtMs.load() < kFaultSurvivedMs) return;
    g_carriedOnSeq = seq;
    g_dumped.store(false);   // a later fault gets its own dump
    HSM_LOG("crash: the game still runs %llu s after the fault on thread %lu",
            (unsigned long long)(kFaultSurvivedMs / 1000), (unsigned long)g_faultThread.load());
}

void NoteLastBlow(int attacker, int striker, int source, const char* component, const char* bone) {
    std::snprintf(g_lastBlow, sizeof g_lastBlow, "e%d (striker %d, source %d, %s on %s)", attacker, striker, source, component, bone);
    g_lastBlowAtMs.store(GetTickCount64());
}

void InstallCrashLog() {
    const std::wstring dump = InstanceFilePath(L"HSMMod", L".crash.dmp"), prev = InstanceFilePath(L"HSMMod", L".crash.prev.dmp");
    if (dump.size() < MAX_PATH && prev.size() < MAX_PATH) {
        std::wcscpy(g_dumpPath, dump.c_str());
        std::wcscpy(g_prevPath, prev.c_str());
    }
    if (AddVectoredExceptionHandler(1, OnFault)) HSM_LOG("crash log: armed");
}
}  // namespace hsm
