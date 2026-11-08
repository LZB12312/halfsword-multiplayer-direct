// Unit tests for hsm_enhancer.h. Run via implementation/shared/run_tests.sh test_hsm_enhancer.
#include "session/hsm_enhancer.h"
#include <cstdio>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

// A version-resource-like block: some bytes, then the text as UTF-16LE, then more bytes.
static std::vector<uint8_t> Utf16Block(const std::string& text, size_t padBefore = 6, size_t padAfter = 4) {
    std::vector<uint8_t> b(padBefore, 0xAB);
    for (char c : text) { b.push_back(uint8_t(c)); b.push_back(0); }
    b.insert(b.end(), padAfter, 0);
    return b;
}

static void TestFileNames() {
    CHECK(IsEnhancerFileName("HSEnhancer.dll") && IsEnhancerFileName("hsenhancer.DLL") && IsEnhancerFileName("HS-Enhancer.dll"));
    CHECK(!IsEnhancerFileName("winmm.dll") && !IsEnhancerFileName("main.dll") && !IsEnhancerFileName("HSEnhancer.dll.bak"));
    CHECK(!IsEnhancerFileName("HSEnhancerLauncher.exe") && !IsEnhancerFileName("") && !IsEnhancerFileName("dwmapi.dll"));
    CHECK(FileNameOf("C:\\Games\\HalfSwordUE5\\Binaries\\Win64\\HSEnhancer.dll") == "HSEnhancer.dll");
    CHECK(FileNameOf("Z:/x/HS-Enhancer.dll") == "HS-Enhancer.dll" && FileNameOf("plain.dll") == "plain.dll" && FileNameOf("C:\\dir\\").empty());
}

static void TestVersionResource() {
    // 0.6.0's DLL and its winmm.dll proxy both carry ProductName "Half Sword Enhancer"; a renamed DLL still does.
    auto product = Utf16Block("ProductName Half Sword Enhancer");
    CHECK(VersionNamesEnhancer(product.data(), product.size()));
    auto desc = Utf16Block("Half Sword Enhancer Loader", 0, 0);   // FileDescription, right at the edges of the block
    CHECK(VersionNamesEnhancer(desc.data(), desc.size()));
    auto cased = Utf16Block("HALF SWORD ENHANCER mod");
    CHECK(VersionNamesEnhancer(cased.data(), cased.size()));
    auto game = Utf16Block("Half Sword");                          // the game itself, and UE4SS: no match
    auto ue4ss = Utf16Block("UE4SS Half Sword Enhanced");
    CHECK(!VersionNamesEnhancer(game.data(), game.size()) && !VersionNamesEnhancer(ue4ss.data(), ue4ss.size()));
    const std::string ascii = "xxHalf Sword Enhancerxx";             // 8-bit text is not the UTF-16 resource string
    CHECK(!VersionNamesEnhancer(reinterpret_cast<const uint8_t*>(ascii.data()), ascii.size()));
    auto cut = Utf16Block("Half Sword Enhance", 0, 0);              // one letter short at the end of the block
    CHECK(!VersionNamesEnhancer(cut.data(), cut.size()));
    CHECK(!VersionNamesEnhancer(nullptr, 100) && !VersionNamesEnhancer(product.data(), 3));
    auto odd = Utf16Block("Half Sword Enhancer", 7, 0);             // starts on an odd byte: not aligned, not a resource string
    CHECK(!VersionNamesEnhancer(odd.data(), odd.size()));
}

static void TestSignals() {
    EnhancerSignals s;
    CHECK(!s.Matches());
    s.exportName = true; CHECK(s.Matches());                         // renamed DLL, export still there
    EnhancerSignals v; v.version = true; CHECK(v.Matches());
    EnhancerSignals f; f.fileName = true; CHECK(f.Matches());
}

static void TestLobbyRule() {
    CHECK(LobbyAllowsEnhancer("1") && !LobbyAllowsEnhancer("0") && !LobbyAllowsEnhancer("") && !LobbyAllowsEnhancer("true"));
    // Blocked lobby: the Enhancer player is refused; everyone else is fine.
    CHECK(!EnhancerMayStay(false, true, false));
    CHECK(EnhancerMayStay(false, false, false));
    // Allowed lobby: everyone may stay.
    CHECK(EnhancerMayStay(true, true, false) && EnhancerMayStay(true, false, false));
    // The host never refuses itself; its lobby is allowed while it runs the Enhancer.
    CHECK(EnhancerMayStay(false, true, true));
    CHECK(HostAllowsEnhancer(false, true) && HostAllowsEnhancer(true, false) && !HostAllowsEnhancer(false, false));
}

static void TestMessagesSayWhat() {
    const std::string join = kEnhancerJoinRefusal, left = kEnhancerBlockedLeave;
    for (const std::string& m : {join, left}) {
        CHECK(m.find("Half Sword Enhancer detected") == 0);
        CHECK(m.find("HSEnhancer.dll") != std::string::npos && m.find("Binaries\\Win64") != std::string::npos);
    }
    CHECK(join.find("cannot join") != std::string::npos && left.find("you left") != std::string::npos);
}

int main() {
    TestFileNames();
    TestVersionResource();
    TestSignals();
    TestLobbyRule();
    TestMessagesSayWhat();
    if (g_fail) { std::printf("test_hsm_enhancer: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_enhancer: ok\n");
    return 0;
}
