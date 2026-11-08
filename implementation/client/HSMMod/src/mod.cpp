// Worker thread: load config, then (unless role=off) bring up the engine layer and networking.
#include <windows.h>
#include "config.h"
#include "file_hash.h"
#include "hsm_protocol.h"
#include "gameplay/save_guard.h"
#include "log.h"
#include "paths.h"
#include "session/hsm_cmdline.h"
#include "ue4ss_load.h"

namespace hsm {
bool StartMod(const Config& cfg);   // implemented in gameplay/session.cpp
namespace game { std::string ModBuild(); }   // gameplay/lobby.cpp

DWORD WINAPI ModMain(void*) {
    // "Play alone, mod off" started this game: wait for the one that started it to close first, so the two never write the mod's files at once.
    if (const std::string cl = CommandLineUtf8(); !CommandLineIgnoresHsmFlags(cl))
        if (const int parent = CmdArgInt(cl, "hsmwaitfor", 0, 1, 0x7fffffff))
            if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(parent))) { WaitForSingleObject(h, 30000); CloseHandle(h); }
    LogInit();
    InstallCrashLog();
    HSM_LOG("HalfSwordMP loaded (protocol rev %u)", unsigned(kProtocolRevision));
    // The build string carries this DLL's hash (session/hsm_build.h); made here, off the game thread.
    HSM_LOG("build: %s%s", game::ModBuild().c_str(), OwnBinaryHash().empty() ? " (could not read my own DLL: no hash)" : "");
    Config cfg = LoadConfig();
    game::SweepAtStartup();   // S1: before the role gate, so role=off still handles a crashed session's backup
    // UE4SS is chain-loaded here, on the worker thread (never in DllMain: the loader lock). role=off still loads it:
    // HSMMod's proxy replaced UE4SS's, so skipping it would silently turn the player's mods off.
    if (cfg.ue4ssBefore) ue4ss::Run(cfg);
    if (cfg.role == Role::Off) { HSM_LOG("role=off: staying vanilla"); ue4ss::Run(cfg); return 0; }
    if (!StartMod(cfg)) HSM_LOG("startup failed; game continues unmodified");
    ue4ss::Run(cfg);   // order=after: StartMod ends with InstallProcessEventHook, so HSMMod's hook is in (a no-op if already run)
    return 0;
}
}  // namespace hsm
