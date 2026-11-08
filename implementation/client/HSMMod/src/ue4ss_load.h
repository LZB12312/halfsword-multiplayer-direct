// Chain-loads UE4SS from HSMMod's worker thread. HSMMod's dwmapi.dll is the one proxy next to the
// game exe; UE4SS 3.x lives in <exe dir>/ue4ss/UE4SS.dll (older installs: UE4SS.dll next to the exe). The choice of file
// and the mods.txt reading are in shared/session/hsm_ue4ss.h.
#pragma once
#include <string>
#include <vector>
#include "config.h"

namespace hsm::ue4ss {
// Loads UE4SS once (a second call does nothing), unless ue4ss=0 in HSMMod.ini. Call it from ModMain's worker thread only,
// never from DllMain: LoadLibrary under the loader lock deadlocks. Never throws; every outcome is one "ue4ss:" log line.
// mod.cpp calls it before StartMod when ue4ss_order=before and after StartMod (the ProcessEvent hook is installed by then)
// otherwise, and also when role=off, since HSMMod's proxy replaced UE4SS's.
void Run(const Config& cfg);

// For bug reports and crash notes (thread safe): "3.0.1.0" (the file version), "unknown version", "2.x proxy", "none",
// "off", or "load failed (error N)"; empty before Run.
std::string ReportVersion();
// Names of the enabled UE4SS mods, "A, B, C" (bounded, names only; session/hsm_ue4ss.h), empty for none.
std::string ReportMods();
// A warning for the multiplayer page when the dwmapi.dll next to the exe is not HSMMod's, else empty.
std::string Notice();
// The enabled mod folders under modsDir (".../Mods" with a trailing backslash): mods.txt and enabled.txt, as UE4SS reads
// them (session/hsm_ue4ss.h). The mod report (gameplay/mod_rules.cpp) lists them. Any thread.
std::vector<std::string> EnabledModNames(const std::wstring& modsDir);
}  // namespace hsm::ue4ss
