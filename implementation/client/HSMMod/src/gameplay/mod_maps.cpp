#include "gameplay/mod_maps.h"
#include "file_hash.h"
#include "log.h"
#include "paths.h"
#include "session/hsm_mod_rules.h"
#include "session/hsm_modpack.h"
#include "ue/engine.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

namespace hsm::game::modmaps {
using namespace ue;
namespace {

constexpr int kMaxDepth = 8;                 // folders below Content/Paks that are searched
constexpr size_t kMaxFilesSeen = 20000;      // directory entries looked at in one listing
constexpr DWORD kRegistryDelayMs = 4000;     // the registry is asked this long after the first frame, then on every Rescan

std::wstring PaksRoot() {
    wchar_t full[MAX_PATH * 2];
    const std::wstring rel = ModFilePath(L"..\\..\\Content\\Paks\\");
    const DWORD n = GetFullPathNameW(rel.c_str(), DWORD(sizeof full / sizeof full[0]), full, nullptr);
    return n && n < sizeof full / sizeof full[0] ? std::wstring(full, n) : rel;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 0x80 ? char(c) : '_';
    return s;
}
std::string LowerPath(std::wstring w) { std::string s = Narrow(w); for (char& c : s) { if (c == '\\') c = '/'; if (c >= 'A' && c <= 'Z') c = char(c + 32); } return s; }
std::string Short(const Sha256Digest& d) { return Sha256Hex(d).substr(0, 12); }

// ---- worker state ------------------------------------------------------------------------------------------------

struct Shared {
    std::mutex mu;
    std::vector<ModPak> paks;
    std::vector<PakMaps> pakMaps;
    std::vector<LocalPack> packs;             // the manifests under Paks/~mods/<pack>/ (session/hsm_modpack.h)
    std::vector<ModKind> kinds;               // per paks[i]: a map pak or another content pak (ContentPak until its index is read)
    bool listed = false, allHashed = false, running = false;
    uint32_t version = 0;                     // bumps on every change; the game thread copies when it differs
};
Shared g_shared;
std::atomic<bool> g_rescanWanted{false};

// Reads len bytes at offset; false on any short read.
bool ReadAt(HANDLE h, uint64_t offset, void* out, size_t len) {
    LARGE_INTEGER li; li.QuadPart = LONGLONG(offset);
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ReadFile(h, out, DWORD(len), &got, nullptr) && got == len;
}

// Reads the pak's own index (unencrypted, version 3 to 11) and returns the map packages it lists, and the folders of its
// other assets (for the mod rules' map-pak test, session/hsm_mod_rules.h ClassifyPak).
PakParse ReadPakMaps(const std::wstring& path, uint64_t size, std::vector<std::string>& maps, PakAssets& assets) {
    maps.clear();
    assets = PakAssets{};
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return PakParse::NotPak;
    PakParse result = PakParse::NotPak;
    do {
        const size_t tailLen = size_t(std::min<uint64_t>(size, kPakFooterReadBytes));
        std::vector<uint8_t> tail(tailLen);
        if (tailLen == 0 || !ReadAt(h, size - tailLen, tail.data(), tailLen)) break;
        PakFooter f;
        result = ParsePakFooter(tail.data(), tail.size(), size, f);
        if (result == PakParse::Unsupported) HSM_LOG("maps: pak version %u is not read (3 to 11 are, a frozen v9 index is not)", f.version);
        if (result != PakParse::Ok) break;
        std::vector<uint8_t> primary(size_t(f.indexSize));
        if (!ReadAt(h, f.indexOffset, primary.data(), primary.size())) { result = PakParse::Malformed; break; }
        if (f.version < 10) { result = ParsePakLegacyIndex(primary.data(), primary.size(), f.version, maps, &assets); break; }
        PakPrimary pr;
        result = ParsePakPrimaryIndex(primary.data(), primary.size(), size, pr);
        if (result != PakParse::Ok) break;
        std::vector<uint8_t> fdi(size_t(pr.fdiSize));
        if (!ReadAt(h, pr.fdiOffset, fdi.data(), fdi.size())) { result = PakParse::Malformed; break; }
        result = ParsePakDirectoryIndex(fdi.data(), fdi.size(), pr.mountPoint, maps, &assets);
    } while (false);
    CloseHandle(h);
    return result;
}

struct FoundPak { std::wstring path; std::string name; uint64_t size = 0, mtime = 0; };

// What a scan couldn't vouch for. The online check refuses these: a mod the list can't name mustn't be a way off the list.
std::mutex g_uncheckedMu;
std::vector<std::string> g_unchecked;
void NoteUnchecked(const std::string& what) {
    std::lock_guard<std::mutex> lock(g_uncheckedMu);
    if (g_unchecked.size() < 16 && std::find(g_unchecked.begin(), g_unchecked.end(), what) == g_unchecked.end()) g_unchecked.push_back(what);
    HSM_LOG("maps: not checked: %s", what.c_str());
}

void ListPaks(const std::wstring& root, const std::wstring& dir, int depth, std::vector<FoundPak>& out, size_t& seen) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (++seen > kMaxFilesSeen) { NoteUnchecked("Paks (too many files)"); break; }
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // The game follows these and goes as deep as it likes, so what's out of reach here has to count as something.
            if (depth >= kMaxDepth || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) NoteUnchecked(Narrow(name) + " (folder not checked)");
            else ListPaks(root, dir + name + L"\\", depth + 1, out, seen);
            continue;
        }
        const std::string n8 = Narrow(name);
        if (!EndsWithNoCase(n8, ".pak") || IsIoStoreCompanion(n8)) continue;
        if (IsStockPakName(n8, dir == root)) continue;
        FoundPak p;
        p.path = dir + name;
        p.name = SanitizePakName(n8);
        p.size = uint64_t(fd.nFileSizeHigh) << 32 | fd.nFileSizeLow;
        p.mtime = uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32 | fd.ftLastWriteTime.dwLowDateTime;
        if (!ModPakNameOk(p.name) || p.size > kMaxModPakBytes) { NoteUnchecked(p.name + " (pak not checked)"); continue; }   // the game loads it anyway
        if (p.size == 0) continue;
        out.push_back(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::wstring CachePath() { return ModFilePath(L"HSMMod-modpaks.cache"); }

std::string ReadWholeFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    std::string s;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 0 && sz.QuadPart <= 256 * 1024) {
        s.resize(size_t(sz.QuadPart));
        DWORD got = 0;
        if (!ReadFile(h, s.data(), DWORD(s.size()), &got, nullptr)) got = 0;
        s.resize(got);
    }
    CloseHandle(h);
    return s;
}
void WriteWholeFile(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    WriteFile(h, text.data(), DWORD(text.size()), &put, nullptr);
    CloseHandle(h);
}

// A folder name that is plain ASCII, else empty (a pack's folder is its name).
std::string AsciiName(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) { if (c >= 0x80) return {}; s += char(c); }
    return s;
}

// Paks/~mods/<pack>/HSMPack.ini for every folder directly under ~mods that has one. A folder without a manifest is not a
// pack (its paks are still listed and hashed as mod paks). Logged once per outcome, not at every rescan.
std::vector<LocalPack> ScanPacks(const std::wstring& root) {
    static std::set<std::string> logged;
    std::vector<LocalPack> out;
    const std::wstring mods = root + L"~mods\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((mods + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        const std::string folder = AsciiName(fd.cFileName);
        if (!PackNameOk(folder)) continue;
        const std::wstring file = mods + fd.cFileName + L"\\" + L"HSMPack.ini";
        if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (out.size() >= 16) break;
        LocalPack p;
        p.folder = folder;
        const std::string text = ReadWholeFile(file);
        p.error = text.empty() ? PackParse::MissingPack : ParsePackManifest(text, p.manifest);
        if (p.error == PackParse::Ok && modpack_detail::Lower(p.manifest.name) != modpack_detail::Lower(folder)) p.error = PackParse::BadName;
        p.valid = p.error == PackParse::Ok;
        const std::string line = p.valid ? "pack: found " + p.manifest.name + " " + p.manifest.version + " (" + std::to_string(p.manifest.paks.size()) + " paks) in ~mods/" + folder
                                         : "pack: ~mods/" + folder + "/HSMPack.ini is not valid (" + PackParseText(p.error) + ")";
        if (logged.insert(line).second) HSM_LOG("%s", line.c_str());
        out.push_back(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const LocalPack& a, const LocalPack& b) { return a.folder < b.folder; });
    return out;
}

void Publish(const std::vector<ModPak>& paks, const std::vector<PakMaps>& pakMaps, const std::vector<LocalPack>& packs, const std::vector<ModKind>& kinds,
             bool listed, bool allHashed) {
    std::lock_guard<std::mutex> lock(g_shared.mu);
    g_shared.paks = paks; g_shared.pakMaps = pakMaps; g_shared.packs = packs; g_shared.kinds = kinds; g_shared.listed = listed; g_shared.allHashed = allHashed;
    ++g_shared.version;
}

void ScanOnce() {
    { std::lock_guard<std::mutex> lock(g_uncheckedMu); g_unchecked.clear(); }
    const ULONGLONG t0 = GetTickCount64();
    const std::wstring root = PaksRoot();
    std::vector<FoundPak> found;
    size_t seen = 0;
    ListPaks(root, root, 0, found, seen);
    std::sort(found.begin(), found.end(), [](const FoundPak& a, const FoundPak& b) { return LowerPath(a.path) < LowerPath(b.path); });
    if (found.size() > kMaxModPaks) {
        HSM_LOG("maps: %zu mod paks found, only the first %zu are used", found.size(), kMaxModPaks);
        NoteUnchecked(std::to_string(found.size() - kMaxModPaks) + " paks (too many paks)");
        found.resize(kMaxModPaks);
    }
    std::vector<ModPak> paks;
    for (const FoundPak& f : found) { ModPak p; p.name = f.name; p.size = f.size; paks.push_back(p); }
    std::vector<PakMaps> pakMaps;
    std::vector<ModKind> kinds(paks.size(), ModKind::ContentPak);
    const std::vector<LocalPack> packs = ScanPacks(root);
    Publish(paks, pakMaps, packs, kinds, true, paks.empty());     // listed: the client can already say "no such pak" for names that are absent

    HashCache cache;
    cache.Parse(ReadWholeFile(CachePath()));
    bool cacheDirty = false;
    for (size_t i = 0; i < found.size(); ++i) {
        const FoundPak& f = found[i];
        const std::string key = LowerPath(f.path);
        Sha256Digest sha{};
        bool have = false;
        if (auto c = cache.Lookup(key, f.size, f.mtime)) { sha = *c; have = true; }
        else if (HashFile(f.path, sha)) { cache.Put(key, f.size, f.mtime, sha); cacheDirty = true; have = true; }
        if (!have) { HSM_LOG("maps: mod pak %s: could not be read", f.name.c_str()); paks[i].size = 0; continue; }
        paks[i].hashed = true; paks[i].sha = sha;
        std::vector<std::string> maps;
        PakAssets assets;
        const PakParse r = ReadPakMaps(f.path, f.size, maps, assets);
        // An unread index cannot show the pak holds only maps: it counts as a content pak.
        PakClass judged;
        if (r == PakParse::Ok) judged = ClassifyPakWhy(maps, assets.dirs, assets.overflow, assets.overflowWhy, assets.badMaps, assets.stockFiles);
        else judged.why = std::string("index not read: ") + PakParseName(r);
        kinds[i] = judged.kind;
        HSM_LOG("maps: mod pak %s %llu %s %s%s", f.name.c_str(), (unsigned long long)f.size, Short(sha).c_str(), ModKindName(judged.kind),
                judged.why.empty() ? "" : (" (" + judged.why + ")").c_str());
        if (r == PakParse::Ok && assets.badMaps) HSM_LOG("maps: mod pak %s: %zu map(s) can't go on the map list, first %s", f.name.c_str(), assets.badMaps, assets.badMapName.c_str());
        if (r == PakParse::Ok && !maps.empty()) pakMaps.push_back({i, maps});
        Publish(paks, pakMaps, packs, kinds, true, false);
    }
    // A pak that could not be read is dropped from the list so nothing waits on its hash.
    std::vector<ModPak> kept; std::vector<PakMaps> keptMaps; std::vector<ModKind> keptKinds;
    std::vector<size_t> remap(paks.size(), size_t(-1));
    for (size_t i = 0; i < paks.size(); ++i) if (paks[i].hashed) { remap[i] = kept.size(); kept.push_back(paks[i]); keptKinds.push_back(kinds[i]); }
    for (PakMaps pm : pakMaps) { pm.pak = remap[pm.pak]; keptMaps.push_back(pm); }
    if (cacheDirty) WriteWholeFile(CachePath(), cache.Serialize());
    Publish(kept, keptMaps, packs, keptKinds, true, true);
    HSM_LOG("maps: scanned %zu mod pak(s) under %s in %llu ms", kept.size(), Narrow(root).c_str(), (unsigned long long)(GetTickCount64() - t0));
}

void StartWorker() {
    {
        std::lock_guard<std::mutex> lock(g_shared.mu);
        if (g_shared.running) { g_rescanWanted = true; return; }
        g_shared.running = true;
    }
    std::thread([] {
        do {
            g_rescanWanted = false;
            ScanOnce();
        } while (g_rescanWanted.load());
        std::lock_guard<std::mutex> lock(g_shared.mu);
        g_shared.running = false;
    }).detach();
}

// ---- game-thread state -------------------------------------------------------------------------------------------

struct Local {
    bool started = false;
    uint32_t seenVersion = 0;
    std::vector<ModPak> paks;
    std::vector<PakMaps> pakMaps;
    std::vector<LocalPack> packs;
    std::vector<ModKind> kinds;
    bool listed = false, allHashed = false;
    std::vector<std::string> registryMaps;
    bool registryDone = false, registryTried = false;
    DWORD registryAt = 0;
    std::vector<ModMapEntry> maps;
    std::set<std::string> loggedMaps;
    bool dirty = true;
};
Local g;

// FAssetData in UE 5.4: the first three members are FName PackageName, PackagePath, AssetName (8 bytes each).
// Its size is read from the reflected OutAssetData parameter (a TArray of a struct), never guessed.
constexpr size_t kArrayInnerOffset = 0x70;      // FArrayProperty::Inner follows FProperty (0x70 bytes in this build)
constexpr int32_t kMinAssetDataSize = 40, kMaxAssetDataSize = 256;
constexpr int32_t kMaxRegistryMaps = 20000;

bool Readable(const void* p, size_t len) {
    MEMORY_BASIC_INFORMATION mbi;
    const char* c = static_cast<const char*>(p);
    const char* end = c + len;
    while (c < end) {
        if (!VirtualQuery(c, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        c = static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

std::string LastSegmentOf(const std::string& p) { const size_t k = p.rfind('/'); return k == std::string::npos ? p : p.substr(k + 1); }

// Asks the asset registry for every World asset. False (and the reason logged) when the registry or its layout is not as expected.
bool QueryRegistry(std::vector<std::string>& out) {
    out.clear();
    std::vector<UObject*> regs = FindInstances("AssetRegistryImpl");
    UObject* reg = regs.empty() ? nullptr : regs[0];
    if (!reg) {
        // The shipping game keeps no named instance we can find: ask AssetRegistryHelpers.GetAssetRegistry, which returns
        // the registry as a TScriptInterface (its object pointer first).
        UClass* helpers = FindClass("AssetRegistryHelpers");
        PreparedCall get(helpers, "GetAssetRegistry", {"ReturnValue"});
        if (helpers && get.valid() && get.invoke(DefaultObject(helpers))) reg = get.take<UObject*>(0);
    }
    if (!reg) { HSM_LOG("maps: asset registry not found"); return false; }
    // Only a function the registry's own class lists is called (the interface's thunk is not called on it): without one the
    // pak index alone names the maps.
    PreparedCall pc(ClassOf(reg), "GetAssetsByClass", {"ClassPathName", "bSearchSubClasses", "OutAssetData"});
    if (!pc.valid()) { HSM_LOG("maps: GetAssetsByClass not found on the asset registry (%s); the pak index names the maps", ObjectName(reg).c_str()); return false; }
    // The element size from the reflected parameter: OutAssetData is TArray<FAssetData>, its Inner a StructProperty.
    int32_t stride = 0;
    for (FProperty* p : Params(pc.function())) {
        if (PropertyName(p) != "OutAssetData") continue;
        const void* const* slot = reinterpret_cast<const void* const*>(static_cast<const char*>(static_cast<const void*>(p)) + kArrayInnerOffset);
        if (!Readable(slot, sizeof(void*)) || !*slot || !Readable(*slot, 0x80)) break;
        const FProperty* inner = static_cast<const FProperty*>(*slot);
        if (PropertyType(inner) == "StructProperty") stride = PropertySize(inner);
    }
    if (stride < kMinAssetDataSize || stride > kMaxAssetDataSize || (stride & 7)) { HSM_LOG("maps: asset registry layout not as expected (element size %d)", stride); return false; }
    struct TopLevelAssetPath { FName package, asset; };
    pc.put(0, TopLevelAssetPath{MakeName("/Script/Engine"), MakeName("World")});
    pc.put(1, false);
    if (!pc.invoke(reg)) { HSM_LOG("maps: GetAssetsByClass was refused"); return false; }
    const FByteArray arr = pc.take<FByteArray>(2);
    if (arr.num < 0 || arr.num > kMaxRegistryMaps) { HSM_LOG("maps: asset registry returned %d maps; ignored", arr.num); return false; }
    if (arr.num == 0) return true;
    if (!arr.data || !Readable(arr.data, size_t(arr.num) * size_t(stride))) { HSM_LOG("maps: asset registry result unreadable"); return false; }
    for (int32_t i = 0; i < arr.num; ++i) {
        const uint8_t* e = arr.data + size_t(i) * size_t(stride);
        FName pkg, asset;
        std::memcpy(&pkg, e, sizeof pkg); std::memcpy(&asset, e + 16, sizeof asset);
        const std::string pn = NameToString(pkg), an = NameToString(asset);
        if (i == 0 && (pn.empty() || pn[0] != '/' || LastSegmentOf(pn) != an)) { HSM_LOG("maps: asset registry entry layout not as expected (%s / %s)", pn.c_str(), an.c_str()); return false; }
        if (!pn.empty() && pn[0] == '/' && LastSegmentOf(pn) == an) out.push_back(pn);
    }
    return true;
}

void Rebuild() {
    g.maps = BuildModMapList(g.pakMaps, g.registryMaps);
    for (const ModMapEntry& e : g.maps) {
        if (!g.loggedMaps.insert(e.path).second) continue;
        const std::string pak = e.pak >= 0 && size_t(e.pak) < g.paks.size() ? g.paks[size_t(e.pak)].name : std::string("?");
        HSM_LOG("maps: custom map %s (%s)", e.path.c_str(), pak.c_str());
    }
    g.dirty = false;
}

}  // namespace

void Tick() {
    if (!g.started) { g.started = true; g.registryAt = GetTickCount() + kRegistryDelayMs; StartWorker(); }
    {
        std::lock_guard<std::mutex> lock(g_shared.mu);
        if (g_shared.version != g.seenVersion) {
            g.seenVersion = g_shared.version;
            g.paks = g_shared.paks; g.pakMaps = g_shared.pakMaps; g.packs = g_shared.packs; g.kinds = g_shared.kinds;
            g.listed = g_shared.listed; g.allHashed = g_shared.allHashed;
            g.dirty = true;
        }
    }
    if (!g.registryDone && GetTickCount() - g.registryAt < 0x80000000u) {
        g.registryDone = true;
        std::vector<std::string> maps;
        if (QueryRegistry(maps)) {
            g.registryMaps = maps; g.registryTried = true;
            HSM_LOG("maps: the asset registry lists %zu map(s)", maps.size());
            g.dirty = true;
        }
    }
    if (g.dirty) Rebuild();
}

void Rescan() {
    g.registryDone = false; g.registryAt = GetTickCount();
    g.started = true;
    StartWorker();
}

const std::vector<ModMapEntry>& Maps() { return g.maps; }
std::vector<std::string> Unchecked() { std::lock_guard<std::mutex> lock(g_uncheckedMu); return g_unchecked; }
const std::vector<ModPak>& Paks() { return g.paks; }
const std::vector<LocalPack>& Packs() { return g.packs; }
const std::vector<ModKind>& PakKinds() { return g.kinds; }
uint32_t ScanVersion() { return g.seenVersion; }
bool Listed() { return g.listed; }
bool AllHashed() { return g.allHashed; }

bool PickPending(const std::string& pick) {
    if (pick.empty()) return false;
    return MakeHostModMap(pick, g.maps, g.paks).state == HostModMap::HashPending;
}
HostModMapResult HostMessage(const std::string& pick) { return MakeHostModMap(pick, g.maps, g.paks); }
ModMapVerdict Decide(const ModMapMsg& host) { return DecideModMap(host, g.paks, g.maps, g.listed && g.allHashed && g.registryDone); }

}  // namespace hsm::game::modmaps
