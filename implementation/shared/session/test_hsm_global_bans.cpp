// Unit tests for hsm_global_bans.h. Run via implementation/shared/run_tests.sh test_hsm_global_bans.
#include "session/hsm_global_bans.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

int main() {
    // The hash, as Python's hashlib gives it for b"hsm-ban:76561198012345678".
    CHECK(GlobalBanHash(76561198012345678ull) == "bd47c022cd376e586bc9a3f5e7890ef07eaa97d29f95f562b04a8d71b37f96d2");
    static constexpr std::string_view list[] = {"bd47c022cd376e586bc9a3f5e7890ef07eaa97d29f95f562b04a8d71b37f96d2"};
    CHECK(GloballyBannedIn(76561198012345678ull, list));
    CHECK(!GloballyBannedIn(76561198012345677ull, list) && !GloballyBannedIn(76561198012345679ull, list) && !GloballyBannedIn(0, list));
    // The real list: well-formed entries, and nobody it doesn't name.
    for (std::string_view b : kGlobalBans) {
        CHECK(b.size() == 64);
        for (char c : b) CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
    CHECK(!GloballyBanned(76561198012345678ull) && !GloballyBanned(0));
    if (g_fail) { std::printf("test_hsm_global_bans: %d failure(s)\n", g_fail); return 1; }
    std::printf("PASS test_hsm_global_bans\n");
    return 0;
}
