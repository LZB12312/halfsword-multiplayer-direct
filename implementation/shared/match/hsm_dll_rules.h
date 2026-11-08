// Which DLLs may be loaded in the game, judged by what they are and not what they're called. A name proves nothing: a proxy can be called
// boost_x.dll or dxgi.dll. So the game's own are pinned by SHA-256, for Steam build 24185754, and anything else in the game's folders
// has to be one of a few named things. The mod only runs on this build, so a game update means a new mod and a new table, not a blocked player.
// None of this has a setting: a setting would be one more thing a cheat could flip.
// Game-independent.
#pragma once
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace hsm {

struct PinnedDll { std::string_view path, sha256; };   // from the game's folder, lower case. No hash: upscalers, which players swap for newer ones
inline constexpr PinnedDll kGameDlls[] = {
    {"engine\\binaries\\thirdparty\\dbghelp\\dbghelp.dll", "4b803d0bfa5d360cfcb3237d260226743d2255eacf6a98615334fb55deb7cf88"},
    {"engine\\binaries\\thirdparty\\msquic\\v220\\win64\\msquic.dll", "150963f4a05ffddc9a8498ae0ec4ddf5bed762a83b6ad089d2fd8d499101c7ea"},
    {"engine\\binaries\\thirdparty\\nvidia\\nvaftermath\\win64\\gfsdk_aftermath_lib.x64.dll", "4877a28a3edaa86257061d7de3d16f1aaa331bdd6e1b1446000553504d2fd3b1"},
    {"engine\\binaries\\thirdparty\\ogg\\win64\\vs2015\\libogg_64.dll", "284a6e70a6dce01f8a02d93e1bb78baf0efb6140459c587d8986d59fb6bd675b"},
    {"engine\\binaries\\thirdparty\\steamworks\\steamv157\\win64\\steam_api64.dll", "4df999c0c8cb12589f0864d52be5d4c775577aeb27fee28b49b188f9ba083eea"},
    {"engine\\binaries\\thirdparty\\vorbis\\win64\\vs2015\\libvorbisfile_64.dll", "a9d6f5538f44011c1f65d7ace64216a237636717cb0c8c59f4f89cc08dd3d24f"},
    {"engine\\binaries\\thirdparty\\vorbis\\win64\\vs2015\\libvorbis_64.dll", "0f66dcb5837e9b59c499277fd272b862043356aa9145802cf0a63e00a1bcca59"},
    {"engine\\binaries\\thirdparty\\windows\\xaudio2_9\\x64\\xaudio2_9redist.dll", "0e01f400baa09694cacfd2cfbff0d721c17b56785d2d6e29b6fdd117f70ccc1e"},
    {"halfswordue5\\binaries\\win64\\boost_atomic-mt-x64.dll", "d2afe12d3ac81f642266747a3688c157fbab614b442d12a54a15d08628c683b6"},
    {"halfswordue5\\binaries\\win64\\boost_chrono-mt-x64.dll", "a0106a6c49ba4c249e830e5c2870e62be7225af4eb878871986adcc9d5ddb097"},
    {"halfswordue5\\binaries\\win64\\boost_filesystem-mt-x64.dll", "696f9a8b24d270e79151bbab3c92411d103775340c20d738da34ef004c9cd07e"},
    {"halfswordue5\\binaries\\win64\\boost_iostreams-mt-x64.dll", "9e39f72c251266f6f6c8e376c7fd7d0cd20269d60e9fcb2c05dcf65c69f7b14a"},
    {"halfswordue5\\binaries\\win64\\boost_program_options-mt-x64.dll", "8b5d87b598d3c99fd9653019ae6b1ca59d65e0e733d410409df7cd0c7b5aad69"},
    {"halfswordue5\\binaries\\win64\\boost_python311-mt-x64.dll", "e15bd4e6f18ddcc2f5c19c6e5d6c9d40540dd1a715399c793637976cbf324011"},
    {"halfswordue5\\binaries\\win64\\boost_regex-mt-x64.dll", "6e08a46df6d8605e4d1a1065742bdc3e0450570ec539a4f262d95e6dbbc4a7b8"},
    {"halfswordue5\\binaries\\win64\\boost_system-mt-x64.dll", "7f29d86095bdd46eb3ed13f8d8325cc363989720bc38ca7eff3079c04773c59d"},
    {"halfswordue5\\binaries\\win64\\boost_thread-mt-x64.dll", "001248097cdd4ec3492d1793e10d003ae860bec3efc109779ca9c6cd3ef82095"},
    {"halfswordue5\\binaries\\win64\\openimagedenoise.dll", "7d47ea2ded08f14ff0565e3636d9e72f52a2a70bcd94c8331053b3814e17c7c5"},
    {"halfswordue5\\binaries\\win64\\tbb.dll", "d5db1d48d7025f2c59704dad2e9ba8dfc5562c134fa49d8373b823cf9ccd1957"},
    {"halfswordue5\\binaries\\win64\\tbb12.dll", "de25bc26f71c5fa53b585116db11d643dc63373fab094e5d786a2ef7eb50786d"},
    {"halfswordue5\\binaries\\win64\\tbbmalloc.dll", "c14ea6f54cdfe42b7e87455e220589799b764c5335dd3665e7343a404afadb34"},
    {"halfswordue5\\binaries\\win64\\d3d12\\d3d12core.dll", "31f364f2b3aa1753e7ba9ff912d0721c8f424be5fa14503efddafbb30c635d3b"},
    {"halfswordue5\\binaries\\win64\\d3d12\\d3d12sdklayers.dll", "5dfc320d5ee27190181b1a63e74eb481b07ea727513dbeb38a96db6157969d24"},
    {"halfswordue5\\plugins\\discordpartnersdk\\binaries\\thirdparty\\discordpartnersdklibrary\\win64\\discord_partner_sdk.dll", "d16c393e1b3f66c11a6f2589d8d74f7cc366a25a45a84f5a756c16d01aca6bc2"},
    {"halfswordue5\\plugins\\discordrpc\\source\\thirdparty\\discordrpclibrary\\win64\\discord-rpc.dll", "5cc2e504800cf4ed2f4781364f661ea22349658ddc391b5d54195e573109d87b"},
    {"halfswordue5\\plugins\\dlss\\binaries\\thirdparty\\win64\\nvngx_dlss.dll", ""},
    {"halfswordue5\\plugins\\dlss\\binaries\\thirdparty\\win64\\nvngx_dlssd.dll", ""},
    {"halfswordue5\\plugins\\fsr\\source\\fidelityfx-sdk\\kits\\fidelityfx\\signedbin\\amd_fidelityfx_framegeneration_dx12.dll", ""},
    {"halfswordue5\\plugins\\fsr\\source\\fidelityfx-sdk\\kits\\fidelityfx\\signedbin\\amd_fidelityfx_upscaler_dx12.dll", ""},
    {"halfswordue5\\plugins\\streamlinecore\\binaries\\thirdparty\\win64\\nvngx_deepdvc.dll", ""},
    {"halfswordue5\\plugins\\streamlinecore\\binaries\\thirdparty\\win64\\nvngx_dlssg.dll", ""},
    {"halfswordue5\\plugins\\streamlinecore\\binaries\\thirdparty\\win64\\sl.common.dll", "b015702b768e4fa94ab16a4c745902513d60a438f5e0350ddd6899d542365483"},
    {"halfswordue5\\plugins\\streamlinecore\\binaries\\thirdparty\\win64\\sl.deepdvc.dll", "89469c1ff15f1994529663533d7b390160466211ee82788b5731252e8a929725"},
    {"halfswordue5\\plugins\\streamlinecore\\binaries\\thirdparty\\win64\\sl.dlss_g.dll", "0f7f2465602e26a50b733ac121452dfc48f2943e43424bf3e8a2d567992dfd11"},
    {"halfswordue5\\plugins\\streamlinecore\\binaries\\thirdparty\\win64\\sl.interposer.dll", "48f79bdd8eb35d6de0e54a70799a3340913990ef5e0e3a837a4f724008d5685b"},
    {"halfswordue5\\plugins\\streamlinecore\\binaries\\thirdparty\\win64\\sl.pcl.dll", "b3d3e4f5571b5a67bd83d1d20793fa0e5c3088aa41e961faba2e316d8339b987"},
    {"halfswordue5\\plugins\\streamlinecore\\binaries\\thirdparty\\win64\\sl.reflex.dll", "366172fc3e3e98a82080521a5b4825c2d2ff664273631e86792fe47fc0c32078"},
    {"halfswordue5\\plugins\\zibravdb\\zibravdbnative\\blosc.dll", "a204d3386358695e84f09e6188a26f4b5399795afa3eb65947babbfc8e080f44"},
    {"halfswordue5\\plugins\\zibravdb\\zibravdbnative\\compressionenginenativeplugin_win.dll", "f9a8b5b7838301ed41900af89a9ec187da01515285110d994a1f87039104069e"},
    {"halfswordue5\\plugins\\zibravdb\\zibravdbnative\\compressionenginenativeplugin_win_editor.dll", "6e7371a179cca352cc6b2c5e3b842622112ea75cb162435cb56f7ab6430997c3"},
    {"halfswordue5\\plugins\\zibravdb\\zibravdbnative\\imath-3_1.dll", "cb27c51f88ffccabe8da19075daf8d828749b364b2e155da75091e29e1418f3b"},
    {"halfswordue5\\plugins\\zibravdb\\zibravdbnative\\lz4.dll", "c59f007a9fd36c30f31d23632e3407bee07e3256eb0ad2526975dadea3f13f8d"},
    {"halfswordue5\\plugins\\zibravdb\\zibravdbnative\\openvdb.dll", "9b8ec38cf706db7aa8e0953e3b9e623d25ceed04b270a2894e5da573fdf53f08"},
    {"halfswordue5\\plugins\\zibravdb\\zibravdbnative\\tbb12.dll", "335fb6171ee63a943a98b18774d964f51960def7630ccc9d01171adf4a48a10f"},
    {"halfswordue5\\plugins\\zibravdb\\zibravdbnative\\winpixeventruntime.dll", "365a63da6a83c91b76bd7f8a9efcc3806b6188a6d29acdbe9d7e43decf3510d8"},
    {"halfswordue5\\plugins\\zibravdb\\zibravdbnative\\zlib1.dll", "a41a8c433fe6800170e5d92f75b963482b54dfbd0791d1c7daa0121fb557ee19"},
    {"halfswordue5\\plugins\\zibravdb\\zibravdbnative\\zstd.dll", "0fe78e8fb1883503d7b29723b9b380c7c0708c71bec29ffa0bd1f4fa696a09c0"},
};
// The dxgi.dll slot is what a proxy uses, but it's also where ReShade goes, and nobody should have to clear their ReShade with anyone.
// So it passes if it's what it looks like. Copying the text means building a cheat on purpose, which no check on this PC can stop.
inline bool LooksLikeKnownDxgi(std::string_view fileBytes) {
    return fileBytes.find("crosire") != std::string_view::npos || fileBytes.find("ReShade") != std::string_view::npos || fileBytes.find("dxvk") != std::string_view::npos;
}

// Graphics add-ons that no vendor signs, pinned by SHA-256 where players put them, beside the exe. A newer version is a new line here, never
// a looser check: the hash is what keeps a cheat from taking one of these names. Each was read and named before it went in.
struct KnownAddOnDll { std::string_view file, sha256, what; };
inline constexpr KnownAddOnDll kKnownAddOnDlls[] = {
    {"version.dll", "f131734e9bf1fd191a9f60fad54231cd88e45ada3c957bd02c8763f8e9ad4440", "DLSS Enabler 4.9.0.15 (OptiScaler 0.9.5-pre3 inside)"},
    {"nvngx_dlssnr.dll", "6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927", "NVIDIA DLSSNR 310.8 from the DLSS 5 one-click pack (its signature cut off)"},
    {"renodx-dlss5.addon64", "79871879324053d49931a9f8c1865fcb83e8146df4a56b146d7a8a14cc7e70fe", "RenoDX DLSS5 Generic 10.8.0, a ReShade add-on"},
};

inline std::string DllPathLower(std::string_view path) {
    std::string out(path);
    for (char& c : out) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
// The UE4SS builds this mod accepts. The mod carries one (kBundledUe4ss, client src/ue4ss_bundle.cpp) and players may have another, so this
// lists versions rather than one file. A build of his own is how a tester got a gun mod past the checks and its damage to other players.
// A new accepted build goes here.
inline constexpr std::string_view kBundledUe4ss = "aab30033e336df1feae7ee5f175a7dbdbdc0630c9c3d70f3086d212f25009f8b";   // v3.0.1-1217-g4fb9b615
inline constexpr std::string_view kKnownUe4ss[] = {
    "94c2ebd3ca9f4c4212898d727df5482937f586771c67502635ebd4367422052a",   // v3.0.1 Beta, git f58e8f84
    "8ac18fbffc1ef96b0662d4a2d537b3f224c26d65caaba7989a9404c566102b26",   // v3.0.1, the release
    "82e3b900b43e0e0376144143a6391fd0a89bdd4edcf2b9b7dc246b2c0f1757b6",   // a tester's experimental-latest, as sent 2026-11-04
    "d946975cc6d2d9a1bd07d801c5e2b77350d4dbc6431c08a1f302bb175bd1d860",   // v3.0.1-1212-gd4aebbf0
    "405e9ecf23940894de5fb8eb7a0fb271aa584b20eacd777d04ce2a91669fb9ac",   // v3.0.1-1214-g69f1eba5
    "aab30033e336df1feae7ee5f175a7dbdbdc0630c9c3d70f3086d212f25009f8b",   // v3.0.1-1217-g4fb9b615, the one the mod carries
};
inline bool IsKnownUe4ss(std::string_view sha256) {
    for (std::string_view h : kKnownUe4ss) if (h == sha256) return true;
    return false;
}
// UE4SS's own settings can load mods from anywhere, which nobody else's list would show.
inline bool Ue4ssSettingsMoveMods(std::string_view ini) {
    size_t at = 0;
    while (at < ini.size()) {
        size_t end = ini.find('\n', at);
        if (end == std::string_view::npos) end = ini.size();
        std::string_view line = ini.substr(at, end - at);
        at = end + 1;
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.remove_suffix(1);
        if (line.empty() || line.front() == ';') continue;
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        std::string key = DllPathLower(line.substr(0, eq));
        while (!key.empty() && key.back() == ' ') key.pop_back();
        std::string_view value = line.substr(eq + 1);
        while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        if (value.empty()) continue;
        if (key == "modsfolderpath" || key == "controllingmodstxt" || key == "+modsfolderpaths" || key == "modsfolderpaths") return true;
    }
    return false;
}

inline constexpr std::string_view kWin64Dir = "halfswordue5\\binaries\\win64\\";

// Whether a judgement needs the file's hash, so the rest aren't read at all.
inline bool DllNeedsHash(std::string_view relLower) {
    if (relLower == std::string(kWin64Dir) + "ue4ss\\ue4ss.dll") return true;
    for (const KnownAddOnDll& d : kKnownAddOnDlls) if (relLower == std::string(kWin64Dir) + std::string(d.file)) return true;
    return std::any_of(std::begin(kGameDlls), std::end(kGameDlls), [&](const PinnedDll& d) { return d.path == relLower && !d.sha256.empty(); });
}

enum class DllVerdict { Fine, Unknown, Changed };

// A DLL inside the game's folder, by its path from there. `sha256` may be empty only if DllNeedsHash said so.
inline DllVerdict JudgeGameDll(std::string_view relLower, std::string_view sha256, bool dxgiKnown = false) {
    const std::string win64(kWin64Dir);
    const auto under = [&](const std::string& dir) { return relLower.size() > dir.size() && relLower.compare(0, dir.size(), dir) == 0; };
    if (relLower == win64 + "dwmapi.dll") return DllVerdict::Fine;       // this mod: its own code challenge covers it
    if (relLower == win64 + "hsenhancer.dll") return DllVerdict::Fine;   // judged as HS Enhancer, once it's loaded
    if (relLower == win64 + "ue4ss\\ue4ss.dll") return std::find(std::begin(kKnownUe4ss), std::end(kKnownUe4ss), sha256) != std::end(kKnownUe4ss) ? DllVerdict::Fine : DllVerdict::Changed;
    if (under(win64 + "ue4ss\\mods\\")) {                                 // a UE4SS C++ mod: the mod rules judge it by its folder's name
        constexpr std::string_view tail = "\\dlls\\main.dll";
        return relLower.size() > tail.size() && relLower.substr(relLower.size() - tail.size()) == tail ? DllVerdict::Fine : DllVerdict::Unknown;
    }
    if (relLower == win64 + "dxgi.dll") return dxgiKnown ? DllVerdict::Fine : DllVerdict::Changed;
    for (const KnownAddOnDll& d : kKnownAddOnDlls) if (relLower == win64 + std::string(d.file) && sha256 == d.sha256) return DllVerdict::Fine;
    for (const PinnedDll& d : kGameDlls) if (d.path == relLower) return d.sha256.empty() || d.sha256 == sha256 ? DllVerdict::Fine : DllVerdict::Changed;
    return DllVerdict::Unknown;
}

// A folder as OutsideDllFine wants it: lower case, with one trailing backslash. Empty stays empty.
inline std::string DllFolderLower(std::string_view folder) {
    std::string out = DllPathLower(folder);
    while (!out.empty() && out.back() == '\\') out.pop_back();
    return out.empty() ? out : out + "\\";
}

// The folder a file's path is in, in the same form. Empty when the path has no folder.
inline std::string DllFolderOfLower(std::string_view file) {
    const std::string lower = DllPathLower(file);
    const size_t slash = lower.find_last_of('\\');
    return slash == std::string::npos ? std::string() : lower.substr(0, slash + 1);
}

// Graphics vendors swap their own DLLs in: newer DLSS, Streamline (the frame generation "bridge" files), FSR and XeSS. Players replace them
// with newer versions all the time, so a pinned hash would block them. What can't be faked is the vendor's code signature, checked by
// Windows (WinVerifyTrust), so a file with a valid signature from one of these publishers is fine wherever it is. The name on the
// certificate is the publisher's legal name, exactly.
inline constexpr std::string_view kTrustedDllSigners[] = {
    "NVIDIA Corporation", "Advanced Micro Devices, Inc.", "Advanced Micro Devices Inc.", "Intel Corporation", "Microsoft Corporation", "Microsoft Windows",
};
inline bool TrustedDllSigner(std::string_view signer) {
    return std::find(std::begin(kTrustedDllSigners), std::end(kTrustedDllSigners), signer) != std::end(kTrustedDllSigners);
}
// A verdict once the signature is known: an unknown or changed file signed by a trusted vendor is that vendor's, not a cheat.
inline DllVerdict WithSigner(DllVerdict v, std::string_view validSigner) { return v != DllVerdict::Fine && TrustedDllSigner(validSigner) ? DllVerdict::Fine : v; }

// A DLL from anywhere else on the machine: true when it says nothing about cheating. Windows itself, installed programs (both need admin
// to write to), Microsoft's own data, and the overlays people run. Everything else is somewhere a player can drop a file.
// `steamDirLower`: where Steam says it's installed (SteamPath). `steamClientDirLower`: the folder of the running client (its ActiveProcess key),
// which keeps the path's real case and can't name a copy left behind. Both as DllFolderLower gives them, and empty when they couldn't be read.
inline bool OutsideDllFine(std::string_view pathLower, std::string_view windowsDirLower = "c:\\windows\\", std::string_view steamDirLower = {}, std::string_view steamClientDirLower = {}) {
    const auto has = [&](std::string_view s) { return pathLower.find(s) != std::string_view::npos; };
    if (pathLower.rfind(windowsDirLower, 0) == 0 || (pathLower.size() > 3 && (has(":\\program files\\") || has(":\\program files (x86)\\") || has(":\\programdata\\microsoft\\")))) return true;
    // NVIDIA's driver downloads DLSS models as DLLs with names like 190_E658703.dll. A tester with DLSS on hit this.
    if (has(":\\programdata\\nvidia\\") || has("\\nvidia\\ngx\\") || has("\\appdata\\local\\nvidia\\")) return true;
    const size_t slash = pathLower.find_last_of('\\');
    const std::string_view file = slash == std::string_view::npos ? pathLower : pathLower.substr(slash + 1);
    const std::string_view dir = pathLower.substr(0, pathLower.size() - file.size());
    const bool steamDir = dir.ends_with("\\steam\\") || (!steamDirLower.empty() && dir == steamDirLower) || (!steamClientDirLower.empty() && dir == steamClientDirLower);
    if (file.rfind("gameoverlay", 0) == 0 && (steamDir || has("\\steam\\"))) return true;
    // Steam's own client, in every game it runs. In Program Files it's fine by that; a player with Steam on E:\Steam was told to close it.
    // Joining a lobby loads two more: steamwebrtc64 (its P2P networking) and crashhandler64. A player was sent out of a lobby by them.
    if ((file == "steamclient64.dll" || file == "tier0_s64.dll" || file == "vstdlib_s64.dll" || file == "steamwebrtc64.dll" || file == "crashhandler64.dll") && steamDir) return true;
    if (file.rfind("discordhook", 0) == 0 && has("\\appdata\\local\\discord")) return true;
    if (file.rfind("graphics-hook", 0) == 0 && has("\\obs-studio-hook\\")) return true;
    if (file.rfind("medal-hook", 0) == 0 && has("\\appdata\\local\\medal\\hookdll\\")) return true;   // Medal, for clips. A tester had to close it to play
    if (file.rfind("rtsshooks", 0) == 0 && has("\\rivatuner statistics server\\")) return true;   // RivaTuner's overlay, which MSI Afterburner brings along
    return false;
}

// A drive and the device Windows says it is (QueryDosDevice): "c:" and "\device\harddiskvolume3", both as DllPathLower gives them.
struct DriveDevice { std::string drive, device; };

// A mapped file's name as Windows gives it (\Device\HarddiskVolume3\Users\...) with its drive letter instead, so OutsideDllFine can judge it.
// Lower case in, lower case out. Unchanged when no drive matches (a share or a device without a letter): OutsideDllFine then passes less.
inline std::string DosPathOfDeviceLower(std::string_view deviceLower, const std::vector<DriveDevice>& drives) {
    for (const DriveDevice& d : drives) {
        if (d.device.empty() || deviceLower.size() <= d.device.size() || deviceLower[d.device.size()] != '\\') continue;
        if (deviceLower.compare(0, d.device.size(), d.device) == 0) return d.drive + std::string(deviceLower.substr(d.device.size()));
    }
    return std::string(deviceLower);
}

}  // namespace hsm
