// Unit tests for hsm_build.h. Run via implementation/shared/run_tests.sh test_hsm_build.
#include "session/hsm_build.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestBuildString() {
    CHECK(ModBuildString("0.6.4", "") == "0.6.4");
    CHECK(ModBuildString("0.6.4", "v0.6.3-12-gdb82570") == "0.6.4 (v0.6.3-12-gdb82570)");
    const std::string longOne = ModBuildString("0.6.4", std::string(200, 'g'));
    CHECK(longOne.size() == kMaxModBuild && longOne.rfind("0.6.4 (", 0) == 0);
}

static void TestBinaryHash() {
    CHECK(ModBuildString("0.8.0-pre26", "3dc8f7c", "1a2b3c4d5e6f") == "0.8.0-pre26 (3dc8f7c) #1a2b3c4d5e6f");
    CHECK(ModBuildString("0.8.0-pre26", "", "1a2b3c4d5e6f") == "0.8.0-pre26 #1a2b3c4d5e6f");
    CHECK(ModBuildString("0.8.0-pre26", "3dc8f7c", "") == "0.8.0-pre26 (3dc8f7c)");   // the DLL could not read itself
    // A long describe is cut, never the hash.
    const std::string longOne = ModBuildString("0.8.0-pre26", std::string(200, 'g'), "1a2b3c4d5e6f");
    CHECK(longOne.size() == kMaxModBuild);
    CHECK(longOne.size() >= 14 && longOne.substr(longOne.size() - 14) == " #1a2b3c4d5e6f");
    // A hash longer than the whole field (a full digest, not BuildHash's 12 digits) is cut too: never over the wire cap.
    CHECK(ModBuildString("0.8.0-pre26", "3dc8f7c", std::string(64, 'f')).size() == kMaxModBuild);
    // Same version and commit, different binaries: different builds.
    const std::string a = ModBuildString("0.8.0-pre26", "3dc8f7c", "1a2b3c4d5e6f");
    const std::string b = ModBuildString("0.8.0-pre26", "3dc8f7c", "ffffffffffff");
    CHECK(!MayStartMatch(a, {{"Bob", b}}, false));
    CHECK(BuildWarning(a, {{"Bob", b}}) == "Bob on " + b + " - update");
    // BuildHash: the first 12 hex digits of the digest.
    Sha256Digest d{};
    d[0] = 0x1a; d[1] = 0x2b; d[2] = 0x3c; d[3] = 0x4d; d[4] = 0x5e; d[5] = 0x6f; d[6] = 0x77;
    CHECK(BuildHash(d) == "1a2b3c4d5e6f");
    CHECK(BuildHash(Sha256Of(reinterpret_cast<const uint8_t*>("abc"), 3)) == "ba7816bf8f01");   // FIPS 180-2 "abc"
}

static void TestMatchingBuildsSayNothing() {
    const std::vector<PeerBuild> others{{"Bob", "0.6.4"}, {"Eve", "0.6.4"}};
    CHECK(BuildMismatches("0.6.4", others).empty());
    CHECK(BuildWarning("0.6.4", others).empty());
    CHECK(MayStartMatch("0.6.4", others, false));
    CHECK(BuildWarning("0.6.4", {}).empty() && MayStartMatch("0.6.4", {}, false));
}

static void TestMismatchIsNamed() {
    const std::vector<PeerBuild> others{{"Bob", "0.6.3"}, {"Eve", "0.6.4"}};
    const std::vector<std::string> m = BuildMismatches("0.6.4", others);
    CHECK(m.size() == 1 && m[0] == "Bob on 0.6.3");
    CHECK(BuildWarning("0.6.4", others) == "Bob on 0.6.3 - update");
    CHECK(!MayStartMatch("0.6.4", others, false));
    CHECK(MayStartMatch("0.6.4", others, true));   // allow_build_mismatch=1
}

static void TestSeveralMismatchesAndUnknowns() {
    const std::vector<PeerBuild> others{{"Bob", "0.6.3"}, {"", "0.6.4 (v0.6.3-2-gabc)"}, {"Ann", ""}};
    CHECK(BuildWarning("0.6.4", others) == "Bob on 0.6.3; another player on 0.6.4 (v0.6.3-2-gabc) - update");
    // a build not known yet (no Hello or member data) neither warns nor blocks
    const std::vector<PeerBuild> unknown{{"Ann", ""}};
    CHECK(BuildWarning("0.6.4", unknown).empty() && MayStartMatch("0.6.4", unknown, false));
}

static void TestSameVersionOtherCommitDiffers() {   // two dev builds of one version are two builds
    const std::vector<PeerBuild> others{{"Bob", ModBuildString("0.6.4", "v0.6.3-11-g15c0495")}};
    CHECK(!MayStartMatch(ModBuildString("0.6.4", "v0.6.3-12-gdb82570"), others, false));
}

int main() {
    TestBuildString();
    TestMatchingBuildsSayNothing();
    TestMismatchIsNamed();
    TestSeveralMismatchesAndUnknowns();
    TestSameVersionOtherCommitDiffers();
    TestBinaryHash();
    if (g_fail) { std::printf("test_hsm_build: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_build: ok\n");
    return 0;
}
