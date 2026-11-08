// Entry point. DllMain only wires the dwmapi forwarders and starts a worker thread; all game work happens later
// on that thread and on the game thread (via the ProcessEvent hook).
#include <windows.h>
#include "gameplay/mod_rules.h"
#include "log.h"

bool HsmInitProxy();
namespace hsm { DWORD WINAPI ModMain(void*); }

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        HsmInitProxy();
        // One mod per process, even if an old xinput1_4.dll copy of HSMMod is also installed.
        wchar_t name[64];
        wsprintfW(name, L"Local\\HalfSwordMP-%lu", GetCurrentProcessId());
        HANDLE once = CreateMutexW(nullptr, FALSE, name);
        if (once && GetLastError() == ERROR_ALREADY_EXISTS) return TRUE;
        if (HANDLE t = CreateThread(nullptr, 0, hsm::ModMain, nullptr, 0, nullptr)) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH && reserved) {
        hsm::game::modrules::RestoreModsTxt();   // a game with mods turned off for it: the player's mods.txt comes back
        hsm::LogExit();   // the process is exiting the normal way (a crash ends it with TerminateProcess: no detach)
    }
    return TRUE;
}
