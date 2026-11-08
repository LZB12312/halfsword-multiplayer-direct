// Reads the list of map files out of an unencrypted UE 5.4 .pak (pak version 10 or 11, the "path hash + full directory
// index" layout), from three byte ranges the caller reads from the file: the footer, the primary index and the full
// directory index. Pure, bounded and allocation-capped: every offset, count and string length is checked against the
// bytes it was given, so a hostile or corrupt file yields an error, never a read past the buffers.
//
//   footer   the last bytes of the file:  [guid 16][encrypted 1][magic u32][version u32][index offset i64][index size i64]
//            [index hash 20][compression names 5 x 32]
//   primary  FString mount point, i32 entry count, u64 path-hash seed, i32 hasPathHashIndex (+ i64 offset, i64 size,
//            20-byte hash), i32 hasFullDirectoryIndex (+ i64 offset, i64 size, 20-byte hash), the encoded entries
//   full directory index   i32 dirCount, per dir: FString name, i32 fileCount, per file: FString name, i32 entry offset
//
// Versions 3 to 9 (UE 4.x tools such as repak or an old UnrealPak; the engine still mounts them) have the legacy index
// instead: FString mount point, i32 entry count, then per entry FString file name + the serialized FPakEntry
// (ParsePakLegacyIndex). A frozen v9 index is refused.
//
// An encrypted index (the stock game's own pak), a pak older than version 3 and an IoStore container are reported, not
// parsed; the caller then lists that pak's maps without attribution. Only the .umap names are kept: a package path
// (/Game/Maps/Foo) for each, which is what OpenLevel takes.
#pragma once
#include "session/hsm_stock_packages.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hsm {

inline constexpr uint32_t kPakMagic = 0x5A6F12E1u;
inline constexpr size_t kPakFooterReadBytes = 512;                 // the tail the caller reads (the v11 footer is 221 bytes)
inline constexpr uint64_t kMaxPakIndexBytes = 32ull * 1024 * 1024;   // primary or directory index: bigger is refused, not read
inline constexpr size_t kMaxPakMaps = 256;                         // .umap entries kept per pak
inline constexpr size_t kMaxPakPathChars = 1024;                   // one FString in an index
inline constexpr const char* kGameProjectDir = "HalfswordUE5";     // the project folder in the pak's paths (../../../HalfswordUE5/Content/...)

enum class PakParse : uint8_t { Ok, NotPak, Unsupported, Encrypted, Malformed, TooBig };

inline const char* PakParseName(PakParse p) {
    switch (p) {
        case PakParse::Ok: return "ok";
        case PakParse::NotPak: return "not a pak";
        case PakParse::Unsupported: return "pak version not supported";
        case PakParse::Encrypted: return "encrypted index";
        case PakParse::Malformed: return "malformed index";
        case PakParse::TooBig: return "index too big";
    }
    return "?";
}

struct PakFooter { uint32_t version = 0; bool encryptedIndex = false; uint64_t indexOffset = 0, indexSize = 0; };
struct PakPrimary { std::string mountPoint; uint32_t numEntries = 0; bool hasFullDir = false; uint64_t fdiOffset = 0, fdiSize = 0; };

namespace pak_detail {
class Cursor {
public:
    Cursor(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    bool ok() const { return ok_; }
    size_t left() const { return ok_ ? n_ - p_ : 0; }
    uint8_t u8() { return need(1) ? d_[p_++] : uint8_t(0); }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v = 0; for (int i = 3; i >= 0; --i) v = v << 8 | d_[p_ + size_t(i)]; p_ += 4; return v; }
    uint64_t u64() { const uint64_t lo = u32(); const uint64_t hi = u32(); return hi << 32 | lo; }
    int32_t i32() { return int32_t(u32()); }
    void skip(size_t k) { if (need(k)) p_ += k; }
    // UE FString: i32 length (characters, including the NUL); negative = UTF-16. Non-ASCII characters become '?'.
    std::string fstring() {
        const int32_t len = i32();
        if (!ok_) return {};
        if (len == 0) return {};
        const bool wide = len < 0;
        const int64_t chars = wide ? -int64_t(len) : int64_t(len);
        if (chars > int64_t(kMaxPakPathChars) || !need(size_t(chars) * (wide ? 2 : 1))) { ok_ = false; return {}; }
        std::string s;
        for (int64_t i = 0; i < chars; ++i) {
            const unsigned c = wide ? unsigned(d_[p_ + size_t(i) * 2]) | unsigned(d_[p_ + size_t(i) * 2 + 1]) << 8 : unsigned(d_[p_ + size_t(i)]);
            if (i == chars - 1) { if (c != 0) ok_ = false; }          // the terminator
            else s += (c >= 0x20 && c < 0x7F) ? char(c) : '?';
        }
        p_ += size_t(chars) * (wide ? 2 : 1);
        return ok_ ? s : std::string();
    }
private:
    bool need(size_t k) { if (!ok_ || n_ - p_ < k) { ok_ = false; return false; } return true; }
    const uint8_t* d_; size_t n_; size_t p_ = 0; bool ok_ = true;
};
}  // namespace pak_detail

// tail: the last min(fileSize, kPakFooterReadBytes) bytes of the file.
inline PakParse ParsePakFooter(const uint8_t* tail, size_t n, uint64_t fileSize, PakFooter& out) {
    out = PakFooter{};
    if (!tail || n < 40 || fileSize < n) return PakParse::NotPak;
    // The magic sits 204 bytes before the end in v10/v11 (a 221 byte footer: 17 bytes of guid and flag before it). Scan the
    // last 300 bytes rather than trust one layout; the flag byte before the magic must exist (p >= 1).
    const size_t last = n - 24;                      // magic + version + offset + size must fit
    const size_t from = n > 300 ? n - 300 : 1;
    for (size_t p = last; p >= from; --p) {
        pak_detail::Cursor c(tail + p, n - p);
        if (c.u32() != kPakMagic) continue;
        const uint32_t version = c.u32();
        if (version < 1 || version > 32) continue;
        out.version = version;
        if (version < 3 || version > 11) return PakParse::Unsupported;
        const uint64_t off = c.u64(), size = c.u64();
        if (!c.ok()) return PakParse::Malformed;
        out.encryptedIndex = version >= 4 && tail[p - 1] != 0;     // the flag byte exists from IndexEncryption (4)
        if (version == 9) {                                         // FrozenIndex: a bool after the 20-byte index hash
            c.skip(20);
            const uint8_t frozen = c.u8();
            if (!c.ok()) return PakParse::Malformed;
            if (frozen != 0) return PakParse::Unsupported;
        }
        out.indexOffset = off; out.indexSize = size;
        if (out.encryptedIndex) return PakParse::Encrypted;
        if (size == 0 || off > fileSize || size > fileSize - off) return PakParse::Malformed;
        if (size > kMaxPakIndexBytes) return PakParse::TooBig;
        return PakParse::Ok;
    }
    return PakParse::NotPak;
}

inline PakParse ParsePakPrimaryIndex(const uint8_t* d, size_t n, uint64_t fileSize, PakPrimary& out) {
    out = PakPrimary{};
    pak_detail::Cursor c(d, n);
    out.mountPoint = c.fstring();
    out.numEntries = c.u32();
    (void)c.u64();                                   // path hash seed
    if (c.i32() != 0) { c.skip(8 + 8 + 20); }        // the path hash index: not needed
    const int32_t hasFull = c.i32();
    if (!c.ok()) return PakParse::Malformed;
    if (hasFull == 1) {
        out.fdiOffset = c.u64(); out.fdiSize = c.u64(); c.skip(20);
        if (!c.ok()) return PakParse::Malformed;
        out.hasFullDir = true;
        if (out.fdiSize == 0 || out.fdiOffset > fileSize || out.fdiSize > fileSize - out.fdiOffset) return PakParse::Malformed;
        if (out.fdiSize > kMaxPakIndexBytes) return PakParse::TooBig;
    } else if (hasFull != 0) {
        return PakParse::Malformed;
    }
    return out.hasFullDir ? PakParse::Ok : PakParse::Unsupported;
}

inline bool PakNameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '+';
}

inline bool EndsWithNoCase(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    for (size_t i = 0; i < suffix.size(); ++i) {
        char a = s[s.size() - suffix.size() + i], b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = char(a + 32);
        if (b >= 'A' && b <= 'Z') b = char(b + 32);
        if (a != b) return false;
    }
    return true;
}

// "../../../HalfswordUE5/Content/Maps/Foo/Bar.umap" -> "/Game/Maps/Foo/Bar"; Engine/Content -> /Engine; a plugin's
// Content (HalfswordUE5/Plugins/<Name>/Content/...) -> /<Name>/.... Empty when the file is not a .umap under one of those
// roots or any segment has a character a package path cannot carry (spaces and dots included: such a name is refused).
// `anyName`: any characters. Such a path is still a real file of the pak, it just can't be opened by name.
inline std::string PakFileToPackagePath(std::string_view file, bool anyName = false) {
    if (!EndsWithNoCase(file, ".umap")) return {};
    file.remove_suffix(5);
    std::vector<std::string> seg;
    std::string cur;
    for (char ch : file) {
        const char c = ch == '\\' ? '/' : ch;
        if (c == '/') { if (!cur.empty()) seg.push_back(cur); cur.clear(); } else cur += c;
    }
    if (!cur.empty()) seg.push_back(cur);
    size_t i = 0;
    while (i < seg.size() && (seg[i] == ".." || seg[i] == ".")) ++i;
    seg.erase(seg.begin(), seg.begin() + long(i));
    for (const std::string& s : seg) {
        if (s.empty() || s == "..") return {};
        if (!anyName) for (char c : s) if (!PakNameChar(c)) return {};
    }
    if (seg.size() < 3) return {};
    std::string root;
    size_t first = 0;
    if (seg[0] == "Engine" && seg[1] == "Content") { root = "/Engine"; first = 2; }
    else if (seg[0] == kGameProjectDir && seg[1] == "Content") { root = "/Game"; first = 2; }
    else if (seg[0] == kGameProjectDir && seg[1] == "Plugins") {
        for (size_t k = 3; k < seg.size(); ++k)
            if (seg[k] == "Content") { root = "/" + seg[k - 1]; first = k + 1; break; }
    }
    if (root.empty() || first >= seg.size()) return {};
    std::string out = root;
    for (size_t k = first; k < seg.size(); ++k) out += "/" + seg[k];
    return out;
}

// What a pak holds besides its maps, for the map/content split (session/hsm_mod_rules.h ClassifyPak): the package
// folders of its other package files (.uasset .uexp .ubulk .uptnl), deduplicated, at most kMaxPakAssetDirs, and which of them
// replace a stock package. overflow: more folders than that, a package file outside any content root, or an .ini anywhere (a
// config override). Shader archives, the asset registry and other cooked side files do not count.
inline constexpr size_t kMaxPakAssetDirs = 512;
inline constexpr size_t kMaxPakStockFiles = 8;      // named in the log; one is enough to decide
struct PakAssets {
    std::vector<std::string> dirs;
    std::vector<std::string> stockFiles;  // package paths this pak replaces, at most kMaxPakStockFiles
    bool overflow = false;
    const char* overflowWhy = "";         // what set overflow, for the log
    size_t badMaps = 0;                   // .umap files whose path cannot be a package path: not in the maps list
    std::string badMapName;               // the first of them, for the log
};

namespace pak_detail {
inline void Overflow(PakAssets* a, const char* why) { a->overflow = true; a->overflowWhy = why; }

inline void NoteAssetDir(const std::string& dir, PakAssets* a) {
    for (const std::string& d : a->dirs) if (d == dir) return;
    if (a->dirs.size() >= kMaxPakAssetDirs) { Overflow(a, "too many asset folders"); return; }
    a->dirs.push_back(dir);
}

inline void NoteStockFile(const std::string& pkg, PakAssets* a) {
    if (!IsStockPackage(pkg) || a->stockFiles.size() >= kMaxPakStockFiles) return;
    for (const std::string& f : a->stockFiles) if (f == pkg) return;
    a->stockFiles.push_back(pkg);
}

inline void NoteAsset(const std::string& fullPath, std::string_view name, PakAssets* a) {
    if (!a || a->overflow) return;
    if (EndsWithNoCase(name, ".ini")) { Overflow(a, "a config file"); return; }
    if (!EndsWithNoCase(name, ".uasset") && !EndsWithNoCase(name, ".uexp") && !EndsWithNoCase(name, ".ubulk") && !EndsWithNoCase(name, ".uptnl")) return;
    const size_t cut = fullPath.find_last_of("/\\");
    const std::string pkg = PakFileToPackagePath((cut == std::string::npos ? std::string() : fullPath.substr(0, cut + 1)) + "x.umap", true);
    if (pkg.size() < 3) { Overflow(a, "a package file outside any content root"); return; }
    const std::string dir = pkg.substr(0, pkg.size() - 2);
    const std::string file = cut == std::string::npos ? fullPath : fullPath.substr(cut + 1);   // a legacy name is a whole path
    NoteStockFile(dir + "/" + file.substr(0, file.rfind('.')), a);
    NoteAssetDir(dir, a);
}

// A .umap that can't be a package path: a space or a dot in its path, or no content root at all.
inline void NoteBadMap(const std::string& fullPath, PakAssets* a) {
    if (!a || a->overflow) return;
    const std::string pkg = PakFileToPackagePath(fullPath, true);
    if (pkg.empty()) { Overflow(a, "a package file outside any content root"); return; }
    if (a->badMaps++ == 0) a->badMapName = pkg;
}
}  // namespace pak_detail

// d: the whole full directory index. maps gets the package path of each .umap (capped at kMaxPakMaps, deduplicated).
inline PakParse ParsePakDirectoryIndex(const uint8_t* d, size_t n, const std::string& mountPoint, std::vector<std::string>& maps, PakAssets* assets = nullptr) {
    maps.clear();
    if (assets) *assets = PakAssets{};
    pak_detail::Cursor c(d, n);
    const int32_t dirs = c.i32();
    if (!c.ok() || dirs < 0) return PakParse::Malformed;
    for (int32_t i = 0; i < dirs; ++i) {
        std::string dir = c.fstring();
        const int32_t files = c.i32();
        if (!c.ok() || files < 0) return PakParse::Malformed;
        if (dir == "/") dir.clear();
        for (int32_t k = 0; k < files; ++k) {
            const std::string name = c.fstring();
            (void)c.i32();                           // the encoded entry offset
            if (!c.ok()) return PakParse::Malformed;
            if (!EndsWithNoCase(name, ".umap")) { pak_detail::NoteAsset(mountPoint + dir + name, name, assets); continue; }
            if (maps.size() >= kMaxPakMaps) continue;
            const std::string pkg = PakFileToPackagePath(mountPoint + dir + name);
            if (pkg.empty()) { pak_detail::NoteBadMap(mountPoint + dir + name, assets); continue; }
            bool dup = false;
            for (const std::string& m : maps) if (m == pkg) dup = true;
            if (!dup) maps.push_back(pkg);
        }
    }
    return c.left() == 0 ? PakParse::Ok : PakParse::Malformed;
}

namespace pak_detail {
// One legacy FPakEntry (FPakEntry::Serialize before v10). v8 comes in two shapes (8A: a u8 compression index, 8B: u32),
// told apart by the caller trying both. False when the bytes cannot be such an entry.
inline bool SkipLegacyEntry(Cursor& c, uint32_t version, bool v8a) {
    c.skip(8 + 8 + 8);                               // offset, size, uncompressed size
    uint32_t method = 0;
    if (version == 8 && v8a) method = c.u8();
    else method = c.u32();                           // flags before v8, an index from v8
    if (version >= 8 && method > 4) return false;    // five compression name slots at most
    if (version <= 1) c.skip(8);                     // timestamp
    c.skip(20);                                      // hash
    if (version >= 3) {
        if (method != 0) {
            const int32_t blocks = c.i32();
            if (!c.ok() || blocks < 0 || size_t(blocks) > c.left() / 16) return false;
            c.skip(size_t(blocks) * 16);
        }
        c.skip(1 + 4);                               // flags, compression block size
    }
    return c.ok();
}

inline PakParse LegacyPass(const uint8_t* d, size_t n, uint32_t version, bool v8a, std::vector<std::string>& maps, PakAssets* assets) {
    maps.clear();
    if (assets) *assets = PakAssets{};
    Cursor c(d, n);
    const std::string mount = c.fstring();
    const int32_t entries = c.i32();
    if (!c.ok() || entries < 0) return PakParse::Malformed;
    for (int32_t i = 0; i < entries; ++i) {
        const std::string name = c.fstring();
        if (!c.ok() || !SkipLegacyEntry(c, version, v8a)) return PakParse::Malformed;
        if (!EndsWithNoCase(name, ".umap")) { NoteAsset(mount + name, name, assets); continue; }
        if (maps.size() >= kMaxPakMaps) continue;
        const std::string pkg = PakFileToPackagePath(mount + name);
        if (pkg.empty()) { NoteBadMap(mount + name, assets); continue; }
        bool dup = false;
        for (const std::string& m : maps) if (m == pkg) dup = true;
        if (!dup) maps.push_back(pkg);
    }
    return PakParse::Ok;                             // v8+ may have trailing bytes (a hash-less filler); entries are what count
}
}  // namespace pak_detail

// d: the whole legacy index (version 3..9). maps gets the package path of each .umap.
inline PakParse ParsePakLegacyIndex(const uint8_t* d, size_t n, uint32_t version, std::vector<std::string>& maps, PakAssets* assets = nullptr) {
    if (version < 3 || version > 9) return PakParse::Unsupported;
    PakParse r = pak_detail::LegacyPass(d, n, version, false, maps, assets);
    if (r != PakParse::Ok && version == 8) r = pak_detail::LegacyPass(d, n, version, true, maps, assets);
    return r;
}

}  // namespace hsm
