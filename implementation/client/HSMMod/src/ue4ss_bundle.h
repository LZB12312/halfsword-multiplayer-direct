// The UE4SS build HSMMod carries inside its DLL (third_party/ue4ss), so a player needs nothing but dwmapi.dll. What to do with
// it is decided in shared/session/hsm_ue4ss.h; this file does the unpacking.
#pragma once
#include <string>
#include "session/hsm_ue4ss.h"

namespace hsm::ue4ss {
// Makes the bundled build the UE4SS here: ChooseBundleStep says what to write in <exe dir>/ue4ss, and an old-layout or 2.x install
// gets .off on its name. A build the mod doesn't know gets a popup first, and the wait is here. `dir` is the exe dir with a trailing
// backslash; `keep` is the hash the player said to keep (ue4ss_keep). Call it before HSMMod loads UE4SS, from the worker thread.
// Files other than the DLL stay as the player has them. A failure is logged and whatever was there stays.
void InstallBundled(const std::wstring& dir, const Files& files, const std::string& keep);
}  // namespace hsm::ue4ss
