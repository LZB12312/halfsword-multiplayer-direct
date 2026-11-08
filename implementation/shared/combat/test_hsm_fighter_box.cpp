// Unit tests for hsm_fighter_box.h. Run via implementation/shared/run_tests.sh test_hsm_fighter_box.
//
// The failing case is the report's (#1679): a player's arms stop behind another player, whose stand-in the game's own
// trace cannot find. Both directions have to be cleared, as the game does when both fighters tick.
#include "combat/hsm_fighter_box.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm::combat;

static bool Has(const std::vector<FighterBoxPair>& pairs, size_t body, size_t box) {
    for (const FighterBoxPair& p : pairs) if (p.body == body && p.box == box) return true;
    return false;
}

int main() {
    // The report: one stand-in (0), my fighter (1). My body against its box, and its body against mine.
    {
        const auto pairs = FighterBoxPairs(1, 1);
        CHECK(pairs.size() == 2);
        CHECK(Has(pairs, 1, 0));
        CHECK(Has(pairs, 0, 1));
    }
    // A fighter's own box keeps stopping its own arms.
    {
        const auto pairs = FighterBoxPairs(2, 2);
        for (size_t i = 0; i < 4; ++i) CHECK(!Has(pairs, i, i));
    }
    // Two stand-ins: neither has a tick, so nothing else clears them.
    {
        const auto pairs = FighterBoxPairs(2, 0);
        CHECK(pairs.size() == 2);
        CHECK(Has(pairs, 0, 1));
        CHECK(Has(pairs, 1, 0));
    }
    // Two live fighters (a player and a bot) are the game's own pair, and nothing is called with no stand-in.
    {
        CHECK(FighterBoxPairs(0, 2).empty());
        const auto pairs = FighterBoxPairs(1, 2);
        CHECK(!Has(pairs, 1, 2));
        CHECK(!Has(pairs, 2, 1));
        CHECK(pairs.size() == 4);
    }
    CHECK(FighterBoxPairs(0, 0).empty());
    CHECK(FighterBoxPairs(1, 0).empty());

    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_fighter_box: ok\n");
    return 0;
}
