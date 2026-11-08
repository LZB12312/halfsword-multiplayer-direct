#include "ue4ss_load.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>
#include "log.h"
#include "paths.h"
#include "session/hsm_ue4ss.h"
#include "ue4ss_bundle.h"

// Load mechanics (assumption until an in-game test has run): UE4SS 3.x's proxy dwmapi.dll
// only forwards exports and LoadLibrary's ue4ss/UE4SS.dll; UE4SS.dll starts itself from its own DllMain by creating its
// init thread (it does not need an export called by the proxy). So a plain LoadLibraryExW of the same file from our
// worker thread starts it the same way. If a UE4SS build turns out to need an exported entry called, that call goes here,
// after the LoadLibrary. UE4SS finds UE4SS-settings.ini and Mods/ from its own module path, so the absolute path matters.
namespace hsm::ue4ss {
namespace {
std::mutex g_mu;
std::string g_version, g_mods, g_notice;
std::atomic<bool> g_ran{false};

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool DirExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) if (c >= L'A' && c <= L'Z') c = wchar_t(c - L'A' + L'a');
    return s;
}
std::wstring Wide(const char* ascii) {
    std::wstring w;
    for (; *ascii; ++ascii) w.push_back(*ascii == '/' ? L'\\' : wchar_t(static_cast<unsigned char>(*ascii)));
    return w;
}
std::wstring OurModulePath() {
    HMODULE m = nullptr;
    wchar_t p[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&OurModulePath), &m) || !m) return {};
    const DWORD n = GetModuleFileNameW(m, p, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(p, n) : std::wstring();
}

// The fixed file version of a DLL, read from its version resource without running it or loading version.dll: the DLL is
// mapped as a data file (RT_VERSION is resource type 16) and the VS_FIXEDFILEINFO (signature 0xFEEF04BD) is found in the RT_VERSION block. Empty: no
// version resource (or the file cannot be mapped).
std::string FileVersion(const std::wstring& path) {
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (!m) return {};
    std::string out;
    if (HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16))) {
        const DWORD size = SizeofResource(m, r);
        if (HGLOBAL g = LoadResource(m, r)) {
            const auto* p = static_cast<const uint8_t*>(LockResource(g));
            for (DWORD off = 0; p && off + 16 <= size && off < 256; off += 4) {
                uint32_t sig;
                std::memcpy(&sig, p + off, 4);
                if (sig != 0xFEEF04BDu) continue;
                uint32_t ms, ls;
                std::memcpy(&ms, p + off + 8, 4);
                std::memcpy(&ls, p + off + 12, 4);
                out = FormatFileVersion(ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF);
                break;
            }
        }
    }
    FreeLibrary(m);
    return out;
}

// Enabled mod names for the report: names only, never file contents beyond the parsed "Name : 1" lines.
std::vector<std::string> ReadMods(const std::wstring& modsDir) {
    std::string text;
    if (FILE* f = _wfopen((modsDir + L"mods.txt").c_str(), L"rb")) {
        text.resize(kMaxModsTxtBytes);
        text.resize(std::fread(text.data(), 1, text.size(), f));
        std::fclose(f);
    }
    std::vector<std::string> folders;
    WIN32_FIND_DATAW fd;
    if (HANDLE h = FindFirstFileW((modsDir + L"*").c_str(), &fd); h != INVALID_HANDLE_VALUE) {
        for (int seen = 0; seen < 256; ++seen) {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' && FileExists(modsDir + fd.cFileName + L"\\enabled.txt"))
                folders.push_back(Utf8(fd.cFileName));
            if (!FindNextFileW(h, &fd)) break;
        }
        FindClose(h);
    }
    return EnabledMods(ParseModsTxt(text), folders);
}

void Set(const std::string& version, const std::string& mods) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_version = version;
    g_mods = mods;
}

// One note per start about the proxies next to the exe.
void CheckProxies(const std::wstring& dir) {
    const std::wstring ours = Lower(OurModulePath()), dwmapi = dir + L"dwmapi.dll";
    const bool oursIsBeside = !ours.empty() && ours == Lower(dwmapi);
    std::vector<std::string> others;
    for (const char* n : {"version.dll", "dsound.dll", "winmm.dll"})
        if (OtherProxyName(n) && FileExists(dir + Wide(n))) others.push_back(n);
    const bool present = FileExists(dwmapi);
    const std::string note = ProxyNote(oursIsBeside, present, FileExists(dir + L"ue4ss\\dwmapi.dll"), others);
    if (note.empty()) return;
    HSM_LOG("ue4ss: proxy: %s", note.c_str());
    if (!oursIsBeside && present) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_notice = "UE4SS: " + note + ".";
    }
}
}  // namespace

void Run(const Config& cfg) {
    if (g_ran.exchange(true)) return;   // once per process
    const std::wstring dir = ModFilePath(L"");
    CheckProxies(dir);
    if (!cfg.ue4ss) {
        HSM_LOG("ue4ss: off (ue4ss=0)");
        Set("off", {});
        return;
    }
    Files files;
    files.sub = FileExists(dir + Wide(RelPath(Pick::Sub)));
    files.beside = FileExists(dir + Wide(RelPath(Pick::Beside)));
    files.xinput2x = LooksLikeUe4ss2x(FileExists(dir + L"xinput1_3.dll"), FileExists(dir + L"UE4SS-settings.ini"), DirExists(dir + L"Mods"));
    const bool proxyRunning = files.xinput2x && GetModuleHandleW(L"xinput1_3.dll");
    InstallBundled(dir, files, cfg.ue4ssKeep);
    files.sub = FileExists(dir + Wide(RelPath(Pick::Sub)));
    files.beside = FileExists(dir + Wide(RelPath(Pick::Beside)));
    // A 2.x proxy that already started keeps this one game: a second UE4SS would double-hook. The next start has no proxy.
    files.xinput2x = proxyRunning || (files.xinput2x && FileExists(dir + L"xinput1_3.dll"));
    const Pick pick = ChoosePick(files);

    if (pick == Pick::None) {
        HSM_LOG("ue4ss: none");
        Set("none", {});
        return;
    }
    const std::wstring base = pick == Pick::Proxy2x ? dir : dir + Wide(RelDir(pick)) + (RelDir(pick)[0] ? L"\\" : L"");
    const std::string mods = FormatMods(ReadMods(base + L"Mods\\"));
    if (pick == Pick::Proxy2x) {
        std::string v = FileVersion(dir + L"xinput1_3.dll");
        HSM_LOG("ue4ss: 2.x proxy (xinput1_3.dll%s%s) loads itself; HSMMod leaves it alone", v.empty() ? "" : ", ", v.c_str());
        if (!mods.empty()) HSM_LOG("ue4ss: mods: %s", mods.c_str());
        Set("2.x proxy", mods);
        return;
    }

    const std::wstring path = dir + Wide(RelPath(pick));
    const std::string shown = Utf8(path);
    // Never load twice: UE4SS's own proxy (a stray dwmapi.dll of theirs, an old copy) may already have started it.
    if (HMODULE have = GetModuleHandleW(L"UE4SS.dll")) {
        wchar_t p[MAX_PATH];
        const DWORD n = GetModuleFileNameW(have, p, MAX_PATH);
        HSM_LOG("ue4ss: already loaded (%s); not loading it again", Utf8(std::wstring(p, n < MAX_PATH ? n : 0)).c_str());
        Set(FileVersion(path).empty() ? "unknown version" : FileVersion(path), mods);
        return;
    }
    // Never pick up ourselves: the candidate is a fixed UE4SS.dll name, but a copy of HSMMod renamed to it must not be
    // loaded into its own process.
    if (Lower(path) == Lower(OurModulePath())) {
        HSM_LOG("ue4ss: %s is HSMMod itself; not loading it", shown.c_str());
        Set("none", {});
        return;
    }
    const std::string version = FileVersion(path);
    // An absolute path with LOAD_WITH_ALTERED_SEARCH_PATH: the DLL's own folder is searched for what it imports.
    HMODULE h = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        const DWORD err = GetLastError();
        HSM_LOG("ue4ss: load failed %s (error %lu)", shown.c_str(), static_cast<unsigned long>(err));
        char v[64];
        std::snprintf(v, sizeof v, "load failed (error %lu)", static_cast<unsigned long>(err));
        Set(v, mods);
        return;
    }
    HSM_LOG("ue4ss: loaded %s (%s) [%s, order=%s]", shown.c_str(), version.empty() ? "no version info" : version.c_str(), PickName(pick),
            cfg.ue4ssBefore ? "before" : "after");
    if (!mods.empty()) HSM_LOG("ue4ss: mods: %s", mods.c_str());
    Set(version.empty() ? "unknown version" : version, mods);
}

std::string ReportVersion() { std::lock_guard<std::mutex> lock(g_mu); return g_version; }
std::string ReportMods() { std::lock_guard<std::mutex> lock(g_mu); return g_mods; }
std::string Notice() { std::lock_guard<std::mutex> lock(g_mu); return g_notice; }
std::vector<std::string> EnabledModNames(const std::wstring& modsDir) { return ReadMods(modsDir); }
}  // namespace hsm::ue4ss
