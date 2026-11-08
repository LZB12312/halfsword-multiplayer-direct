// Pak index reader (session/hsm_pak_index.h): a synthetic v11 pak (footer, primary index, full directory index) parses to
// its map packages; every truncation, wrong version, encrypted index and out-of-range offset is refused, never over-read.
#include "session/hsm_mod_rules.h"
#include "session/hsm_pak_index.h"
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

namespace {
struct Buf {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) b.push_back(uint8_t(v >> (8 * i))); }
    void zeros(size_t n) { b.insert(b.end(), n, 0); }
    void fstr(const std::string& s) { u32(uint32_t(s.size() + 1)); b.insert(b.end(), s.begin(), s.end()); u8(0); }
    void wstr(const std::string& s) { u32(uint32_t(-int32_t(s.size() + 1))); for (char c : s) { u8(uint8_t(c)); u8(0); } u8(0); u8(0); }
};
using Dirs = std::vector<std::pair<std::string, std::vector<std::string>>>;

struct Pak {
    std::vector<uint8_t> file;
    size_t primaryOff = 0, primarySize = 0, fdiOff = 0, fdiSize = 0;
};

Pak Build(const std::string& mount, const Dirs& dirs, uint32_t version = 11, bool encrypted = false, bool wide = false) {
    Pak p;
    Buf data; data.zeros(100);                                         // stand-in for the file data
    Buf fdi;
    fdi.u32(uint32_t(dirs.size()));
    for (const auto& d : dirs) {
        if (wide) fdi.wstr(d.first); else fdi.fstr(d.first);
        fdi.u32(uint32_t(d.second.size()));
        for (const std::string& f : d.second) { fdi.fstr(f); fdi.u32(0); }
    }
    auto primary = [&](uint64_t fdiOff) {
        Buf b; b.fstr(mount); b.u32(7); b.u64(0x1234);
        b.u32(0);                                                      // no path hash index
        b.u32(1); b.u64(fdiOff); b.u64(fdi.b.size()); b.zeros(20);     // full directory index
        b.u32(0); b.u32(0);                                            // encoded entries, non-encoded entries
        return b;
    };
    const size_t primarySize = primary(0).b.size();
    p.primaryOff = data.b.size(); p.primarySize = primarySize;
    p.fdiOff = p.primaryOff + primarySize; p.fdiSize = fdi.b.size();
    Buf f = data;
    const Buf pri = primary(p.fdiOff);
    f.b.insert(f.b.end(), pri.b.begin(), pri.b.end());
    f.b.insert(f.b.end(), fdi.b.begin(), fdi.b.end());
    f.zeros(16); f.u8(encrypted ? 1 : 0); f.u32(kPakMagic); f.u32(version);
    f.u64(p.primaryOff); f.u64(p.primarySize); f.zeros(20); f.zeros(160);
    p.file = f.b;
    return p;
}
std::vector<uint8_t> Tail(const Pak& p) {
    const size_t n = p.file.size() < kPakFooterReadBytes ? p.file.size() : kPakFooterReadBytes;
    return std::vector<uint8_t>(p.file.end() - long(n), p.file.end());
}
const Dirs kSample = {
    {"HalfswordUE5/Content/Maps/Custom/", {"Arena_Bog.umap", "Arena_Bog.uasset", "Arena_Bog_Lighting.umap", "Arena_Bog.uexp"}},
    {"HalfswordUE5/Content/Maps/Arenas/", {"Map_Arena_Yard.umap"}},
    {"HalfswordUE5/Content/Textures/", {"T_Bog.uasset"}},
};

// A legacy (v3..v9) pak: the index is the mount point, the entry count and per entry the file name + FPakEntry.
// v8a: the 4.22 shape (u8 compression index). compressed: every entry has 2 compression blocks.
Pak BuildLegacy(const std::string& mount, const std::vector<std::string>& files, uint32_t version, bool v8a = false,
                bool compressed = false, uint8_t frozen = 0) {
    Pak p;
    Buf data; data.zeros(100);
    Buf idx; idx.fstr(mount); idx.u32(uint32_t(files.size()));
    for (const std::string& f : files) {
        idx.fstr(f);
        idx.u64(0); idx.u64(10); idx.u64(10);
        const uint32_t method = compressed ? 1 : 0;
        if (version == 8 && v8a) idx.u8(uint8_t(method)); else idx.u32(method);
        if (version <= 1) idx.u64(0);
        idx.zeros(20);
        if (version >= 3) {
            if (compressed) { idx.u32(2); idx.zeros(32); }
            idx.u8(0); idx.u32(65536);
        }
    }
    p.primaryOff = data.b.size(); p.primarySize = idx.b.size();
    Buf f = data;
    f.b.insert(f.b.end(), idx.b.begin(), idx.b.end());
    f.zeros(16); f.u8(0); f.u32(kPakMagic); f.u32(version);
    f.u64(p.primaryOff); f.u64(p.primarySize); f.zeros(20); f.u8(frozen); f.zeros(160);
    p.file = f.b;
    return p;
}
const std::vector<std::string> kLegacyFiles = {"HalfswordUE5/Content/Maps/Airplane/Airplane.umap", "HalfswordUE5/Content/Maps/Airplane/Airplane.uexp",
                                               "HalfswordUE5/Content/Textures/T_Wing.uasset", "HalfswordUE5/Content/Maps/Airplane/Airplane_Sub.umap"};
}  // namespace

static void TestLegacy() {
    struct Case { uint32_t v; bool v8a, compressed; };
    for (const Case k : {Case{3, false, false}, Case{4, false, true}, Case{7, false, true}, Case{8, false, false}, Case{8, false, true},
                         Case{8, true, false}, Case{8, true, true}, Case{9, false, true}}) {
        const Pak p = BuildLegacy("../../../", kLegacyFiles, k.v, k.v8a, k.compressed);
        const std::vector<uint8_t> t = Tail(p);
        PakFooter f;
        CHECK(ParsePakFooter(t.data(), t.size(), p.file.size(), f) == PakParse::Ok && f.version == k.v);
        CHECK(f.indexOffset == p.primaryOff && f.indexSize == p.primarySize);
        std::vector<std::string> maps;
        CHECK(ParsePakLegacyIndex(p.file.data() + p.primaryOff, p.primarySize, k.v, maps) == PakParse::Ok);
        CHECK(maps.size() == 2 && maps[0] == "/Game/Maps/Airplane/Airplane" && maps[1] == "/Game/Maps/Airplane/Airplane_Sub");
        // Every truncation is refused, never over-read.
        for (size_t n = 0; n + 1 < p.primarySize; ++n) CHECK(ParsePakLegacyIndex(p.file.data() + p.primaryOff, n, k.v, maps) != PakParse::Ok);
    }
    // A frozen v9 index is not read.
    { const Pak p = BuildLegacy("../../../", kLegacyFiles, 9, false, false, 1); const std::vector<uint8_t> t = Tail(p); PakFooter f;
      CHECK(ParsePakFooter(t.data(), t.size(), p.file.size(), f) == PakParse::Unsupported && f.version == 9); }
    // The legacy reader refuses the newer layouts and a compression index past the five name slots.
    std::vector<std::string> maps;
    const Pak p = BuildLegacy("../../../", kLegacyFiles, 8);
    CHECK(ParsePakLegacyIndex(p.file.data() + p.primaryOff, p.primarySize, 10, maps) == PakParse::Unsupported);
    std::vector<uint8_t> bad(p.file.begin() + long(p.primaryOff), p.file.begin() + long(p.primaryOff + p.primarySize));
    const size_t method = 4 + 10 + 4 + 4 + 50 + 24;                    // mount FString, count, first name FString, 3 x i64
    bad[method] = 9;
    CHECK(ParsePakLegacyIndex(bad.data(), bad.size(), 8, maps) != PakParse::Ok);
}

static void TestPackagePaths() {
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Content/Maps/Foo/Bar.umap") == "/Game/Maps/Foo/Bar");
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Content/Maps/Foo/Bar.UMAP") == "/Game/Maps/Foo/Bar");
    CHECK(PakFileToPackagePath("HalfswordUE5/Content/Bar.umap") == "/Game/Bar");
    CHECK(PakFileToPackagePath("..\\..\\..\\HalfswordUE5\\Content\\Maps\\Bar.umap") == "/Game/Maps/Bar");
    CHECK(PakFileToPackagePath("../../../Engine/Content/Maps/Entry.umap") == "/Engine/Maps/Entry");
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Plugins/BogMod/Content/Maps/Bog.umap") == "/BogMod/Maps/Bog");
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Plugins/Gfx/Sub/Content/Maps/Bog.umap") == "/Sub/Maps/Bog");
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Content/Maps/Bar.uasset").empty());           // not a map
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Content/Maps/Bar.umap.bak").empty());
    CHECK(PakFileToPackagePath("../../../Other/Content/Maps/Bar.umap").empty());                    // another project
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Config/Bar.umap").empty());                   // not under Content
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Plugins/BogMod/Maps/Bar.umap").empty());      // no Content folder
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Content/Maps/Bar Baz.umap").empty());         // a space
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Content/Maps/Ba.r.umap").empty());            // a dot
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Content/../Bar.umap").empty());               // a dotdot after the root
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Content/.umap").empty());
    CHECK(PakFileToPackagePath("Bar.umap").empty() && PakFileToPackagePath("").empty() && PakFileToPackagePath(".umap").empty());
    CHECK(PakFileToPackagePath("../../../HalfswordUE5/Content").empty());
}

static void TestParse() {
    const Pak p = Build("../../../", kSample);
    const std::vector<uint8_t> tail = Tail(p);
    PakFooter f;
    CHECK(ParsePakFooter(tail.data(), tail.size(), p.file.size(), f) == PakParse::Ok);
    CHECK(f.version == 11 && !f.encryptedIndex && f.indexOffset == p.primaryOff && f.indexSize == p.primarySize);
    PakPrimary pr;
    CHECK(ParsePakPrimaryIndex(p.file.data() + f.indexOffset, size_t(f.indexSize), p.file.size(), pr) == PakParse::Ok);
    CHECK(pr.mountPoint == "../../../" && pr.numEntries == 7 && pr.hasFullDir && pr.fdiOffset == p.fdiOff && pr.fdiSize == p.fdiSize);
    std::vector<std::string> maps;
    CHECK(ParsePakDirectoryIndex(p.file.data() + pr.fdiOffset, size_t(pr.fdiSize), pr.mountPoint, maps) == PakParse::Ok);
    CHECK(maps.size() == 3);
    CHECK(maps.size() == 3 && maps[0] == "/Game/Maps/Custom/Arena_Bog" && maps[1] == "/Game/Maps/Custom/Arena_Bog_Lighting" && maps[2] == "/Game/Maps/Arenas/Map_Arena_Yard");
    // The other package files' folders, for the map/content split.
    PakAssets assets;
    CHECK(ParsePakDirectoryIndex(p.file.data() + pr.fdiOffset, size_t(pr.fdiSize), pr.mountPoint, maps, &assets) == PakParse::Ok && maps.size() == 3);
    CHECK(assets.dirs.size() == 2 && assets.dirs[0] == "/Game/Maps/Custom" && assets.dirs[1] == "/Game/Textures" && !assets.overflow);

    // Version 10 reads the same; the mount point may carry the project, the root directory is "/".
    const Pak p10 = Build("../../../HalfswordUE5/Content/", {{"/", {"Top.umap"}}, {"Maps/", {"A.umap", "A.umap"}}}, 10);
    const std::vector<uint8_t> t10 = Tail(p10);
    CHECK(ParsePakFooter(t10.data(), t10.size(), p10.file.size(), f) == PakParse::Ok && f.version == 10);
    CHECK(ParsePakPrimaryIndex(p10.file.data() + f.indexOffset, size_t(f.indexSize), p10.file.size(), pr) == PakParse::Ok);
    CHECK(ParsePakDirectoryIndex(p10.file.data() + pr.fdiOffset, size_t(pr.fdiSize), pr.mountPoint, maps) == PakParse::Ok);
    CHECK(maps.size() == 2 && maps[0] == "/Game/Top" && maps[1] == "/Game/Maps/A");   // duplicates collapse

    // UTF-16 directory names read the same.
    const Pak pw = Build("../../../", {{"HalfswordUE5/Content/Maps/", {"W.umap"}}}, 11, false, true);
    const std::vector<uint8_t> tw = Tail(pw);
    CHECK(ParsePakFooter(tw.data(), tw.size(), pw.file.size(), f) == PakParse::Ok);
    CHECK(ParsePakPrimaryIndex(pw.file.data() + f.indexOffset, size_t(f.indexSize), pw.file.size(), pr) == PakParse::Ok);
    CHECK(ParsePakDirectoryIndex(pw.file.data() + pr.fdiOffset, size_t(pr.fdiSize), pr.mountPoint, maps) == PakParse::Ok && maps.size() == 1 && maps[0] == "/Game/Maps/W");

    // A pak with no maps parses to an empty list.
    const Pak pe = Build("../../../", {{"HalfswordUE5/Content/Textures/", {"T.uasset"}}});
    const std::vector<uint8_t> te = Tail(pe);
    CHECK(ParsePakFooter(te.data(), te.size(), pe.file.size(), f) == PakParse::Ok);
    CHECK(ParsePakPrimaryIndex(pe.file.data() + f.indexOffset, size_t(f.indexSize), pe.file.size(), pr) == PakParse::Ok);
    CHECK(ParsePakDirectoryIndex(pe.file.data() + pr.fdiOffset, size_t(pr.fdiSize), pr.mountPoint, maps) == PakParse::Ok && maps.empty());
}

static void TestFooterRejections() {
    PakFooter f;
    const Pak p = Build("../../../", kSample);
    const std::vector<uint8_t> tail = Tail(p);
    // Every truncation of the tail is "not a pak" (the magic is gone) or malformed: never Ok.
    for (size_t n = 0; n < 204; ++n) CHECK(ParsePakFooter(tail.data() + (tail.size() - n), n, p.file.size(), f) != PakParse::Ok);
    CHECK(ParsePakFooter(nullptr, 0, 0, f) == PakParse::NotPak);
    // An unrelated file.
    std::vector<uint8_t> junk(600, 0x5A);
    CHECK(ParsePakFooter(junk.data(), junk.size(), 100000, f) == PakParse::NotPak);
    // Versions: older and newer than the layout this reads.
    for (uint32_t v : {1u, 2u, 12u, 13u}) {
        const Pak pv = Build("../../../", kSample, v);
        const std::vector<uint8_t> tv = Tail(pv);
        CHECK(ParsePakFooter(tv.data(), tv.size(), pv.file.size(), f) == PakParse::Unsupported);
    }
    // A version number that is not one (a magic by chance) is skipped, not trusted.
    { const Pak pv = Build("../../../", kSample, 4000); const std::vector<uint8_t> tv = Tail(pv); CHECK(ParsePakFooter(tv.data(), tv.size(), pv.file.size(), f) == PakParse::NotPak); }
    // Encrypted index (the stock game's own pak).
    { const Pak pe = Build("../../../", kSample, 11, true); const std::vector<uint8_t> te = Tail(pe); CHECK(ParsePakFooter(te.data(), te.size(), pe.file.size(), f) == PakParse::Encrypted && f.encryptedIndex); }
    // The index must lie inside the file; empty and oversized indexes are refused.
    { std::vector<uint8_t> t = tail; const size_t at = t.size() - 221 + 17 + 8;   // index offset field
      for (int i = 0; i < 8; ++i) t[at + size_t(i)] = 0xFF;
      CHECK(ParsePakFooter(t.data(), t.size(), p.file.size(), f) == PakParse::Malformed); }
    { std::vector<uint8_t> t = tail; const size_t at = t.size() - 221 + 17 + 16;  // index size field
      for (int i = 0; i < 8; ++i) t[at + size_t(i)] = 0;
      CHECK(ParsePakFooter(t.data(), t.size(), p.file.size(), f) == PakParse::Malformed);
      for (int i = 0; i < 8; ++i) t[at + size_t(i)] = i == 5 ? 0x10 : 0;          // 16 TiB: past the file
      CHECK(ParsePakFooter(t.data(), t.size(), p.file.size(), f) == PakParse::Malformed);
      CHECK(ParsePakFooter(t.data(), t.size(), uint64_t(1) << 50, f) == PakParse::TooBig); }
}

static void TestIndexRejections() {
    const Pak p = Build("../../../", kSample);
    PakPrimary pr;
    const uint8_t* pri = p.file.data() + p.primaryOff;
    // Every truncation of the primary index.
    for (size_t n = 0; n + 8 < p.primarySize; ++n) CHECK(ParsePakPrimaryIndex(pri, n, p.file.size(), pr) != PakParse::Ok);
    // A directory index outside the file / too big / a bad flag.
    { std::vector<uint8_t> b(pri, pri + p.primarySize); const size_t mp = 4 + 10;  // after the mount FString ("../../../" + NUL)
      const size_t fdiFlag = mp + 4 + 8 + 4;
      std::vector<uint8_t> c = b; c[fdiFlag] = 2; CHECK(ParsePakPrimaryIndex(c.data(), c.size(), p.file.size(), pr) == PakParse::Malformed);
      c = b; c[fdiFlag] = 0; CHECK(ParsePakPrimaryIndex(c.data(), c.size(), p.file.size(), pr) == PakParse::Unsupported);
      c = b; for (int i = 0; i < 8; ++i) c[fdiFlag + 4 + size_t(i)] = 0xFF; CHECK(ParsePakPrimaryIndex(c.data(), c.size(), p.file.size(), pr) == PakParse::Malformed);
      c = b; c[fdiFlag + 4 + 12] = 0x7F; CHECK(ParsePakPrimaryIndex(c.data(), c.size(), uint64_t(1) << 60, pr) != PakParse::Ok); }
    // A mount point longer than the cap.
    { Buf b; b.u32(uint32_t(kMaxPakPathChars + 1)); b.zeros(kMaxPakPathChars + 1); b.zeros(64);
      CHECK(ParsePakPrimaryIndex(b.b.data(), b.b.size(), 1u << 20, pr) == PakParse::Malformed); }
    // A mount point whose terminator is not NUL.
    { Buf b; b.u32(3); b.u8('a'); b.u8('b'); b.u8('c'); b.zeros(64);
      CHECK(ParsePakPrimaryIndex(b.b.data(), b.b.size(), 1u << 20, pr) == PakParse::Malformed); }

    // The directory index: every truncation, a negative count, trailing bytes, a count the bytes cannot hold.
    std::vector<std::string> maps;
    const uint8_t* fdi = p.file.data() + p.fdiOff;
    for (size_t n = 0; n < p.fdiSize; ++n) CHECK(ParsePakDirectoryIndex(fdi, n, "../../../", maps) != PakParse::Ok);
    { std::vector<uint8_t> b(fdi, fdi + p.fdiSize); b.push_back(0); CHECK(ParsePakDirectoryIndex(b.data(), b.size(), "../../../", maps) == PakParse::Malformed); }
    { std::vector<uint8_t> b(fdi, fdi + p.fdiSize); b[3] = 0x80; CHECK(ParsePakDirectoryIndex(b.data(), b.size(), "../../../", maps) == PakParse::Malformed); }
    { std::vector<uint8_t> b(fdi, fdi + p.fdiSize); b[3] = 0x7F; CHECK(ParsePakDirectoryIndex(b.data(), b.size(), "../../../", maps) == PakParse::Malformed); }
    CHECK(ParsePakDirectoryIndex(nullptr, 0, "", maps) == PakParse::Malformed);
    { Buf b; b.u32(0); CHECK(ParsePakDirectoryIndex(b.b.data(), b.b.size(), "", maps) == PakParse::Ok && maps.empty()); }
    { Buf b; b.u32(0xFFFFFFFFu); CHECK(ParsePakDirectoryIndex(b.b.data(), b.b.size(), "", maps) == PakParse::Malformed); }
    // The per-pak map cap.
    { Dirs many(1); many[0].first = "HalfswordUE5/Content/M/";
      for (size_t i = 0; i < kMaxPakMaps + 40; ++i) many[0].second.push_back("Map" + std::to_string(i) + ".umap");
      const Pak pm = Build("../../../", many);
      CHECK(ParsePakDirectoryIndex(pm.file.data() + pm.fdiOff, pm.fdiSize, "../../../", maps) == PakParse::Ok && maps.size() == kMaxPakMaps); }
}

static PakAssets AssetsOf(const Dirs& dirs, const std::string& mount = "../../../") {
    const Pak p = Build(mount, dirs);
    const std::vector<uint8_t> tail = Tail(p);
    PakFooter f; PakPrimary pr; std::vector<std::string> maps; PakAssets a;
    if (ParsePakFooter(tail.data(), tail.size(), p.file.size(), f) != PakParse::Ok) { a.overflow = true; a.dirs = {"footer"}; return a; }
    if (ParsePakPrimaryIndex(p.file.data() + f.indexOffset, size_t(f.indexSize), p.file.size(), pr) != PakParse::Ok) { a.overflow = true; a.dirs = {"primary"}; return a; }
    if (ParsePakDirectoryIndex(p.file.data() + pr.fdiOffset, size_t(pr.fdiSize), pr.mountPoint, maps, &a) != PakParse::Ok) { a.overflow = true; a.dirs = {"fdi"}; }
    return a;
}

// What the game's lobby rule makes of a pak with this index, from the parser on.
static PakClass ClassOf(const Dirs& dirs) {
    const Pak p = Build("../../../", dirs);
    std::vector<std::string> maps;
    PakAssets a;
    if (ParsePakDirectoryIndex(p.file.data() + p.fdiOff, p.fdiSize, "../../../", maps, &a) != PakParse::Ok) return {};
    return ClassifyPakWhy(maps, a.dirs, a.overflow, a.overflowWhy, a.badMaps, a.stockFiles);
}

// The cooked shapes real map paks come in, from the parser on.
static void TestMapPaks() {
    const std::string root = "HalfswordUE5/Content/";
    CHECK(ClassOf({{root, {"Hole.umap", "Hole.uexp"}}}).kind == ModKind::MapPak);
    CHECK(ClassOf({{root, {"Hole.uexp", "Hole.umap", "Hole.ubulk", "Hole.uptnl"}}}).kind == ModKind::MapPak);   // any order
    CHECK(ClassOf({{root, {"Hole.umap", "Hole.uexp", "Hole_BuiltData.uasset", "Hole_BuiltData.uexp", "Hole_BuiltData.ubulk"}},
                   {root + "Hole/Textures/", {"T.uasset", "T.uexp"}}}).kind == ModKind::MapPak);
    CHECK(ClassOf({{"HalfswordUE5/Plugins/Hole/Content/", {"Hole.umap", "Hole.uexp"}}}).kind == ModKind::MapPak);
    // New files anywhere only add: beside the map, in a folder of their own, or in a stock folder.
    CHECK(ClassOf({{root, {"Hole.umap", "Hole.uexp", "Other.uasset", "Other.uexp"}}}).kind == ModKind::MapPak);
    CHECK(ClassOf({{root, {"Hole.umap", "Hole.uexp"}}, {root + "Stadium/", {"W.uasset"}}}).kind == ModKind::MapPak);
    CHECK(ClassOf({{root + "Maps/", {"Tower.umap", "Tower.uexp"}}}).kind == ModKind::MapPak);
    CHECK(ClassOf({{root + "Mine/", {"M.umap"}}, {root + "Character/Blueprints/", {"Mine.uasset", "Mine.uexp"}}}).kind == ModKind::MapPak);
    // A stock file by any of its extensions, in any case, wherever it comes in the index.
    PakClass c = ClassOf({{root + "Mine/", {"M.umap"}}, {root + "Character/Blueprints/", {"AI_BP.uasset", "AI_BP.uexp"}}});
    CHECK(c.kind == ModKind::ContentPak && c.why == "replaces stock files: /Game/Character/Blueprints/AI_BP");
    CHECK(ClassOf({{root + "Character/Blueprints/", {"ai_bp.ubulk"}}, {root + "Mine/", {"M.umap"}}}).kind == ModKind::ContentPak);
    CHECK(ClassOf({{root + "Mine/", {"M.umap"}}, {root, {"RT_Blood.uexp"}}}).why == "replaces stock files: /Game/RT_Blood");
    // A map that takes a stock package's place.
    c = ClassOf({{root, {"RT_Blood.umap", "RT_Blood.uexp"}}});
    CHECK(c.kind == ModKind::ContentPak && c.why == "replaces stock /Game/RT_Blood");
    // A map whose name cannot be a package path is a map all the same, and off the list: the log says so and names it.
    c = ClassOf({{root + "Mine/", {"Space Hall.umap", "Space Hall.uexp"}}});
    CHECK(c.kind == ModKind::MapPak && c.why == "a map's file name cannot be a package path");
    CHECK(AssetsOf({{root + "Mine/", {"Space Hall.umap", "A.b.umap"}}}).badMaps == 2);
    CHECK(AssetsOf({{root + "Mine/", {"Space Hall.umap", "A.b.umap"}}}).badMapName == "/Game/Mine/Space Hall");
    CHECK(ClassOf({{root + "My Props/", {"P.uasset"}}, {root + "Mine/", {"M.umap"}}}).kind == ModKind::MapPak);
    // A weapon pak's badly named test map doesn't help it.
    CHECK(ClassOf({{root + "Mods/Gun/", {"Test Map.umap", "Test Map.uexp", "ModActor.uasset"}}}).kind == ModKind::ContentPak);
    // The reasons name what happened. A .uasset under Engine/Plugins is neither too many folders nor a config file.
    c = ClassOf({{root + "Mine/", {"M.umap"}}, {"HalfswordUE5/Config/", {"DefaultGame.ini"}}});
    CHECK(c.kind == ModKind::ContentPak && c.why == "a config file");
    c = ClassOf({{root + "Mine/", {"M.umap"}}, {"Engine/Plugins/X/Content/", {"T.uasset"}}});
    CHECK(c.kind == ModKind::ContentPak && c.why == "a package file outside any content root");
    CHECK(ClassOf({{"HalfswordUE5/Config/", {"Stray.umap"}}}).kind == ModKind::ContentPak);     // nowhere a map could load from
    // Legacy paks read the same.
    const Pak lp = BuildLegacy("../../../", {"HalfswordUE5/Content/Hole.umap", "HalfswordUE5/Content/Hole.uexp"}, 8);
    std::vector<std::string> maps;
    PakAssets la;
    CHECK(ParsePakLegacyIndex(lp.file.data() + lp.primaryOff, lp.primarySize, 8, maps, &la) == PakParse::Ok);
    CHECK(maps.size() == 1 && la.dirs.size() == 1 && la.dirs[0] == "/Game" && !la.overflow && la.stockFiles.empty());
    const Pak bad = BuildLegacy("../../../", {"HalfswordUE5/Content/Mine/Space Hall.umap"}, 8);
    CHECK(ParsePakLegacyIndex(bad.file.data() + bad.primaryOff, bad.primarySize, 8, maps, &la) == PakParse::Ok);
    CHECK(maps.empty() && la.badMaps == 1 && la.badMapName == "/Game/Mine/Space Hall");
    const Pak swap = BuildLegacy("../../../", {"HalfswordUE5/Content/Mine/M.umap", "HalfswordUE5/Content/Character/Blueprints/AI_BP.uexp"}, 8);
    CHECK(ParsePakLegacyIndex(swap.file.data() + swap.primaryOff, swap.primarySize, 8, maps, &la) == PakParse::Ok);
    CHECK(la.stockFiles.size() == 1 && la.stockFiles[0] == "/Game/Character/Blueprints/AI_BP");   // a legacy name is a whole path
}

static void TestAssets() {
    // Side files do not count; an .ini anywhere does; a folder with a space is still a folder.
    PakAssets a = AssetsOf({{"HalfswordUE5/Content/", {"ShaderArchive-HalfswordUE5-PCD3D_SM6.ushaderbytecode"}}, {"HalfswordUE5/", {"AssetRegistry.bin"}},
                            {"HalfswordUE5/Content/Mine/", {"M.uasset", "M.uexp", "M.ubulk", "N.uptnl"}}});
    CHECK(!a.overflow && a.dirs.size() == 1 && a.dirs[0] == "/Game/Mine");
    a = AssetsOf({{"HalfswordUE5/Config/", {"DefaultGame.ini"}}});
    CHECK(a.overflow);
    a = AssetsOf({{"HalfswordUE5/Content/Bad Folder/", {"M.uasset"}}});
    CHECK(!a.overflow && a.dirs.size() == 1 && a.dirs[0] == "/Game/Bad Folder");
    a = AssetsOf({{"HalfswordUE5/Plugins/Mod/Content/Things/", {"T.uasset"}}});
    CHECK(!a.overflow && a.dirs.size() == 1 && a.dirs[0] == "/Mod/Things");
    a = AssetsOf({{"HalfswordUE5/Content/", {"Stray.uasset"}}});
    CHECK(!a.overflow && a.dirs.size() == 1 && a.dirs[0] == "/Game" && a.stockFiles.empty());
    // Stock files by name, once each, and no more than the log needs.
    Dirs stock;
    for (size_t i = 1; i < kMaxPakStockFiles + 3; ++i) {     // [0] is under /Engine
        const std::string path = kStockMapPaths[i];
        const size_t cut = path.rfind('/');
        stock.push_back({"HalfswordUE5/Content" + path.substr(5, cut - 4), {path.substr(cut + 1) + ".uexp", path.substr(cut + 1) + ".ubulk"}});
    }
    a = AssetsOf(stock);
    CHECK(a.stockFiles.size() == kMaxPakStockFiles && a.stockFiles[0] == kStockMapPaths[1] && a.stockFiles[1] == kStockMapPaths[2]);
    // More folders than the cap: overflow.
    Dirs many;
    for (size_t i = 0; i <= kMaxPakAssetDirs; ++i) many.push_back({"HalfswordUE5/Content/D" + std::to_string(i) + "/", {"A.uasset"}});
    a = AssetsOf(many);
    CHECK(a.overflow);
    // Legacy paks note the same.
    const Pak lp = BuildLegacy("../../../", kLegacyFiles, 8);
    std::vector<std::string> maps; PakAssets la;
    CHECK(ParsePakLegacyIndex(lp.file.data() + lp.primaryOff, lp.primarySize, 8, maps, &la) == PakParse::Ok);
    CHECK(maps.size() == 2 && la.dirs.size() == 2 && la.dirs[0] == "/Game/Maps/Airplane" && la.dirs[1] == "/Game/Textures" && !la.overflow);
}

int main() {
    TestAssets();
    TestMapPaks();
    TestLegacy();
    TestPackagePaths();
    TestParse();
    TestFooterRejections();
    TestIndexRejections();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok\n");
    return 0;
}
