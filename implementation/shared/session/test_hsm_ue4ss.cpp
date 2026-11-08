// Unit tests for hsm_ue4ss.h. Run via implementation/shared/run_tests.sh test_hsm_ue4ss.
#include "session/hsm_ue4ss.h"
#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::ue4ss;

static void TestPick() {
    CHECK(ChoosePick({}) == Pick::None);
    CHECK(ChoosePick({true, false, false}) == Pick::Sub);
    CHECK(ChoosePick({false, true, false}) == Pick::Beside);
    CHECK(ChoosePick({true, true, false}) == Pick::Sub);          // 3.x layout wins over a stray old copy
    CHECK(ChoosePick({false, false, true}) == Pick::Proxy2x);
    CHECK(ChoosePick({true, true, true}) == Pick::Proxy2x);       // 2.x loads itself: never a second copy
    CHECK(std::strcmp(RelPath(Pick::Sub), "ue4ss/UE4SS.dll") == 0 && std::strcmp(RelPath(Pick::Beside), "UE4SS.dll") == 0);
    CHECK(RelPath(Pick::None)[0] == 0 && RelPath(Pick::Proxy2x)[0] == 0);
    CHECK(std::strcmp(RelDir(Pick::Sub), "ue4ss") == 0 && RelDir(Pick::Beside)[0] == 0);
    CHECK(LooksLikeUe4ss2x(true, true, false) && LooksLikeUe4ss2x(true, false, true) && LooksLikeUe4ss2x(true, true, true));
    CHECK(!LooksLikeUe4ss2x(true, false, false) && !LooksLikeUe4ss2x(false, true, true));
}

static void TestOrder() {
    CHECK(ParseOrder("before") == Order::Before && ParseOrder("  BEFORE \r") == Order::Before && ParseOrder("Before") == Order::Before);
    CHECK(ParseOrder("after") == Order::After && ParseOrder("") == Order::After && ParseOrder("befor") == Order::After);
    CHECK(ParseOrder("beforex") == Order::After && ParseOrder("xbefore") == Order::After && ParseOrder("1") == Order::After);
}

static void TestModsTxt() {
    const char* txt =
        "\xEF\xBB\xBF; comment line\r\n"
        "\r\n"
        "BPModLoaderMod : 1\r\n"
        "  Half Sword Trainer:1  \n"
        "OffMod : 0\n"
        "; Disabled : 1\n"
        "NoColonLine\n"
        "Weird : yes\n"
        " : 1\n"
        "keybinds : 1\n";
    const auto m = ParseModsTxt(txt);
    CHECK(m.size() == 5);
    CHECK(m[0].name == "BPModLoaderMod" && m[0].enabled);
    CHECK(m[1].name == "Half Sword Trainer" && m[1].enabled);
    CHECK(m[2].name == "OffMod" && !m[2].enabled);
    CHECK(m[3].name == "Weird" && !m[3].enabled);
    CHECK(m[4].name == "keybinds" && m[4].enabled);
    // the last value of a repeated name wins, at the first position, any case
    const auto d = ParseModsTxt("A : 1\nB : 1\na : 0\n");
    CHECK(d.size() == 2 && d[0].name == "A" && !d[0].enabled && d[1].enabled);
    const auto bom = ParseModsTxt("\xEF\xBB\xBF" "First : 1\n");
    CHECK(bom.size() == 1 && bom[0].name == "First");   // BOM
    CHECK(ParseModsTxt("").empty() && ParseModsTxt("\n\n;x\n").empty());
    CHECK(ParseModsTxt("Last : 1").size() == 1);                      // no trailing newline
    CHECK(ParseModsTxt("Bad\x01Name : 1\n").empty());                 // a control character is not a mod name
    CHECK(ParseModsTxt("A:1\nB: 10\n")[1].enabled == false);          // only "1" enables
}

static void TestBounds() {
    std::string many;
    for (int i = 0; i < 100; ++i) many += "Mod" + std::to_string(i) + " : 1\n";
    CHECK(ParseModsTxt(many).size() == kMaxMods);
    const auto lng = ParseModsTxt(std::string(300, 'x') + " : 1\n");
    CHECK(lng.size() == 1 && lng[0].name.size() == kMaxNameLen);
    // text past the read limit is ignored
    std::string big = "First : 1\n" + std::string(kMaxModsTxtBytes, ';') + "\nLate : 1\n";
    CHECK(ParseModsTxt(big).size() == 1);
    std::string exactCut = std::string(kMaxModsTxtBytes - 8, ' ') + "AB : 1\nZ : 1\n";   // "AB : 1" starts before the limit but is cut
    CHECK(ParseModsTxt(exactCut).size() <= 1);
}

static void TestEnabled() {
    const auto txt = ParseModsTxt("A : 1\nB : 0\nC : 1\n");
    auto e = EnabledMods(txt, {"B", "D", "c", "E"});
    CHECK((e == std::vector<std::string>{"A", "C", "D", "E"}));       // B is off in mods.txt, c repeats C
    CHECK(EnabledMods({}, {"X", "x", "Y"}).size() == 2);
    CHECK(EnabledMods({}, {}).empty());
    std::vector<std::string> folders;
    for (int i = 0; i < 100; ++i) folders.push_back("F" + std::to_string(i));
    CHECK(EnabledMods({}, folders).size() == kMaxMods);
    CHECK(EnabledMods({}, {"Bad\x02"}).empty());
}

static void TestFormat() {
    CHECK(FormatMods({}).empty());
    CHECK(FormatMods({"A", "B"}) == "A, B");
    std::vector<std::string> names;
    for (size_t i = 0; i < kMaxMods; ++i) names.push_back(std::string(kMaxNameLen, char('a' + i % 26)));
    const std::string f = FormatMods(names);
    CHECK(f.size() <= kMaxFactLen && f.find(" more") != std::string::npos && f.find("+") != std::string::npos);
    // exactly fitting names are not cut
    std::vector<std::string> fit{std::string(kMaxFactLen, 'q')};
    CHECK(FormatMods(fit) == fit[0]);
    CHECK(FormatMods({std::string(kMaxFactLen, 'q'), "z"}) == " +2 more");   // room is kept for the count
    CHECK(FormatFileVersion(3, 0, 1, 0) == "3.0.1.0" && FormatFileVersion(0, 0, 0, 0) == "unknown version" && FormatFileVersion(0, 0, 0, 7) == "0.0.0.7");
}

static void TestProxy() {
    CHECK(OtherProxyName("version.dll") && OtherProxyName("DSOUND.DLL") && OtherProxyName("winmm.dll") && OtherProxyName("xinput1_3.dll"));
    CHECK(!OtherProxyName("dwmapi.dll") && !OtherProxyName("xinput1_4.dll") && !OtherProxyName("") && !OtherProxyName("version.dl"));
    CHECK(ProxyNote(true, true, false, {}).empty());
    CHECK(ProxyNote(false, false, false, {}).empty());
    CHECK(ProxyNote(false, true, false, {}).find("not HSMMod's") != std::string::npos);
    CHECK(ProxyNote(true, true, true, {}).find("ue4ss/dwmapi.dll") != std::string::npos);
    CHECK(ProxyNote(true, true, false, {"version.dll"}).find("version.dll") != std::string::npos);
    CHECK(ProxyNote(false, true, false, {"winmm.dll"}).find("; another proxy") != std::string::npos);
}

static void TestBundle() {
    // Nothing there: unpack it, whatever the flags say with no DLL to judge.
    CHECK(ChooseBundleStep(false, false, false, false) == BundleStep::Install);
    CHECK(ChooseBundleStep(false, true, true, true) == BundleStep::Install);
    // The bundled build stays, and an older accepted one goes without a question.
    CHECK(ChooseBundleStep(true, true, true, false) == BundleStep::None);
    CHECK(ChooseBundleStep(true, false, true, false) == BundleStep::Replace);
    // A build nobody knows is the player's to decide, once.
    CHECK(ChooseBundleStep(true, false, false, false) == BundleStep::Ask);
    CHECK(ChooseBundleStep(true, false, false, true) == BundleStep::Keep);

    CHECK(BundleFilePath("ue4ss/UE4SS.dll") == "ue4ss\\UE4SS.dll");
    CHECK(BundleFilePath("ue4ss/Mods/Keybinds/Scripts/main.lua") == "ue4ss\\Mods\\Keybinds\\Scripts\\main.lua");
    // UE4SS's proxy stays in the zip: that slot is HSMMod's.
    CHECK(BundleFilePath("dwmapi.dll").empty());
    CHECK(BundleFilePath("ue4ss/").empty() && BundleFilePath("ue4ss/Mods/").empty());
    CHECK(BundleFilePath("ue4ss/../dwmapi.dll").empty() && BundleFilePath("ue4ss/a\\b").empty() && BundleFilePath("ue4ss/C:x").empty());
    CHECK(BundleFilePath("ue4ssx/a").empty());
}

static void TestConsoleMods() {
    for (const char* m : kConsoleMods) CHECK(IsConsoleMod(m));
    CHECK(IsConsoleMod("consoleenablermod"));
    CHECK(!IsConsoleMod("Keybinds") && !IsConsoleMod("BPModLoaderMod") && !IsConsoleMod(""));
}

int main() {
    TestBundle();
    TestConsoleMods();
    TestPick();
    TestOrder();
    TestModsTxt();
    TestBounds();
    TestEnabled();
    TestFormat();
    TestProxy();
    if (g_fail) { std::printf("test_hsm_ue4ss: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_ue4ss: ok\n");
    return 0;
}
