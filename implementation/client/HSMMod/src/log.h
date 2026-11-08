// Minimal thread-safe file logger: <game exe dir>/HSMMod.log
#pragma once
#include <cstdarg>

namespace hsm {
void LogInit();
void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// Like Log, but gives up instead of waiting if another thread holds the log (for the fault handler).
void LogTry(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void InstallCrashLog();   // crash_log.cpp
void NoteLastBlow(int attacker, int striker, int source, const char* component, const char* bone);   // crash_log.cpp: named if the game then crashes
// Called every game tick (crash_log.cpp): once the game has run kFaultSurvivedMs past the last logged fault, logs
// "crash: the game still runs ..." (the engine's crash reporter may still end it) and re-arms the minidump.
void NoteGameCarriedOn();
// DllMain, DLL_PROCESS_DETACH of a process exit: appends kCleanExitLine (shared/session/hsm_report.h) to HSMMod.log with
// kernel32 only; the C runtime's streams may be closed by then. A crash never gets here (TerminateProcess skips it).
void LogExit();
// Logs "<what>: the code addresses on this thread's stack" as module+offset (crash_log.cpp), for a call that is refused.
void LogCallStack(const char* what);
}  // namespace hsm

#define HSM_LOG(...) ::hsm::Log(__VA_ARGS__)
