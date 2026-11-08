// A DLL is judged by what it is. These are the ways round a check that goes by name, as a tester tried them.
#include "hsm_dll_rules.h"
#include <cstdio>
#include <cstdlib>
using namespace hsm;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

int main() {
    const std::string win = std::string(kWin64Dir);
    const std::string_view real = "d2afe12d3ac81f642266747a3688c157fbab614b442d12a54a15d08628c683b6";
    // The game's own pass only as themselves.
    CHECK(DllNeedsHash(win + "boost_atomic-mt-x64.dll") && JudgeGameDll(win + "boost_atomic-mt-x64.dll", real) == DllVerdict::Fine);
    CHECK(JudgeGameDll(win + "boost_atomic-mt-x64.dll", "00") == DllVerdict::Changed);
    // A swapped upscaler is normal, and needs no hash.
    const std::string dlss = "halfswordue5\\plugins\\dlss\\binaries\\thirdparty\\win64\\nvngx_dlss.dll";
    CHECK(!DllNeedsHash(dlss) && JudgeGameDll(dlss, "") == DllVerdict::Fine);
    // A name that looks like the game's isn't.
    CHECK(JudgeGameDll(win + "boost_cheat.dll", "") == DllVerdict::Unknown && JudgeGameDll(win + "tbbx.dll", "") == DllVerdict::Unknown);
    CHECK(JudgeGameDll(win + "steam_api64.dll", "") == DllVerdict::Unknown);   // the game keeps that one under Engine
    CHECK(JudgeGameDll(win + "ue4ss.dll", "") == DllVerdict::Unknown);
    // The usual proxies, and a spoof of DirectX.
    for (const char* n : {"d3d11.dll", "d3d12.dll", "version.dll", "winmm.dll", "winhttp.dll", "xinput1_3.dll", "multiplayer.dll", "x.asi"}) CHECK(JudgeGameDll(win + n, "") == DllVerdict::Unknown);
    // ReShade in that slot just works, whatever its version. A proxy there doesn't.
    CHECK(!DllNeedsHash(win + "dxgi.dll") && JudgeGameDll(win + "dxgi.dll", "", false) == DllVerdict::Changed && JudgeGameDll(win + "dxgi.dll", "", true) == DllVerdict::Fine);
    CHECK(LooksLikeKnownDxgi("..crosire's ReShade..") && LooksLikeKnownDxgi("dxvk-1.10") && !LooksLikeKnownDxgi("UltimateProxyDLL") && !LooksLikeKnownDxgi(""));
    // A subfolder of the game is no hiding place.
    CHECK(JudgeGameDll("engine\\plugins\\cheat.dll", "") == DllVerdict::Unknown);
    CHECK(JudgeGameDll(win + "ue4ss\\cheat.dll", "") == DllVerdict::Unknown);
    CHECK(JudgeGameDll(win + "ue4ss\\mods\\x\\cheat.dll", "") == DllVerdict::Unknown);
    CHECK(JudgeGameDll(win + "ue4ss\\mods\\x\\dlls\\main.dll", "") == DllVerdict::Fine);
    CHECK(DllPathLower("HalfswordUE5/Binaries\\Win64/X.DLL") == win + "x.dll");
    // Outside the game.
    for (const char* p : {"c:\\windows\\system32\\d3d11.dll", "c:\\program files\\nvidia\\x.dll", "c:\\program files (x86)\\steam\\gameoverlayrenderer64.dll", "c:\\programdata\\microsoft\\windows defender\\mpoav.dll",
                          "d:\\steam\\gameoverlayrenderer64.dll", "c:\\users\\me\\appdata\\local\\discord\\app-1\\discordhook64.dll",
                          "c:\\programdata\\nvidia\\ngx\\models\\dlss\\190_e658703.dll", "c:\\users\\me\\appdata\\local\\nvidia\\ngx\\190_e658703.dll"}) CHECK(OutsideDllFine(p));
    CHECK(OutsideDllFine("c:\\users\\me\\appdata\\local\\medal\\hookdll\\medalhook_9812\\medal-hook64-v2.dll"));
    CHECK(!OutsideDllFine("c:\\users\\me\\appdata\\local\\medal\\hookdll\\medalhook_9812\\speedhack.dll") && !OutsideDllFine("c:\\temp\\medal-hook64-v2.dll"));
    for (const char* p : {"c:\\users\\me\\downloads\\speedhack.dll", "c:\\programdata\\x\\cheat.dll", "c:\\temp\\gameoverlayrenderer64.dll", "c:\\temp\\discordhook64.dll", "d:\\windows\\cheat.dll", "c:\\temp\\windows\\cheat.dll"}) CHECK(!OutsideDllFine(p));
    // Discord's overlay hides itself from the module list, so its image is named by device. With the drive back it passes as when listed.
    const std::vector<DriveDevice> drives = {{"d:", "\\device\\harddiskvolume1"}, {"c:", "\\device\\harddiskvolume10"}, {"z:", ""}};
    const std::string hidden = DosPathOfDeviceLower("\\device\\harddiskvolume10\\users\\me\\appdata\\local\\discord\\app-1.0.9\\modules\\discord_hook\\discordhook64.dll", drives);
    CHECK(hidden == "c:\\users\\me\\appdata\\local\\discord\\app-1.0.9\\modules\\discord_hook\\discordhook64.dll" && OutsideDllFine(hidden));
    CHECK(!OutsideDllFine(DosPathOfDeviceLower("\\device\\harddiskvolume10\\users\\me\\appdata\\local\\discord\\speedhack.dll", drives)));
    CHECK(DosPathOfDeviceLower("\\device\\harddiskvolume1\\x.dll", drives) == "d:\\x.dll");
    // Volume1 isn't the start of volume10's name, a share has no drive, and a bare device name isn't a file on it.
    CHECK(DosPathOfDeviceLower("\\device\\harddiskvolume11\\x.dll", drives) == "\\device\\harddiskvolume11\\x.dll");
    CHECK(DosPathOfDeviceLower("\\device\\mup\\server\\x.dll", drives) == "\\device\\mup\\server\\x.dll");
    CHECK(DosPathOfDeviceLower("\\device\\harddiskvolume1", drives) == "\\device\\harddiskvolume1");
    // Steam installed outside Program Files, and RivaTuner wherever it was put: a player's real paths.
    for (const char* p : {"e:\\steam\\steamclient64.dll", "e:\\steam\\tier0_s64.dll", "e:\\steam\\vstdlib_s64.dll", "d:\\games\\steam\\steamclient64.dll", "e:\\steam\\steamwebrtc64.dll", "e:\\steam\\crashhandler64.dll",
                          "f:\\gpu stuff\\msi\\rivatuner statistics server\\rtsshooks64.dll"}) CHECK(OutsideDllFine(p));
    // Steam in a folder with any name, going by where Steam says it is. Nothing else in that folder earns anything by it.
    for (const char* p : {"g:\\valve client\\steamclient64.dll", "g:\\valve client\\tier0_s64.dll", "g:\\valve client\\vstdlib_s64.dll", "g:\\valve client\\gameoverlayrenderer64.dll",
                          "g:\\valve client\\steamwebrtc64.dll", "g:\\valve client\\crashhandler64.dll"})
        CHECK(OutsideDllFine(p, "c:\\windows\\", "g:\\valve client\\") && !OutsideDllFine(p));
    CHECK(!OutsideDllFine("g:\\valve client\\speedhack.dll", "c:\\windows\\", "g:\\valve client\\"));
    CHECK(!OutsideDllFine("g:\\valve client\\mods\\steamclient64.dll", "c:\\windows\\", "g:\\valve client\\"));
    // The registry's forms: SteamPath is a folder with forward slashes, the running client is a file.
    CHECK(DllFolderLower("E:/Nowy folder") == "e:\\nowy folder\\" && DllFolderLower("e:/nowy folder/") == "e:\\nowy folder\\" && DllFolderLower("").empty());
    CHECK(DllFolderOfLower("E:\\Nowy folder\\steamclient64.dll") == "e:\\nowy folder\\" && DllFolderOfLower("steamclient64.dll").empty());
    // Steam moved by hand: SteamPath names the copy left behind, and the running client says where it really is.
    const std::string oldCopy = "c:\\program files (x86)\\steam\\", moved = DllFolderOfLower("C:\\Users\\Me\\Desktop\\New folder\\steamclient64.dll");
    for (const char* p : {"c:\\users\\me\\desktop\\new folder\\steamclient64.dll", "c:\\users\\me\\desktop\\new folder\\tier0_s64.dll",
                          "c:\\users\\me\\desktop\\new folder\\vstdlib_s64.dll", "c:\\users\\me\\desktop\\new folder\\gameoverlayrenderer64.dll",
                          "c:\\users\\me\\desktop\\new folder\\steamwebrtc64.dll", "c:\\users\\me\\desktop\\new folder\\crashhandler64.dll"})
        CHECK(OutsideDllFine(p, "c:\\windows\\", oldCopy, moved) && !OutsideDllFine(p, "c:\\windows\\", oldCopy));
    CHECK(!OutsideDllFine("c:\\users\\me\\desktop\\new folder\\speedhack.dll", "c:\\windows\\", oldCopy, moved));
    CHECK(!OutsideDllFine("c:\\users\\me\\desktop\\new folder\\mods\\steamclient64.dll", "c:\\windows\\", oldCopy, moved));
    // Letters outside ASCII keep their case here, so a lower case SteamPath can miss the DLL's path. The running client's path can't.
    const std::string cyrillic = DllFolderOfLower("C:\\Users\\Me\\\xD0\x9D\xD0\xBE\xD0\xB2\xD0\xB0\xD1\x8F\\steamclient64.dll");
    CHECK(OutsideDllFine(cyrillic + "tier0_s64.dll", "c:\\windows\\", "c:\\users\\me\\\xD0\xBD\xD0\xBE\xD0\xB2\xD0\xB0\xD1\x8F\\", cyrillic));
    // With nothing read, a bare name is in no folder of Steam's.
    CHECK(!OutsideDllFine("steamclient64.dll"));
    // The names alone earn nothing: only in Steam's own folder, or RivaTuner's.
    for (const char* p : {"c:\\temp\\steamclient64.dll", "e:\\steam\\mods\\steamclient64.dll", "c:\\temp\\steamwebrtc64.dll", "e:\\steam\\mods\\crashhandler64.dll", "e:\\steam\\speedhack.dll", "c:\\temp\\rtsshooks64.dll",
                          "f:\\rivatuner statistics server\\speedhack.dll"}) CHECK(!OutsideDllFine(p));
    // UE4SS: only the one that comes with the mod, and only from its own mods folder.
    CHECK(DllNeedsHash(win + "ue4ss\\ue4ss.dll") && JudgeGameDll(win + "ue4ss\\ue4ss.dll", "94c2ebd3ca9f4c4212898d727df5482937f586771c67502635ebd4367422052a") == DllVerdict::Fine);
    CHECK(JudgeGameDll(win + "ue4ss\\ue4ss.dll", "00") == DllVerdict::Changed);
    CHECK(JudgeGameDll(win + "ue4ss\\ue4ss.dll", "82e3b900b43e0e0376144143a6391fd0a89bdd4edcf2b9b7dc246b2c0f1757b6") == DllVerdict::Fine);
    CHECK(!Ue4ssSettingsMoveMods("[Overrides]\r\n; Path\r\nModsFolderPath =\r\n;   +ModsFolderPaths = C:/MyMods\r\nControllingModsTxt =\r\n[General]\r\nHotReloadKey = R\r\n"));
    CHECK(Ue4ssSettingsMoveMods("ModsFolderPath = C:/x\r\n") && Ue4ssSettingsMoveMods("  +ModsFolderPaths = ../x\n") && Ue4ssSettingsMoveMods("ControllingModsTxt=x") && !Ue4ssSettingsMoveMods(""));
    // Vendor-signed upscaler and frame generation files pass whatever their name or version; a lookalike name with no signature doesn't.
    CHECK(WithSigner(DllVerdict::Changed, "NVIDIA Corporation") == DllVerdict::Fine && WithSigner(DllVerdict::Unknown, "Intel Corporation") == DllVerdict::Fine);
    CHECK(WithSigner(DllVerdict::Unknown, "Advanced Micro Devices, Inc.") == DllVerdict::Fine);
    CHECK(WithSigner(DllVerdict::Unknown, "") == DllVerdict::Unknown && WithSigner(DllVerdict::Changed, "") == DllVerdict::Changed);
    CHECK(WithSigner(DllVerdict::Unknown, "NVIDIA") == DllVerdict::Unknown && WithSigner(DllVerdict::Unknown, "nvidia corporation") == DllVerdict::Unknown);
    CHECK(WithSigner(DllVerdict::Unknown, "NVIDIA Corporation ") == DllVerdict::Unknown && WithSigner(DllVerdict::Fine, "") == DllVerdict::Fine);
    // Unsigned graphics add-ons pass beside the exe only as the exact file that was read; the same name with other bytes doesn't.
    CHECK(DllNeedsHash(win + "version.dll") && DllNeedsHash(win + "renodx-dlss5.addon64") && DllNeedsHash(win + "nvngx_dlssnr.dll"));
    CHECK(JudgeGameDll(win + "version.dll", "f131734e9bf1fd191a9f60fad54231cd88e45ada3c957bd02c8763f8e9ad4440") == DllVerdict::Fine);
    CHECK(JudgeGameDll(win + "renodx-dlss5.addon64", "79871879324053d49931a9f8c1865fcb83e8146df4a56b146d7a8a14cc7e70fe") == DllVerdict::Fine);
    CHECK(JudgeGameDll(win + "version.dll", "00") == DllVerdict::Unknown && JudgeGameDll(win + "nvngx_dlssnr.dll", "") == DllVerdict::Unknown);
    CHECK(JudgeGameDll(win + "winmm.dll", "f131734e9bf1fd191a9f60fad54231cd88e45ada3c957bd02c8763f8e9ad4440") == DllVerdict::Unknown);   // pinned by place too
    CHECK(JudgeGameDll(win + "ue4ss\\version.dll", "f131734e9bf1fd191a9f60fad54231cd88e45ada3c957bd02c8763f8e9ad4440") == DllVerdict::Unknown);
    std::printf("ok\n");
    return 0;
}
