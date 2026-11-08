#include "gameplay/mod_rules.h"
#include "util/hsm_ini.h"
#include "match/hsm_online_rules.h"
#include <cstdio>
#include <ctime>
#include <windows.h>
#include <psapi.h>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <vector>
#include "enhancer.h"
#include "file_hash.h"
#include "gameplay/lobby.h"
#include "gameplay/mod_maps.h"
#include "log.h"
#include "net/update_check.h"
#include "paths.h"
#include "session/hsm_ue4ss.h"
#include "ue4ss_load.h"

namespace hsm::game::modrules {
namespace {
bool g_enabled = true, g_challenge = true;
ModRule g_chosen;   // from the INI; the list only for Custom with mod_allow= set
bool g_allowFromIni = false;

// ---- worker: the UE4SS mods and the proxies (files on disk) -------------------------------------------------------

struct Extras {
    std::mutex mu;
    std::vector<ModItem> items;
    bool ue4ss = false, done = false, running = false;
    uint32_t version = 0;
};
Extras g_extras;
std::atomic<bool> g_extrasWanted{false};

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n > 0 ? n : 0), L' ');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}
bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// The folder (with a trailing backslash) UE4SS.dll was loaded from, when it is loaded in this process.
std::wstring Ue4ssFolder() {
    HMODULE m = GetModuleHandleW(L"UE4SS.dll");
    if (!m) return {};
    wchar_t p[MAX_PATH];
    const DWORD n = GetModuleFileNameW(m, p, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring s(p, n);
    const size_t k = s.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring() : s.substr(0, k + 1);
}

// A UE4SS mod's hash: SHA-256 of Scripts/main.lua, or of dlls/main.dll, or (both there) SHA-256 of the two digests.
bool HashUe4ssMod(const std::wstring& dir, Sha256Digest& out) {
    Sha256Digest lua{}, dll{};
    const bool hasLua = FileExists(dir + L"Scripts\\main.lua") && HashFile(dir + L"Scripts\\main.lua", lua);
    const bool hasDll = FileExists(dir + L"dlls\\main.dll") && HashFile(dir + L"dlls\\main.dll", dll);
    if (hasLua && hasDll) {
        uint8_t both[64];
        std::memcpy(both, lua.data(), 32); std::memcpy(both + 32, dll.data(), 32);
        out = Sha256Of(both, sizeof both);
        return true;
    }
    if (hasLua) { out = lua; return true; }
    if (hasDll) { out = dll; return true; }
    return false;
}

void ScanExtras() {
    std::vector<ModItem> items;
    const std::wstring folder = Ue4ssFolder();
    const bool ue4ss = !folder.empty();
    if (ue4ss) {
        const std::wstring mods = folder + L"Mods\\";
        for (const std::string& name : ue4ss::EnabledModNames(mods)) {
            ModItem it;
            it.name = SanitizeModItemName(name);
            if (IsStockUe4ssMod(name)) { it.kind = ModKind::Ue4ssStock; items.push_back(it); continue; }
            it.kind = ModKind::Ue4ssMod;
            it.hashed = HashUe4ssMod(mods + Wide(name) + L"\\", it.sha);
            items.push_back(it);
        }
    }
    const std::wstring exeDir = ModFilePath(L"");
    for (const char* n : {"version.dll", "dsound.dll", "winmm.dll", "xinput1_3.dll", "d3d11.dll", "dxgi.dll"}) {
        const std::wstring p = exeDir + Wide(n);
        if (!FileExists(p)) continue;
        ModItem it; it.kind = ModKind::Proxy; it.name = n;
        it.hashed = HashFile(p, it.sha);
        items.push_back(it);
    }
    std::lock_guard<std::mutex> lock(g_extras.mu);
    g_extras.items = std::move(items);
    g_extras.ue4ss = ue4ss;
    g_extras.done = true;
    ++g_extras.version;
}

void StartExtras() {
    {
        std::lock_guard<std::mutex> lock(g_extras.mu);
        if (g_extras.running) { g_extrasWanted = true; return; }
        g_extras.running = true;
        g_extras.done = false;
    }
    std::thread([] {
        do { g_extrasWanted = false; ScanExtras(); } while (g_extrasWanted.load());
        std::lock_guard<std::mutex> lock(g_extras.mu);
        g_extras.running = false;
    }).detach();
}

// ---- game thread ----------------------------------------------------------------------------------------------------

struct Local {
    bool started = false;
    bool ue4ssSeen = false;
    bool scanDone = false;                       // the pak scan was listed and hashed at the last extras start
    uint32_t scanAtExtras = 0;                   // modmaps::ScanVersion() then
    uint32_t mapsVersion = ~0u, extrasVersion = ~0u;
    bool enhancer = false;
    ModReport report;
    uint32_t version = 0;
    std::string logged;
};
Local g;

void Rebuild() {
    ModReport r;
    const std::vector<ModPak>& paks = modmaps::Paks();
    const std::vector<ModKind>& kinds = modmaps::PakKinds();
    for (size_t i = 0; i < paks.size(); ++i) {
        ModItem it;
        it.kind = i < kinds.size() ? kinds[i] : ModKind::ContentPak;
        it.name = SanitizeModItemName(paks[i].name);
        it.hashed = paks[i].hashed; it.sha = paks[i].sha;
        r.items.push_back(it);
    }
    bool extrasDone = false;
    {
        std::lock_guard<std::mutex> lock(g_extras.mu);
        for (const ModItem& it : g_extras.items) r.items.push_back(it);
        r.ue4ss = g_extras.ue4ss;
        extrasDone = g_extras.done && !g_extras.running;
    }
    if (g.enhancer) { ModItem it; it.kind = ModKind::Module; it.name = "HSEnhancer"; r.items.push_back(it); }
    r.complete = modmaps::Listed() && modmaps::AllHashed() && extrasDone;
    NormalizeReport(r);
    if (r == g.report) return;
    g.report = std::move(r);
    ++g.version;
    if (!g.report.complete) return;
    const std::string line = ReportLogLine(g.report);
    if (line != g.logged) { g.logged = line; HSM_LOG("modrules: my mods: %s", line.c_str()); }
}
}  // namespace

namespace {
Config g_cfg;                 // the rule keys as last read
FILETIME g_iniTime{};
ULONGLONG g_nextIniCheck = 0;

bool IniWriteTime(FILETIME& out) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(ModFilePath(L"HSMMod.ini").c_str(), GetFileExInfoStandard, &a)) return false;
    out = a.ftLastWriteTime;
    return true;
}

void ApplyRule(const Config& cfg) {
    ModPreset p = ModPreset::MapsOnly;
    if (!ParseModPreset(cfg.modRule, p)) HSM_LOG("modrules: mod_rule=%s is not vanilla, maps, bundle, custom, anything or classes; using maps", cfg.modRule.c_str());
    g_chosen = ModRule{};
    g_chosen.preset = p;
    g_chosen.blockCheats = cfg.modBlockCheats;
    g_chosen.officialOnly = cfg.modOfficialOnly;
    g_allowFromIni = !cfg.modAllow.empty();
    if (g_allowFromIni) g_chosen.list = ParseAllowList(cfg.modAllow);
}

// The host edited the rule keys while the game runs: the next ModRulesTick (gameplay/session.cpp) announces the change.
void CheckIni() {
    const ULONGLONG now = GetTickCount64();
    if (now < g_nextIniCheck) return;
    g_nextIniCheck = now + 2000;
    FILETIME t;
    if (!IniWriteTime(t) || CompareFileTime(&t, &g_iniTime) == 0) return;
    g_iniTime = t;
    Config c = g_cfg;
    ReadModRuleKeys(c);
    if (c.modRule == g_cfg.modRule && c.modBlockCheats == g_cfg.modBlockCheats && c.modOfficialOnly == g_cfg.modOfficialOnly && c.modAllow == g_cfg.modAllow) return;
    g_cfg = c;
    ApplyRule(c);
    HSM_LOG("modrules: HSMMod.ini changed; my lobbies: %s", RuleSummary(g_chosen).c_str());
}
std::string ReadTextFile(const std::wstring& path) {
    std::string text;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) { text.resize(8 * 1024 * 1024); text.resize(std::fread(text.data(), 1, text.size(), f)); std::fclose(f); }
    return text;
}
}  // namespace

// A class's Half Wolverine parts go into its file now, before UE4SS starts the mod, because it reads the file only once. The player's
// own settings stay in a copy, and come back when they pick a class that doesn't use the mod.
static void EnableUe4ssHotReload(const Config& cfg) {
    // Off in a stock UE4SS. It's read when UE4SS starts, which is after this.
    if (!cfg.wolverineRefresh) return;
    const std::wstring path = ModFilePath(L"ue4ss\\UE4SS-settings.ini");
    const std::string text = ReadTextFile(path);
    if (text.empty()) return;
    const std::string want = util::SetIniValue(text, "General", "EnableHotReloadSystem", "1");
    if (want == text) return;
    if (FILE* f = _wfopen(path.c_str(), L"wb")) {
        const bool ok = std::fwrite(want.data(), 1, want.size(), f) == want.size();
        std::fclose(f);
        HSM_LOG("classes: UE4SS's hot reload %s, so Half Wolverine can pick up a class without a restart", ok ? "turned on" : "could not be turned on");
    }
}
void ApplyWolverineClass(const Config& cfg) {
    if (!cfg.classWolverine) return;
    const std::wstring path = ModFilePath(L"ue4ss\\Mods\\HalfWolverine\\config.ini"), keep = ModFilePath(L"HSMMod-wolverine-own.ini");
    const std::string text = ReadTextFile(path);
    if (text.empty()) return;
    const std::optional<ClassSheet> c = ResolveClass(cfg.className, cfg.classCustom);
    const auto write = [](const std::wstring& to, const std::string& what) {
        FILE* f = _wfopen(to.c_str(), L"wb");
        if (!f) return false;
        const bool ok = std::fwrite(what.data(), 1, what.size(), f) == what.size();
        std::fclose(f);
        return ok;
    };
    const std::string own = ReadTextFile(keep);
    if (c && ClassNeedsWolverine(*c)) {
        const std::string want = WolverineFileFor(text, *c);
        if (want == text) return;
        if (own.empty() && !write(keep, text)) { HSM_LOG("classes: could not keep your Half Wolverine settings, so they were left alone"); return; }
        HSM_LOG("classes: Half Wolverine set for %s: %s", cfg.className.c_str(), write(path, want) ? "ok" : "could not write its file");
    } else if (!own.empty()) {
        const bool back = write(path, own);
        HSM_LOG("classes: your Half Wolverine settings: %s", back ? "put back" : "could not be put back");
        if (back) DeleteFileW(keep.c_str());
    }
}

void ApplyWolverineFile(const Config& cfg) { ApplyWolverineClass(cfg); }

namespace {
// "next": turned off for the next game. "now": this is that game.
wchar_t g_modsKeep[MAX_PATH * 2] = {}, g_modsTxt[MAX_PATH * 2] = {}, g_modsState[MAX_PATH * 2] = {};
bool g_modsOffThisGame = false;
constexpr int kMaxSetAside = 32;
wchar_t g_asideFrom[kMaxSetAside][MAX_PATH * 2] = {}, g_asideTo[kMaxSetAside][MAX_PATH * 2] = {};   // ready for the exit, which can't build strings
int g_asideCount = 0;
wchar_t g_asideList[MAX_PATH * 2] = {};
std::wstring AsideListPath() { return ModFilePath(L"HSMMod-files-off.txt"); }
std::wstring WideName(const std::string& s) { wchar_t w[MAX_PATH]; int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w, MAX_PATH); return n > 0 ? std::wstring(w) : std::wstring(); }
std::vector<std::string> AsideList() {
    std::vector<std::string> out;
    const std::string text = ReadTextFile(AsideListPath());
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        if (end > at) out.push_back(text.substr(at, end - at));
        at = end + 1;
    }
    return out;
}
void PutFilesBack() {
    for (const std::string& name : AsideList()) {
        const std::wstring w = WideName(name);
        if (w.empty() || name.find_first_of("\\/:") != std::string::npos) continue;   // a bare name beside the exe, nothing else
        const bool back = MoveFileExW((ModFilePath(w.c_str()) + L".off").c_str(), ModFilePath(w.c_str()).c_str(), 0);
        HSM_LOG("modrules: %s: %s", name.c_str(), back ? "put back" : "could not be put back; it's still there as .off");
    }
    DeleteFileW(AsideListPath().c_str());
}
std::wstring ModsTxtPath() { return ModFilePath(L"ue4ss\\Mods\\mods.txt"); }
std::wstring ModsKeepPath() { return ModFilePath(L"HSMMod-mods-own.txt"); }
std::wstring ModsStatePath() { return ModFilePath(L"HSMMod-mods-off.state"); }
bool WriteFileBytes(const std::wstring& path, const std::string& bytes) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return std::fclose(f) == 0 && ok;
}
void PutModsTxtBack() {
    const std::string keep = ReadTextFile(ModsKeepPath());
    const bool back = !keep.empty() && WriteFileBytes(ModsTxtPath(), keep);
    HSM_LOG("modrules: your mods.txt: %s", back ? "put back" : "could not be put back; your own copy is HSMMod-mods-own.txt");
    if (back) DeleteFileW(ModsKeepPath().c_str());
    PutFilesBack();
    DeleteFileW(ModsStatePath().c_str());
}
void ModsTxtAtStart() {
    const std::string state = ReadTextFile(ModsStatePath());
    if (state == "next") {
        WriteFileBytes(ModsStatePath(), "now");
        wcsncpy(g_modsKeep, ModsKeepPath().c_str(), MAX_PATH * 2 - 1);
        wcsncpy(g_modsTxt, ModsTxtPath().c_str(), MAX_PATH * 2 - 1);
        wcsncpy(g_modsState, ModsStatePath().c_str(), MAX_PATH * 2 - 1);
        wcsncpy(g_asideList, AsideListPath().c_str(), MAX_PATH * 2 - 1);
        g_modsOffThisGame = true;
        for (const std::string& name : AsideList()) {
            if (g_asideCount >= kMaxSetAside || name.find_first_of("\\/:") != std::string::npos) continue;
            const std::wstring w = WideName(name);
            if (w.empty()) continue;
            wcsncpy(g_asideTo[g_asideCount], ModFilePath(w.c_str()).c_str(), MAX_PATH * 2 - 1);
            wcsncpy(g_asideFrom[g_asideCount], (ModFilePath(w.c_str()) + L".off").c_str(), MAX_PATH * 2 - 1);
            ++g_asideCount;
        }
        HSM_LOG("modrules: unsupported mods are off for this game; mods.txt goes back when it closes");
    }
    else if (state == "now") PutModsTxtBack();   // the last game didn't close cleanly
}
}  // namespace

bool TurnOffForNextGame(const std::vector<std::string>& mods, const std::vector<std::string>& dllsBesideExe) {
    const std::string text = ReadTextFile(ModsTxtPath());
    if (!mods.empty()) {
        if (ReadTextFile(ModsKeepPath()).empty() && !text.empty() && !WriteFileBytes(ModsKeepPath(), text)) return false;
        if (!WriteFileBytes(ModsTxtPath(), ModsTxtWithOff(text, mods))) return false;
    }
    // A loaded DLL can still be renamed on Windows: this game keeps it, and the next one doesn't find it.
    std::string list = ReadTextFile(AsideListPath());
    for (const std::string& name : dllsBesideExe) {
        const std::wstring w = WideName(name);
        if (w.empty() || name.find_first_of("\\/:") != std::string::npos) continue;
        if (MoveFileExW(ModFilePath(w.c_str()).c_str(), (ModFilePath(w.c_str()) + L".off").c_str(), 0)) list += name + "\n";
        else HSM_LOG("modrules: could not set aside %s (error %lu)", name.c_str(), GetLastError());
    }
    if (!list.empty() && !WriteFileBytes(AsideListPath(), list)) return false;
    if (!WriteFileBytes(ModsStatePath(), "next")) return false;
    HSM_LOG("modrules: turned off %u mod(s) in mods.txt for the next game", unsigned(mods.size()));
    return true;
}
// The process is exiting (DllMain): plain Windows calls on paths kept ready, and no logging. A crash skips this, and the next start puts it back.
void RestoreModsTxt() {
    if (!g_modsOffThisGame) return;
    for (int i = 0; i < g_asideCount; ++i) MoveFileExW(g_asideFrom[i], g_asideTo[i], 0);
    if (g_asideCount) DeleteFileW(g_asideList);
    if (g_modsKeep[0]) MoveFileExW(g_modsKeep, g_modsTxt, MOVEFILE_REPLACE_EXISTING);
    DeleteFileW(g_modsState);
}

void Configure(const Config& cfg) {
    ModsTxtAtStart();
    ApplyWolverineClass(cfg);
    EnableUe4ssHotReload(cfg);
    g_enabled = cfg.modRules;
    g_challenge = cfg.modChallenge;
    g_cfg = cfg;
    IniWriteTime(g_iniTime);
    ApplyRule(cfg);
    HSM_LOG("modrules: %s; my lobbies: %s%s", g_enabled ? "on" : "off (mod_rules=0)", RuleSummary(g_chosen).c_str(),
            g_enabled && !g_challenge ? "; code challenges off (mod_challenge=0)" : "");
}

bool Enabled() { return g_enabled; }

ModChoice Chosen() { return ChoiceOf(g_chosen); }

bool SetChoice(const ModChoice& c) {
    if (c == ChoiceOf(g_chosen)) return true;
    g_cfg.modRule = ModPresetKey(c.preset);
    g_cfg.modBlockCheats = c.blockCheats;
    g_cfg.modOfficialOnly = c.officialOnly;
    ApplyRule(g_cfg);
    const bool saved = SaveIniValues({{"mod_rule", g_cfg.modRule}, {"mod_cheats", c.blockCheats ? "block" : "allow"},
                                      {"mod_official", c.officialOnly ? "1" : "0"}}, "mod");
    IniWriteTime(g_iniTime);   // our own write: CheckIni must not read it back as a hand edit
    HSM_LOG("modrules: MODS tab: my lobbies: %s%s", RuleSummary(g_chosen).c_str(), saved ? "" : " (could not write HSMMod.ini)");
    return saved;
}
bool ChallengeEnabled() { return g_enabled && g_challenge; }

void Tick() {
    // Built with mod_rules=0 too: a client always sends its ModList, the host's rules need it.
    if (g_enabled) CheckIni();
    enhancer::Poll();
    const bool ue4ss = GetModuleHandleW(L"UE4SS.dll") != nullptr;   // UE4SS may load after HSMMod (ue4ss_order=after)
    const uint32_t mv = modmaps::ScanVersion();
    const bool scanDone = modmaps::Listed() && modmaps::AllHashed();
    // The UE4SS mods and proxies are read at the start, when UE4SS appears or goes, and after each finished pak rescan
    // (the player dropped files in and the scan ran again).
    const bool rescanned = scanDone && (!g.scanDone || mv != g.scanAtExtras);
    if (!g.started || ue4ss != g.ue4ssSeen || rescanned) {
        StartExtras();
        g.started = true;
        g.ue4ssSeen = ue4ss;
        g.scanAtExtras = mv;
    }
    g.scanDone = scanDone;
    uint32_t ev;
    { std::lock_guard<std::mutex> lock(g_extras.mu); ev = g_extras.version + (g_extras.running ? 1u << 31 : 0u); }
    const bool enh = enhancer::Detected();
    if (mv == g.mapsVersion && ev == g.extrasVersion && enh == g.enhancer) return;
    g.mapsVersion = mv; g.extrasVersion = ev; g.enhancer = enh;
    Rebuild();
}

const ModReport& Own() { return g.report; }
uint32_t OwnVersion() { return g.version; }
Official OwnOfficial() { return update::OfficialStatus(); }

ModRule HostRule(const Lobby& lobby) {
    ModRule r = g_chosen;
    if (r.preset == ModPreset::Bundle) {
        r.list.clear();
        const LobbyPack pack = lobby.Pack();
        for (const LocalPack& lp : modmaps::Packs())
            if (lp.valid && pack.any() && lp.manifest.name == pack.name) { r.list = BundleList(lp.manifest); break; }
        if (r.list.empty()) r.preset = ModPreset::MapsOnly;   // no pack announced (yet): nothing to bundle
    } else if (r.preset == ModPreset::Custom && !g_allowFromIni) {
        r.list = ListFromReport(g.report);
    }
    return EffectiveRule(r, g.report, OwnOfficial());
}

void LobbyTick(Lobby& lobby) {
    static ULONGLONG next = 0;
    const ULONGLONG now = GetTickCount64();
    if (!lobby.InLobby() || now < next) return;
    next = now + 1000;
    lobby.SetOfficial(OwnOfficial());
    if (!lobby.IsHost()) return;
    if (!g_enabled) { ModRule none = NoRule(); none.blockCheats = false; lobby.SetModRule(none, {}); return; }
    if (!g.report.complete) return;
    const ModRule rule = HostRule(lobby);
    std::vector<std::string> hostCheats;
    for (const ModItem& it : g.report.items) if (IsCheatUnder(rule.preset, it)) hostCheats.push_back(it.name);
    lobby.SetModRule(rule, modrules_detail::Join(hostCheats));
}

// ---- the code challenge -----------------------------------------------------------------------------------------------

namespace {
struct Image {
    bool tried = false, ok = false;
    const uint8_t* base = nullptr;
    PeLayout pe;
    std::vector<uint32_t> relocs;
};
Image g_image;

const Image& OwnImage() {
    if (g_image.tried) return g_image;
    g_image.tried = true;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&OwnImage), &self) || !self) { HSM_LOG("modrules: challenge: own module not found"); return g_image; }
    MODULEINFO mi{};
    if (!K32GetModuleInformation(GetCurrentProcess(), self, &mi, sizeof mi) || mi.SizeOfImage == 0) { HSM_LOG("modrules: challenge: module size unknown"); return g_image; }
    const auto* base = static_cast<const uint8_t*>(mi.lpBaseOfDll);
    PeLayout pe;
    if (!ParsePeLayout(base, mi.SizeOfImage, pe)) { HSM_LOG("modrules: challenge: own PE headers not as expected"); return g_image; }
    std::vector<uint32_t> relocs;
    if (pe.relocSize && !ParseDir64Relocs(base + pe.relocRva, pe.relocSize, relocs)) { HSM_LOG("modrules: challenge: own relocations not as expected"); return g_image; }
    g_image.base = base; g_image.pe = pe; g_image.relocs = std::move(relocs); g_image.ok = true;
    HSM_LOG("modrules: challenge: .text %u bytes, %zu fixups", unsigned(pe.textSize), g_image.relocs.size());
    return g_image;
}
}  // namespace

bool Answer(const ModChallengeMsg& m, Sha256Digest& out) {
    const Image& im = OwnImage();
    return im.ok && ComputeAnswer(im.base, im.pe, im.relocs, uint64_t(reinterpret_cast<uintptr_t>(im.base)), m, out);
}

uint32_t CodeSize() { const Image& im = OwnImage(); return im.ok ? im.pe.textSize : 0; }

}  // namespace hsm::game::modrules
