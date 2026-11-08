// Modded maps (HSMMod 0.8.0-pre2, protocol rev 27): a custom map installed as a .pak in Content/Paks (or ~mods and its
// subfolders) is listed in the host's map picker, opened by its package path like a stock map, and every player must
// have the same pak. Nothing here sends or loads a pak: the host announces the map and the pak it comes from (name, size,
// SHA-256), each client compares that with the paks it has, and a client that lacks it is told what to install and leaves.
// Game-independent, unit-tested; the client's scan and the engine calls are in gameplay/mod_maps.cpp.
//
//   stock filter     which paks and which map packages belong to the game itself (never listed as modded)
//   catalogue        the custom maps: those the engine's asset registry reports (no attribution) and those read out of
//                    each mod pak's own index (session/hsm_pak_index.h; attributed to that pak), minus stock, minus a
//                    pak's sublevels (lighting, details ...), capped
//   hash cache       path + size + mtime -> SHA-256, so a big pak is hashed once, not at every start
//   compatibility    the decision a joining client makes about the host's ModMap, and the line it tells the player
//   wire             ModMap (host -> client, reliable): the chosen map's path and display name, and its pak's name, size, hash
//   travel gate      a Travel to the announced map's path is followed only after the pak check passed
#pragma once
#include "hsm_protocol.h"
#include "match/hsm_maps.h"
#include "session/hsm_pak_index.h"
#include "session/hsm_stock_maps.h"
#include "session/hsm_travel_gate.h"
#include "util/hsm_sha256.h"
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hsm {

inline constexpr size_t kMaxModPakName = 96;        // a pak's file name on the wire and in the cache
inline constexpr size_t kMaxModMapPath = 200;       // a package path on the wire
inline constexpr size_t kMaxModMapDisplay = 40;     // the picker label
inline constexpr size_t kMaxModPaks = 64;           // mod paks scanned (the rest are ignored, logged once)
inline constexpr size_t kMaxModMaps = 32;           // custom maps offered in the picker
inline constexpr uint64_t kMaxModPakBytes = 1ull << 36;   // 64 GiB: a bigger size on the wire is nonsense
inline constexpr size_t kMaxHashCacheEntries = 256;
inline constexpr size_t kMaxHashCacheLine = 700;

namespace modmaps_detail {
inline char Low(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }
inline bool EqNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (Low(a[i]) != Low(b[i])) return false;
    return true;
}
inline bool PrintableAscii(std::string_view s) {
    for (char c : s) { const unsigned char u = static_cast<unsigned char>(c); if (u < 0x20 || u >= 0x7F) return false; }
    return true;
}
inline bool StartsWithNoCase(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && EqNoCase(s.substr(0, prefix.size()), prefix);
}
inline std::string_view LastSegment(std::string_view p) { const size_t k = p.rfind('/'); return k == std::string_view::npos ? p : p.substr(k + 1); }
}  // namespace modmaps_detail

// ------------------------------------------------------------------ names and paths

// A package path a modded map may have: "/Game/Maps/Foo/Bar", letters, digits, '_' '-' '/' only, no "//", not under the
// engine, scripts or the transient package, a last segment that is a name. This is what is shown, sent, and opened.
inline bool ModMapPathOk(std::string_view p) {
    if (p.size() < 3 || p.size() > kMaxModMapPath || p[0] != '/') return false;
    for (char c : p) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '/';
        if (!ok) return false;
    }
    if (p.find("//") != std::string_view::npos || p.back() == '/') return false;
    using modmaps_detail::StartsWithNoCase;
    for (const char* bad : {"/Engine/", "/Script/", "/Temp/", "/Memory/"}) if (StartsWithNoCase(p, bad)) return false;
    return p.find('/', 1) != std::string_view::npos;   // at least a root and a name
}

// A pak file name (no folders): printable ASCII, no path or wildcard characters, ends in ".pak", no leading dot.
inline bool ModPakNameOk(std::string_view n) {
    if (n.size() < 5 || n.size() > kMaxModPakName || n[0] == '.') return false;
    if (!modmaps_detail::PrintableAscii(n)) return false;
    for (char c : n) if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
    if (n.find("..") != std::string_view::npos) return false;
    return modmaps_detail::EqNoCase(n.substr(n.size() - 4), ".pak");
}

// A name read from the file system, made safe to show and send: printable ASCII only (anything else becomes '_'), cut to
// the cap. Compatibility never depends on the name (the hash decides), so a lossy name only costs a prettier message.
inline std::string SanitizePakName(std::string_view n) {
    std::string s;
    for (char c : n) {
        const unsigned char u = static_cast<unsigned char>(c);
        const bool bad = u < 0x20 || u >= 0x7F || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
        s += bad ? '_' : c;
    }
    // Whatever it's called, the result has to pass ModPakNameOk: the game loads a pak by any name, so a name mustn't be a way off the list.
    // A tester's 800 MB pak hid behind a long name with control characters.
    const bool isPak = s.size() >= 4 && modmaps_detail::EqNoCase(std::string_view(s).substr(s.size() - 4), ".pak");
    if (isPak && s.size() > kMaxModPakName) s = s.substr(0, kMaxModPakName - 4) + ".pak";
    else if (s.size() > kMaxModPakName) s.resize(kMaxModPakName);
    for (size_t at; (at = s.find("..")) != std::string::npos;) s[at] = '_';
    if (!s.empty() && s[0] == '.') s[0] = '_';
    return s;
}

// The game's own paks, in the Paks folder itself: pakchunk0-Windows.pak (the one legacy pak), its optional pak and
// patches (pakchunk0optional-Windows.pak, pakchunk0-Windows_0_P.pak), and HalfswordUE5-Windows*.pak. A pak in a
// subfolder (~mods, anything) is never stock, and neither is a pak of another chunk number: a modder's own pak.
inline bool IsStockPakName(std::string_view name, bool inPaksRoot) {
    if (!inPaksRoot) return false;
    using modmaps_detail::StartsWithNoCase;
    std::string_view rest;
    if (StartsWithNoCase(name, "pakchunk0optional-Windows")) rest = name.substr(25);
    else if (StartsWithNoCase(name, "pakchunk0-Windows")) rest = name.substr(17);
    else if (StartsWithNoCase(name, "HalfswordUE5-Windows")) rest = name.substr(20);
    else return false;
    if (modmaps_detail::EqNoCase(rest, ".pak")) return true;
    // A numbered patch: _<digits>_P.pak
    if (rest.size() < 7 || rest[0] != '_') return false;
    size_t i = 1;
    while (i < rest.size() && rest[i] >= '0' && rest[i] <= '9') ++i;
    return i > 1 && modmaps_detail::EqNoCase(rest.substr(i), "_P.pak");
}

// Unreal's IoStore companions of a pak (.utoc / .ucas) are not paks to list or hash.
inline bool IsIoStoreCompanion(std::string_view name) { return EndsWithNoCase(name, ".utoc") || EndsWithNoCase(name, ".ucas"); }

inline bool IsStockMapPath(std::string_view p) {
    for (size_t i = 0; i < kStockMapPathCount; ++i) if (modmaps_detail::EqNoCase(kStockMapPaths[i], p)) return true;
    return false;
}

// A level that is part of another level, not a map to start on: lighting, detail, geometry ... layers and level instances.
inline bool LooksLikeSublevel(std::string_view path) {
    using modmaps_detail::EqNoCase;
    std::string lower;
    for (char c : path) lower += modmaps_detail::Low(c);
    if (lower.find("/levelinstances/") != std::string::npos || lower.find("/lighting/") != std::string::npos) return true;
    const std::string_view name = modmaps_detail::LastSegment(path);
    for (const char* suffix : {"Lighting", "Details", "Geometry", "PhysicsProps", "Props", "VFX", "Foliage", "Terrain", "Architecture", "Rocks", "Spikes", "Sky", "Audio"}) {
        const std::string_view s = suffix;
        if (name.size() >= s.size() && EqNoCase(name.substr(name.size() - s.size()), s) &&
            (name.size() == s.size() || name[name.size() - s.size() - 1] == '_'))
            return true;
    }
    return false;
}

// "/Game/Maps/Custom/My_Cool-Map" -> "My Cool Map": the picker label.
inline std::string ModMapDisplayName(std::string_view path) {
    std::string s;
    for (char c : modmaps_detail::LastSegment(path)) s += (c == '_' || c == '-') ? ' ' : c;
    if (s.size() > kMaxModMapDisplay) s.resize(kMaxModMapDisplay);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s.empty() ? std::string("Modded map") : s;
}

// ------------------------------------------------------------------ the catalogue

struct ModPak {
    std::string name;            // file name only, sanitized
    uint64_t size = 0;
    bool hashed = false;         // sha is valid
    Sha256Digest sha{};
};
struct ModMapEntry {
    std::string path;            // package path, ModMapPathOk
    std::string display;
    int pak = -1;                // index into the pak list the entry was built against; -1 when the pak is unknown
};
struct PakMaps { size_t pak = 0; std::vector<std::string> maps; };   // the .umap packages one pak's index lists

// The custom maps: every map of a mod pak's index and every map the asset registry reports, minus the stock ones, minus
// a pak's sublevels (when the pak has at least one other map; a registry-only sublevel is dropped), de-duplicated by
// path (the pak's attribution wins), sorted by label, at most kMaxModMaps.
inline std::vector<ModMapEntry> BuildModMapList(const std::vector<PakMaps>& fromPaks, const std::vector<std::string>& fromRegistry) {
    std::vector<ModMapEntry> out;
    auto have = [&](std::string_view p) { for (const ModMapEntry& e : out) if (modmaps_detail::EqNoCase(e.path, p)) return true; return false; };
    for (const PakMaps& pm : fromPaks) {
        std::vector<const std::string*> usable;
        bool anyMain = false;
        for (const std::string& m : pm.maps) {
            if (!ModMapPathOk(m) || IsStockMapPath(m)) continue;
            usable.push_back(&m);
            if (!LooksLikeSublevel(m)) anyMain = true;
        }
        for (const std::string* m : usable) {
            if (anyMain && LooksLikeSublevel(*m)) continue;
            if (!have(*m)) out.push_back({*m, ModMapDisplayName(*m), int(pm.pak)});
        }
    }
    for (const std::string& m : fromRegistry) {
        if (!ModMapPathOk(m) || IsStockMapPath(m) || LooksLikeSublevel(m) || have(m)) continue;
        out.push_back({m, ModMapDisplayName(m), -1});
    }
    std::sort(out.begin(), out.end(), [](const ModMapEntry& a, const ModMapEntry& b) {
        if (a.display != b.display) return a.display < b.display;
        return a.path < b.path;
    });
    if (out.size() > kMaxModMaps) out.resize(kMaxModMaps);
    return out;
}

inline const ModMapEntry* FindModMap(const std::vector<ModMapEntry>& list, std::string_view pathOrObject) {
    const std::string_view p = map_detail::StripObjectName(pathOrObject);
    for (const ModMapEntry& e : list) if (modmaps_detail::EqNoCase(e.path, p)) return &e;
    return nullptr;
}

// ------------------------------------------------------------------ hash cache

// "<size>:<mtime>:<lowercase path>": two files with the same path, size and modification time are the same file.
inline std::string HashCacheKey(std::string_view path, uint64_t size, uint64_t mtime) {
    std::string k = std::to_string(size) + ":" + std::to_string(mtime) + ":";
    for (char c : path) k += c == '\\' ? '/' : modmaps_detail::Low(c);
    return k;
}

// The cache file is text, one entry per line: "v1<TAB>sha256 hex<TAB>size<TAB>mtime<TAB>path". Parsing is lenient (a bad
// line is skipped) and bounded; at most kMaxHashCacheEntries are kept, the oldest dropped first.
class HashCache {
public:
    std::optional<Sha256Digest> Lookup(std::string_view path, uint64_t size, uint64_t mtime) const {
        const std::string k = HashCacheKey(path, size, mtime);
        for (const Entry& e : entries_) if (e.key == k) return e.sha;
        return std::nullopt;
    }
    void Put(std::string_view path, uint64_t size, uint64_t mtime, const Sha256Digest& sha) {
        const std::string k = HashCacheKey(path, size, mtime);
        // The same file under an older size or time is stale: drop it.
        std::string prefix;
        for (char c : path) prefix += c == '\\' ? '/' : modmaps_detail::Low(c);
        std::erase_if(entries_, [&](const Entry& e) { return e.key == k || e.path == prefix; });
        entries_.push_back({k, prefix, size, mtime, sha});
        if (entries_.size() > kMaxHashCacheEntries) entries_.erase(entries_.begin());
    }
    size_t size() const { return entries_.size(); }
    std::string Serialize() const {
        std::string s;
        for (const Entry& e : entries_)
            s += "v1\t" + Sha256Hex(e.sha) + "\t" + std::to_string(e.size) + "\t" + std::to_string(e.mtime) + "\t" + e.path + "\n";
        return s;
    }
    void Parse(std::string_view text) {
        entries_.clear();
        size_t pos = 0, lines = 0;
        while (pos < text.size() && lines < kMaxHashCacheEntries * 2) {
            size_t end = text.find('\n', pos);
            if (end == std::string_view::npos) end = text.size();
            std::string_view line = text.substr(pos, end - pos);
            pos = end + 1; ++lines;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty() || line.size() > kMaxHashCacheLine) continue;
            std::string_view f[5]; size_t nf = 0, start = 0;
            for (size_t i = 0; i <= line.size() && nf < 5; ++i)
                if (i == line.size() || line[i] == '\t') { f[nf++] = line.substr(start, i - start); start = i + 1; }
            if (nf != 5 || f[0] != "v1" || f[4].empty() || !modmaps_detail::PrintableAscii(f[4])) continue;
            Sha256Digest sha{};
            uint64_t size = 0, mtime = 0;
            if (!Sha256FromHex(std::string(f[1]), sha) || !Num(f[2], size) || !Num(f[3], mtime)) continue;
            Put(f[4], size, mtime, sha);
        }
    }
private:
    struct Entry { std::string key, path; uint64_t size, mtime; Sha256Digest sha; };
    static bool Num(std::string_view s, uint64_t& v) {
        if (s.empty() || s.size() > 20) return false;
        v = 0;
        for (char c : s) { if (c < '0' || c > '9') return false; const uint64_t nv = v * 10 + uint64_t(c - '0'); if (nv < v) return false; v = nv; }
        return true;
    }
    std::vector<Entry> entries_;
};

// ------------------------------------------------------------------ wire

// ModMap (rev 27, host -> client, reliable): the map the host has picked, when it is a modded one, and the pak it comes
// from. An empty path says "the pick is a stock map" (a client drops what it approved). pakName empty with a path: the
// host could not tell which pak holds the map (the client then needs the map itself, by path).
// Wire: 4 header, str path, str display, str pakName, u64 pakSize, 32 bytes sha256.
struct ModMapMsg {
    std::string path;
    std::string display;
    std::string pakName;
    uint64_t pakSize = 0;
    Sha256Digest sha{};
};

inline std::vector<uint8_t> Encode(const ModMapMsg& m) {
    Writer w; w.header(MsgType::ModMap);
    w.str(m.path, kMaxModMapPath); w.str(m.display, kMaxModMapDisplay); w.str(m.pakName, kMaxModPakName);
    w.u64(m.pakSize);
    w.buf.insert(w.buf.end(), m.sha.begin(), m.sha.end());
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, ModMapMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::ModMap) return false;
    m = ModMapMsg{};
    m.path = r.str(kMaxModMapPath); m.display = r.str(kMaxModMapDisplay); m.pakName = r.str(kMaxModPakName);
    m.pakSize = r.u64();
    for (uint8_t& b : m.sha) b = r.u8();
    if (!r.done()) return false;
    if (m.path.empty()) return m.display.empty() && m.pakName.empty() && m.pakSize == 0 && Sha256IsZero(m.sha);
    if (!ModMapPathOk(m.path) || m.display.empty() || !modmaps_detail::PrintableAscii(m.display)) return false;
    if (m.pakName.empty()) return m.pakSize == 0 && Sha256IsZero(m.sha);
    return ModPakNameOk(m.pakName) && m.pakSize >= 1 && m.pakSize <= kMaxModPakBytes && !Sha256IsZero(m.sha);
}

// Host: the ModMap for a picked map. nullopt while the pak's hash is still being computed (try again later). A pick that
// is not a custom map in the list gives the "stock" message (empty path).
enum class HostModMap : uint8_t { Stock, Ready, HashPending };
struct HostModMapResult { HostModMap state = HostModMap::Stock; ModMapMsg msg; };
inline HostModMapResult MakeHostModMap(std::string_view pick, const std::vector<ModMapEntry>& maps, const std::vector<ModPak>& paks) {
    HostModMapResult r;
    const ModMapEntry* e = pick.empty() ? nullptr : FindModMap(maps, pick);
    if (!e) return r;
    r.msg.path = e->path; r.msg.display = e->display;
    if (e->pak >= 0 && size_t(e->pak) < paks.size()) {
        const ModPak& p = paks[size_t(e->pak)];
        if (!p.hashed) { r.state = HostModMap::HashPending; return r; }
        r.msg.pakName = p.name; r.msg.pakSize = p.size; r.msg.sha = p.sha;
    }
    r.state = HostModMap::Ready;
    return r;
}

// ------------------------------------------------------------------ compatibility

enum class ModMapVerdict : uint8_t {
    NoMap,          // the host picked a stock map: nothing to check
    Ok,             // this machine has the pak (same SHA-256) / the map
    Pending,        // the scan or a hash is still running: ask again
    MissingPak,     // no pak like the host's
    DifferentPak,   // a pak of the same name but not the same bytes
    MissingMap,     // the host could not name the pak and this machine does not have the map either
};

// listed: the file scan is over (every pak is at least listed). A pak whose hash is still pending can only be a match when
// its size equals the host's; the verdict waits for it (Pending) rather than telling the player to install what they have.
inline ModMapVerdict DecideModMap(const ModMapMsg& host, const std::vector<ModPak>& local, const std::vector<ModMapEntry>& localMaps, bool listed) {
    if (host.path.empty()) return ModMapVerdict::NoMap;
    if (host.pakName.empty()) {
        if (FindModMap(localMaps, host.path)) return ModMapVerdict::Ok;
        return listed ? ModMapVerdict::MissingMap : ModMapVerdict::Pending;
    }
    bool waiting = false, sameName = false;
    for (const ModPak& p : local) {
        if (p.hashed) { if (p.size == host.pakSize && p.sha == host.sha) return ModMapVerdict::Ok; }
        else if (p.size == host.pakSize) waiting = true;
        if (modmaps_detail::EqNoCase(p.name, host.pakName)) sameName = true;
    }
    if (waiting || !listed) return ModMapVerdict::Pending;
    return sameName ? ModMapVerdict::DifferentPak : ModMapVerdict::MissingPak;
}

// What the player is told when they cannot join. Empty for the verdicts that need no line.
inline std::string ModMapJoinLine(const ModMapMsg& host, ModMapVerdict v) {
    const std::string map = host.display.empty() ? ModMapDisplayName(host.path) : host.display;
    switch (v) {
        case ModMapVerdict::MissingPak:
            return "This lobby uses the modded map " + map + " from " + host.pakName + ". Install that pak in Content/Paks/~mods to join.";
        case ModMapVerdict::DifferentPak:
            return "This lobby uses the modded map " + map + " from " + host.pakName + ", but your " + host.pakName +
                   " is a different version. Install the host's version in Content/Paks/~mods to join.";
        case ModMapVerdict::MissingMap:
            return "This lobby uses the modded map " + map + ". Install the pak that has it in Content/Paks/~mods to join.";
        default: return {};
    }
}

// ------------------------------------------------------------------ travel gate

// A client follows a Travel to a catalogue/menu level (TravelAllowed), or to the modded map the host announced and this
// machine verified (approvedPath empty = none): the same path, with or without the ".Name" object suffix, no options.
inline bool TravelAllowedWithModMap(std::string_view level, std::string_view options, std::string_view approvedPath) {
    if (TravelAllowed(level, options)) return true;
    if (approvedPath.empty() || !TravelOptionsAllowed(options) || level.empty() || level.size() > kMaxLevelName) return false;
    for (char c : level) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u >= 0x7F || c == ':' || c == '?' || c == '\\' || c == ' ') return false;
    }
    if (level.find("//") != std::string_view::npos || level.find("..") != std::string_view::npos) return false;
    return ModMapPathOk(approvedPath) && modmaps_detail::EqNoCase(map_detail::StripObjectName(level), approvedPath);
}

}  // namespace hsm
