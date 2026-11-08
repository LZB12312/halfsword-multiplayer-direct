// Modded maps (session/hsm_modmaps.h, rev 27): stock filters, names and paths, the catalogue, the hash cache, the ModMap
// wire (round trip, every truncation, rejections, bounds), the compatibility decision, the join line, the travel gate.
#include "session/hsm_modmaps.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static Sha256Digest Sha(uint8_t seed) { Sha256Digest d{}; for (size_t i = 0; i < d.size(); ++i) d[i] = uint8_t(seed + i); return d; }
static ModMapMsg Msg() {
    ModMapMsg m; m.path = "/Game/Maps/Custom/Arena_Bog"; m.display = "Arena Bog"; m.pakName = "BogMap_P.pak"; m.pakSize = 123456789; m.sha = Sha(1);
    return m;
}
static bool Dec(const std::vector<uint8_t>& b, ModMapMsg& o) { return Decode(b.data(), b.size(), o); }

static void TestStockFilter() {
    // Stock paks sit in the Paks folder itself; the same name in ~mods is a mod.
    CHECK(IsStockPakName("pakchunk0-Windows.pak", true));
    CHECK(IsStockPakName("PAKCHUNK0-WINDOWS.PAK", true));
    CHECK(IsStockPakName("pakchunk0optional-Windows.pak", true));
    CHECK(IsStockPakName("pakchunk0-Windows_0_P.pak", true));
    CHECK(IsStockPakName("pakchunk0-Windows_12_P.pak", true));
    CHECK(IsStockPakName("HalfswordUE5-Windows.pak", true));
    CHECK(!IsStockPakName("pakchunk0-Windows.pak", false));              // in ~mods: not the game's
    CHECK(!IsStockPakName("pakchunk1-Windows.pak", true));               // another chunk: a modder's
    CHECK(!IsStockPakName("pakchunk0-Windows_P.pak", true));             // no number
    CHECK(!IsStockPakName("pakchunk0-Windows__P.pak", true));
    CHECK(!IsStockPakName("pakchunk0-Windows_1_P.pak.bak", true));
    CHECK(!IsStockPakName("pakchunk0-WindowsX.pak", true));
    CHECK(!IsStockPakName("MyMap_P.pak", true) && !IsStockPakName("", true));
    CHECK(IsIoStoreCompanion("a.utoc") && IsIoStoreCompanion("A.UCAS") && !IsIoStoreCompanion("a.pak") && !IsIoStoreCompanion("utoc"));

    // Stock map packages, by the generated table (case-insensitive).
    CHECK(IsStockMapPath("/Game/Maps/Arenas/Map_Arena_Yard"));
    CHECK(IsStockMapPath("/game/maps/arenas/map_arena_yard"));
    CHECK(IsStockMapPath("/Engine/Maps/Entry"));
    CHECK(!IsStockMapPath("/Game/Maps/Custom/Arena_Bog") && !IsStockMapPath(""));
    CHECK(kStockMapPathCount > 30);
    for (size_t i = 0; i < kStockMapPathCount; ++i) CHECK(IsStockMapPath(kStockMapPaths[i]));

    CHECK(LooksLikeSublevel("/Game/Maps/Foo/Foo_Lighting") && LooksLikeSublevel("/Game/Maps/Foo/Details") && LooksLikeSublevel("/Game/Maps/Foo/Lighting/X"));
    CHECK(LooksLikeSublevel("/Game/Maps/LevelInstances/Foo") && LooksLikeSublevel("/Game/Maps/Foo/Map_Foo_PhysicsProps"));
    CHECK(!LooksLikeSublevel("/Game/Maps/Foo/Bog") && !LooksLikeSublevel("/Game/Maps/Foo/Skylight") && !LooksLikeSublevel("/Game/Maps/Foo/MySky") && !LooksLikeSublevel("/Game/Maps/Foo/Bigprops") && !LooksLikeSublevel("/Game/Maps/Foo/Foo2"));
}

static void TestNames() {
    CHECK(ModMapPathOk("/Game/Maps/Custom/Arena_Bog") && ModMapPathOk("/BogMod/Maps/Bog-2") && ModMapPathOk("/Game/X"));
    for (const char* bad : {"", "/", "/Game", "Game/Maps/X", "/Game/Maps/X/", "/Game//X", "/Engine/Maps/X", "/engine/Maps/X", "/Script/Foo",
                            "/Temp/X", "/Memory/X", "/Game/Maps/X.umap", "/Game/Maps/X Y", "/Game/Maps/X:1", "/Game/Maps/X?listen", "/Game/../X",
                            "/Game/Maps/X\\Y", "/Game/Mäps/X"})
        CHECK(!ModMapPathOk(bad));
    CHECK(!ModMapPathOk(std::string("/Game/") + std::string(kMaxModMapPath, 'a')));
    CHECK(ModMapPathOk("/Game/" + std::string(kMaxModMapPath - 6, 'a')));

    CHECK(ModPakNameOk("BogMap_P.pak") && ModPakNameOk("a.PAK") && ModPakNameOk("My Map (v2).pak"));
    for (const char* bad : {"", ".pak", "x.pa", "x.bak", ".hidden.pak", "a/b.pak", "a\\b.pak", "a:b.pak", "a*.pak", "a?.pak", "a..b.pak", "a|b.pak", "a\"b.pak", "a<b.pak"})
        CHECK(!ModPakNameOk(bad));
    CHECK(!ModPakNameOk(std::string(kMaxModPakName, 'a') + ".pak"));
    CHECK(ModPakNameOk(std::string(kMaxModPakName - 4, 'a') + ".pak"));
    CHECK(!ModPakNameOk("caf\xC3\xA9.pak"));

    CHECK(SanitizePakName("caf\xC3\xA9.pak") == "caf__.pak");
    CHECK(SanitizePakName("a/b\\c:d.pak") == "a_b_c_d.pak");
    CHECK(SanitizePakName(std::string(300, 'x')).size() == kMaxModPakName);
    CHECK(ModPakNameOk(SanitizePakName("caf\xC3\xA9.pak")));
    // A pak's name can't hide it: this is the one a tester used, left-to-right mark, escapes and all.
    const std::string sneaky = "\xE2\x80\x8E'z'." + std::string(300, '5') + ".pak";
    CHECK(ModPakNameOk(SanitizePakName(sneaky)) && SanitizePakName(sneaky).size() == kMaxModPakName);
    CHECK(ModPakNameOk(SanitizePakName("..\\..\\a.pak")) && ModPakNameOk(SanitizePakName(".hidden.pak")) && ModPakNameOk(SanitizePakName("a..b.pak")));

    CHECK(ModMapDisplayName("/Game/Maps/Custom/My_Cool-Map") == "My Cool Map");
    CHECK(ModMapDisplayName("/Game/Maps/X_") == "X");
    CHECK(ModMapDisplayName("/Game/_") == "Modded map");
    CHECK(ModMapDisplayName("/Game/" + std::string(100, 'a')).size() == kMaxModMapDisplay);
}

static void TestCatalogue() {
    const std::vector<PakMaps> paks = {
        {0, {"/Game/Maps/Custom/Arena_Bog", "/Game/Maps/Custom/Arena_Bog_Lighting", "/Game/Maps/Arenas/Map_Arena_Yard", "/Engine/Maps/Entry", "/Game/Bad Name/X"}},
        {1, {"/Game/Maps/Custom/Apex", "/Game/Maps/Custom/Arena_Bog"}},                 // a duplicate of pak 0's
        {2, {"/Game/Maps/Only/Only_Lighting"}},                                         // only a sublevel: kept (nothing else to offer)
    };
    const std::vector<std::string> reg = {"/Game/Maps/Custom/Arena_Bog", "/Game/Maps/Registry/Zed", "/Game/Maps/Registry/Zed_Details",
                                          "/Game/Maps/Arenas/Map_Arena_Yard", "/Script/Foo", "/Game/Maps/Custom/Apex"};
    const auto list = BuildModMapList(paks, reg);
    std::vector<std::string> paths; for (const auto& e : list) paths.push_back(e.path);
    CHECK(list.size() == 4);
    CHECK(list.size() == 4 && paths[0] == "/Game/Maps/Custom/Apex" && paths[1] == "/Game/Maps/Custom/Arena_Bog" && paths[2] == "/Game/Maps/Only/Only_Lighting" && paths[3] == "/Game/Maps/Registry/Zed");
    CHECK(list.size() == 4 && list[0].pak == 1 && list[1].pak == 0 && list[2].pak == 2 && list[3].pak == -1);
    CHECK(list.size() == 4 && list[1].display == "Arena Bog" && list[3].display == "Zed");
    CHECK(FindModMap(list, "/game/maps/custom/arena_bog")->pak == 0);
    CHECK(FindModMap(list, "/Game/Maps/Custom/Arena_Bog.Arena_Bog") != nullptr);
    CHECK(FindModMap(list, "/Game/Maps/Arenas/Map_Arena_Yard") == nullptr && FindModMap(list, "") == nullptr);
    CHECK(BuildModMapList({}, {}).empty());
    // The cap.
    std::vector<std::string> many;
    for (size_t i = 0; i < kMaxModMaps + 20; ++i) many.push_back("/Game/Maps/M/Map" + std::to_string(1000 + i));
    CHECK(BuildModMapList({}, many).size() == kMaxModMaps);
}

static void TestHashCache() {
    CHECK(HashCacheKey("C:\\Paks\\~mods\\A.pak", 10, 20) == HashCacheKey("c:/paks/~mods/a.pak", 10, 20));
    CHECK(HashCacheKey("a.pak", 10, 20) != HashCacheKey("a.pak", 11, 20));
    CHECK(HashCacheKey("a.pak", 10, 20) != HashCacheKey("a.pak", 10, 21));
    CHECK(HashCacheKey("a.pak", 10, 20) != HashCacheKey("b.pak", 10, 20));
    HashCache c;
    c.Put("C:/p/a.pak", 100, 5, Sha(1));
    c.Put("C:/p/b.pak", 200, 6, Sha(2));
    CHECK(c.Lookup("c:\\p\\A.pak", 100, 5) == Sha(1));
    CHECK(!c.Lookup("C:/p/a.pak", 101, 5) && !c.Lookup("C:/p/a.pak", 100, 6) && !c.Lookup("C:/p/c.pak", 100, 5));
    c.Put("C:/p/a.pak", 150, 9, Sha(3));                                // the file changed: the old entry is gone
    CHECK(!c.Lookup("C:/p/a.pak", 100, 5) && c.Lookup("C:/p/a.pak", 150, 9) == Sha(3) && c.size() == 2);
    HashCache d; d.Parse(c.Serialize());
    CHECK(d.size() == 2 && d.Lookup("C:/p/a.pak", 150, 9) == Sha(3) && d.Lookup("C:/p/b.pak", 200, 6) == Sha(2));
    CHECK(d.Serialize() == c.Serialize());
    // Garbage and hostile lines are skipped, never trusted.
    const std::string good = "v1\t" + Sha256Hex(Sha(7)) + "\t10\t20\tp/x.pak\n";
    HashCache e;
    e.Parse("garbage\n\nv2\t" + Sha256Hex(Sha(7)) + "\t10\t20\tp/y.pak\nv1\tzz\t10\t20\tp/x.pak\nv1\t" + Sha256Hex(Sha(7)) + "\t-1\t20\tp/x.pak\n" +
            "v1\t" + Sha256Hex(Sha(7)) + "\t10\t20\t\nv1\t" + Sha256Hex(Sha(7)) + "\t99999999999999999999999\t20\tp/x.pak\n" +
            "v1\t" + Sha256Hex(Sha(7)) + "\t10\t20\tp/\x01.pak\nv1\t" + Sha256Hex(Sha(7)) + "\t10\t20\tp/x.pak\textra\n" + good);
    CHECK(e.size() == 1 && e.Lookup("p/x.pak", 10, 20) == Sha(7) && !e.Lookup("p/y.pak", 10, 20));
    e.Parse(good.substr(0, good.size() - 3)); CHECK(e.size() == 1);                // a cut path is still a path; the hash decides
    e.Parse(std::string(100000, 'x')); CHECK(e.size() == 0);
    e.Parse(std::string(kMaxHashCacheLine + 10, 'x') + "\n" + good); CHECK(e.size() == 1);
    e.Parse(good + good); CHECK(e.size() == 1);
    // The entry cap drops the oldest.
    HashCache big;
    for (size_t i = 0; i < kMaxHashCacheEntries + 5; ++i) big.Put("p/" + std::to_string(i) + ".pak", i + 1, 1, Sha(uint8_t(i)));
    CHECK(big.size() == kMaxHashCacheEntries && !big.Lookup("p/0.pak", 1, 1) && big.Lookup("p/" + std::to_string(kMaxHashCacheEntries + 4) + ".pak", kMaxHashCacheEntries + 5, 1));
    HashCache big2; big2.Parse(big.Serialize()); CHECK(big2.size() == kMaxHashCacheEntries);
}

static void TestWire() {
    const ModMapMsg m = Msg();
    const auto b = Encode(m);
    CHECK(b[3] == 53 && b[2] == kProtocolRevision);
    ModMapMsg o;
    CHECK(Dec(b, o) && o.path == m.path && o.display == m.display && o.pakName == m.pakName && o.pakSize == m.pakSize && o.sha == m.sha);
    CHECK(Encode(o) == b);
    for (size_t n = 0; n < b.size(); ++n) { ModMapMsg t; CHECK(!Decode(b.data(), n, t)); }
    { auto longer = b; longer.push_back(0); CHECK(!Dec(longer, o)); }
    { auto wrongRev = b; wrongRev[2] = uint8_t(kProtocolRevision + 1); CHECK(!Dec(wrongRev, o)); }
    { auto wrongType = b; wrongType[3] = 47; CHECK(!Dec(wrongType, o)); }
    // Stock pick: empty everything.
    { ModMapMsg s; const auto sb = Encode(s); CHECK(Dec(sb, o) && o.path.empty());
      for (size_t n = 0; n < sb.size(); ++n) { ModMapMsg t; CHECK(!Decode(sb.data(), n, t)); }
      ModMapMsg junk; junk.display = "x"; CHECK(!Dec(Encode(junk), o));
      ModMapMsg junk2; junk2.pakSize = 1; CHECK(!Dec(Encode(junk2), o));
      ModMapMsg junk3; junk3.sha = Sha(1); CHECK(!Dec(Encode(junk3), o));
      ModMapMsg junk4; junk4.pakName = "A.pak"; CHECK(!Dec(Encode(junk4), o)); }
    // A map the host cannot attribute to a pak: path + display, no pak fields.
    { ModMapMsg u = m; u.pakName.clear(); u.pakSize = 0; u.sha = {}; CHECK(Dec(Encode(u), o) && o.pakName.empty());
      u.pakSize = 5; CHECK(!Dec(Encode(u), o)); u.pakSize = 0; u.sha = Sha(1); CHECK(!Dec(Encode(u), o)); }
    // Rejections.
    ModMapMsg x;
    x = m; x.path = "/Engine/Maps/Entry"; CHECK(!Dec(Encode(x), o));
    x = m; x.path = "Game/Maps/X"; CHECK(!Dec(Encode(x), o));
    x = m; x.path = "/Game/Maps/X Y"; CHECK(!Dec(Encode(x), o));
    x = m; x.display.clear(); CHECK(!Dec(Encode(x), o));
    x = m; x.display = "bad\x01name"; CHECK(!Dec(Encode(x), o));
    x = m; x.pakName = "../evil.pak"; CHECK(!Dec(Encode(x), o));
    x = m; x.pakName = "BogMap_P.exe"; CHECK(!Dec(Encode(x), o));
    x = m; x.pakSize = 0; CHECK(!Dec(Encode(x), o));
    x = m; x.pakSize = kMaxModPakBytes + 1; CHECK(!Dec(Encode(x), o));
    x = m; x.pakSize = kMaxModPakBytes; CHECK(Dec(Encode(x), o));
    x = m; x.pakSize = 1; CHECK(Dec(Encode(x), o));
    x = m; x.sha = {}; CHECK(!Dec(Encode(x), o));
    // Bounds on the strings: the writer clamps, the reader rejects an over-long length on a hand-built message.
    { Writer w; w.header(MsgType::ModMap); w.u8(uint8_t(kMaxModMapPath + 1)); for (size_t i = 0; i < kMaxModMapPath + 1; ++i) w.u8('a');
      w.u8(0); w.u8(0); w.u64(0); for (int i = 0; i < 32; ++i) w.u8(0); CHECK(!Dec(w.buf, o)); }
    // Fuzz-ish: every single-byte flip either decodes to a valid message or is rejected, never crashes; and a valid one re-encodes identically.
    for (size_t i = 0; i < b.size(); ++i)
        for (int bit = 0; bit < 8; ++bit) { auto f = b; f[i] ^= uint8_t(1u << bit); ModMapMsg t; if (Dec(f, t)) CHECK(Encode(t) == f); }
}

static void TestHostAndDecision() {
    const std::vector<ModMapEntry> maps = {{"/Game/Maps/A", "A", 0}, {"/Game/Maps/B", "B", 1}, {"/Game/Maps/C", "C", -1}, {"/Game/Maps/D", "D", 7}};
    ModPak p0{"A_P.pak", 1000, true, Sha(1)}, p1{"B_P.pak", 2000, false, {}};
    const std::vector<ModPak> paks = {p0, p1};
    CHECK(MakeHostModMap("/Game/Maps/Arenas/Map_Arena_Yard", maps, paks).state == HostModMap::Stock);
    CHECK(MakeHostModMap("", maps, paks).state == HostModMap::Stock);
    { auto r = MakeHostModMap("/Game/Maps/A", maps, paks); CHECK(r.state == HostModMap::Ready && r.msg.pakName == "A_P.pak" && r.msg.pakSize == 1000 && r.msg.sha == Sha(1) && r.msg.display == "A"); }
    CHECK(MakeHostModMap("/Game/Maps/B", maps, paks).state == HostModMap::HashPending);
    { auto r = MakeHostModMap("/Game/Maps/C.C", maps, paks); CHECK(r.state == HostModMap::Ready && r.msg.pakName.empty() && r.msg.path == "/Game/Maps/C"); }
    { auto r = MakeHostModMap("/Game/Maps/D", maps, paks); CHECK(r.state == HostModMap::Ready && r.msg.pakName.empty()); }   // a pak index out of range is "unknown"

    const ModMapMsg host = Msg();
    const std::vector<ModMapEntry> none;
    ModMapMsg stock; CHECK(DecideModMap(stock, {}, none, true) == ModMapVerdict::NoMap);
    // The pak matches by size and hash, wherever it is and whatever it is called.
    ModPak same{"Renamed.pak", host.pakSize, true, host.sha};
    CHECK(DecideModMap(host, {same}, none, true) == ModMapVerdict::Ok);
    CHECK(DecideModMap(host, {same}, none, false) == ModMapVerdict::Ok);
    ModPak diffHash{"BogMap_P.pak", host.pakSize, true, Sha(9)};
    CHECK(DecideModMap(host, {diffHash}, none, true) == ModMapVerdict::DifferentPak);
    ModPak diffSize{"BogMap_P.pak", host.pakSize + 1, true, host.sha};
    CHECK(DecideModMap(host, {diffSize}, none, true) == ModMapVerdict::DifferentPak);
    ModPak otherName{"Other.pak", 5, true, Sha(3)};
    CHECK(DecideModMap(host, {otherName}, none, true) == ModMapVerdict::MissingPak);
    CHECK(DecideModMap(host, {}, none, true) == ModMapVerdict::MissingPak);
    CHECK(DecideModMap(host, {}, none, false) == ModMapVerdict::Pending);                  // the scan is not over
    ModPak waiting{"X.pak", host.pakSize, false, {}};
    CHECK(DecideModMap(host, {waiting}, none, true) == ModMapVerdict::Pending);            // a hash of the right size is still running
    ModPak waitingWrongSize{"X.pak", host.pakSize + 5, false, {}};
    CHECK(DecideModMap(host, {waitingWrongSize}, none, true) == ModMapVerdict::MissingPak);
    CHECK(DecideModMap(host, {waiting, same}, none, true) == ModMapVerdict::Ok);
    CHECK(DecideModMap(host, {diffHash, waiting}, none, true) == ModMapVerdict::Pending);
    // A host that could not name the pak: this machine needs the map itself.
    ModMapMsg unattributed = host; unattributed.pakName.clear(); unattributed.pakSize = 0; unattributed.sha = {};
    CHECK(DecideModMap(unattributed, {}, {{host.path, "x", -1}}, true) == ModMapVerdict::Ok);
    CHECK(DecideModMap(unattributed, {}, none, true) == ModMapVerdict::MissingMap);
    CHECK(DecideModMap(unattributed, {}, none, false) == ModMapVerdict::Pending);

    // The line the player reads.
    CHECK(ModMapJoinLine(host, ModMapVerdict::MissingPak) == "This lobby uses the modded map Arena Bog from BogMap_P.pak. Install that pak in Content/Paks/~mods to join.");
    CHECK(ModMapJoinLine(host, ModMapVerdict::DifferentPak).find("different version") != std::string::npos);
    CHECK(ModMapJoinLine(host, ModMapVerdict::MissingMap).find("Arena Bog") != std::string::npos);
    CHECK(ModMapJoinLine(host, ModMapVerdict::Ok).empty() && ModMapJoinLine(host, ModMapVerdict::NoMap).empty() && ModMapJoinLine(host, ModMapVerdict::Pending).empty());
    { ModMapMsg nd = host; nd.display.clear(); CHECK(ModMapJoinLine(nd, ModMapVerdict::MissingPak).find("Arena Bog") != std::string::npos); }
}

static void TestTravelGate() {
    const std::string bog = "/Game/Maps/Custom/Arena_Bog";
    CHECK(TravelAllowedWithModMap("/Game/Maps/Arenas/Map_Arena_Yard", "", ""));         // stock still works
    CHECK(!TravelAllowedWithModMap(bog, "", ""));                                       // nothing approved
    CHECK(TravelAllowedWithModMap(bog, "", bog));
    CHECK(TravelAllowedWithModMap("/game/maps/custom/arena_bog", "", bog));
    CHECK(TravelAllowedWithModMap(bog + ".Arena_Bog", "", bog));
    CHECK(!TravelAllowedWithModMap("/Game/Maps/Custom/Other", "", bog));
    CHECK(!TravelAllowedWithModMap(bog + "2", "", bog));
    CHECK(!TravelAllowedWithModMap(bog, "?game=/Script/Evil.Mode", bog));               // options still checked
    CHECK(!TravelAllowedWithModMap(bog + "?listen", "", bog));
    CHECK(!TravelAllowedWithModMap(bog + " ", "", bog));
    CHECK(!TravelAllowedWithModMap("", "", bog));
    CHECK(!TravelAllowedWithModMap(bog, "", "/Engine/Maps/Entry"));                     // an approval that is not a valid mod path
    CHECK(!TravelAllowedWithModMap("/Engine/Maps/Entry", "", "/Engine/Maps/Entry") || TravelAllowed("/Engine/Maps/Entry", ""));
    CHECK(!TravelAllowedWithModMap(std::string(300, 'a'), "", std::string(300, 'a')));
}

int main() {
    TestStockFilter();
    TestNames();
    TestCatalogue();
    TestHashCache();
    TestWire();
    TestHostAndDecision();
    TestTravelGate();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok\n");
    return 0;
}
