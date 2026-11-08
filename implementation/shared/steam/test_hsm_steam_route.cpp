// Steam route description: POP codes decode as Steam packs them, the relayed flag or a relay POP marks a relayed
// session, and the watch reports a route the first time and on each change only.
#include "steam/hsm_steam_route.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::steam;

// CalculateSteamNetworkingPOPIDFromString (steamnetworkingtypes.h).
static uint32_t Pop(const char* s) {
    uint32_t r = uint32_t(uint8_t(s[0])) << 16;
    if (r && s[1]) {
        r |= uint32_t(uint8_t(s[1])) << 8;
        if (s[2]) r |= uint32_t(uint8_t(s[2])) | (uint32_t(uint8_t(s[3])) << 24);
    }
    return r;
}

static void TestPopName() {
    CHECK(PopName(Pop("fra")) == "fra");
    CHECK(PopName(Pop("iad")) == "iad");
    CHECK(PopName(Pop("sgp1")) == "sgp1");
    CHECK(PopName(0).empty());
    CHECK(PopName(0x00010203u).empty());   // not printable
}

static void TestDescribe() {
    CHECK(Describe(RouteFrom(0, 0, 0)) == "direct");
    CHECK(Describe(RouteFrom(kConnFlagFast, 0, 0)) == "direct (LAN)");
    CHECK(Describe(RouteFrom(kConnFlagRelayed, Pop("fra"), Pop("iad"))) == "relayed via fra, far end near iad");
    CHECK(Describe(RouteFrom(kConnFlagRelayed, Pop("fra"), Pop("fra"))) == "relayed via fra");
    CHECK(Describe(RouteFrom(kConnFlagRelayed, 0, 0)) == "relayed");
    // A relay POP without the flag is still relayed.
    CHECK(RouteFrom(0, Pop("ams"), 0).relayed);
}

static void TestWatch() {
    RouteWatch w;
    const Route a = RouteFrom(kConnFlagRelayed, Pop("fra"), Pop("iad"));
    const Route b = RouteFrom(0, 0, 0);
    CHECK(w.Note(7, a));
    CHECK(!w.Note(7, a));
    CHECK(w.Note(7, b));
    CHECK(!w.Note(7, b));
    CHECK(w.Note(8, b));
    w.Forget(7);
    CHECK(w.Note(7, b));
}

int main() {
    TestPopName();
    TestDescribe();
    TestWatch();
    if (g_fail) { std::printf("test_hsm_steam_route: %d failures\n", g_fail); return 1; }
    std::printf("test_hsm_steam_route: ok\n");
    return 0;
}
