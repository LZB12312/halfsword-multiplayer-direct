// Unit tests for the build card (hsm_build_card.h). Run via implementation/shared/run_tests.sh test_hsm_build_card.
#include "match/hsm_build_card.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

int main() {
    // Stock UE4SS mods are left out; the rest keep their order.
    CHECK(CardModsFrom("BPModLoaderMod, HSEnhancer, Keybinds, HalfWolverine") == "HSEnhancer, HalfWolverine");
    CHECK(CardModsFrom("").empty());
    CHECK(CardModsFrom("Keybinds").empty());

    // Round trip, and '_' from a command line reads as a space.
    BuildCard c = DecodeCard("Bob", EncodeCard("Glass_Cannon", "HSEnhancer, HalfWolverine"));
    CHECK(c.player == "Bob" && c.build == "Glass Cannon" && c.mods == "HSEnhancer, HalfWolverine");

    // Peer-written: no separator, control characters, overlong and cut-up UTF-8 all stay safe.
    c = DecodeCard("Bob", "OnlyBuild");
    CHECK(c.build == "OnlyBuild" && c.mods.empty());
    c = DecodeCard("B\x01ob", "Bu\x02ild\nM\x03ods\nextra");
    CHECK(c.player == "Bob" && c.build == "Build" && c.mods == "Modsextra");
    CHECK(DecodeCard("x", std::string(5000, 'a')).build.size() == kMaxCardBuild);
    CHECK(DecodeCard("x", "b\n" + std::string(5000, 'a')).mods.size() <= kMaxCardMods);
    // A long list is not cut at one chat line, and what cannot fit is still counted.
    std::string many, huge;
    for (int i = 0; i < 12; ++i) many += (i ? ", Mod" : "Mod") + std::to_string(100000 + i) + "abcdefghij";
    CHECK(CleanCardMods(many) == many);
    for (int i = 0; i < 40; ++i) huge += (i ? ", Mod" : "Mod") + std::to_string(100000 + i) + "abcdefghijklmnop";
    const std::string cut = CleanCardMods(huge);
    CHECK(cut.size() <= kMaxCardMods && cut.find(" more") != std::string::npos);
    std::string euros;
    for (int i = 0; i < 40; ++i) euros += "\xE2\x82\xAC";
    CHECK(CleanCardField(euros, kMaxCardBuild).size() == 24);   // 8 whole characters of 3 bytes
    CHECK(CleanCardField(euros, 23).size() == 21);               // cut on a character, not inside one
    CHECK(CardPrefix("a\xE2\x82\xAC" "b", 2) == "a\xE2\x82\xAC");

    // Waits for the HUD; a newer card from the same player replaces the waiting one.
    BuildCardQueue q;
    q.Push({"Bob", "Fair", ""});
    q.Push({"Bob", "Tank", "A"});
    q.Push({"Ann", "Duelist", "B"});
    CHECK(!q.Tick(0, false).visible);
    BuildCardView v = q.Tick(1000, true);
    CHECK(v.visible && v.typing && v.player.empty() && v.opacity == 1.0f);
    v = q.Tick(1000 + kCardRevealMs, true);
    CHECK(!v.typing && v.player == "PLAYER: Bob" && v.build == "BUILD:  TANK" && v.mods == "MODS:   A");
    v = q.Tick(1000 + kCardRevealMs + kCardHoldMs + kCardFadeMs / 2, true);
    CHECK(v.visible && v.opacity > 0.4f && v.opacity < 0.6f);
    CHECK(!q.Tick(1000 + kCardRevealMs + kCardHoldMs + kCardFadeMs, true).visible);   // Bob's is over
    v = q.Tick(10000, true);                                                         // Ann's starts now
    CHECK(v.typing);
    v = q.Tick(10000 + kCardRevealMs, true);
    CHECK(v.player == "PLAYER: Ann" && v.build == "BUILD:  DUELIST");

    {   // The class checker key: brings cards back, holds them, steps through them and sends them away.
        BuildCardQueue c;
        c.Toggle(0);
        CHECK(c.Idle());                                                              // nobody yet: nothing to show
        c.Push({"Bob", "Tank", ""});
        c.Push({"Ann", "Duelist", ""});
        (void)c.Tick(0, true);
        (void)c.Tick(kCardRevealMs, true);
        c.Toggle(kCardRevealMs);                                                      // held while it's showing
        BuildCardView h = c.Tick(100000, true);
        CHECK(h.visible && h.opacity == 1.0f && !h.typing && h.player == "PLAYER: Bob");
        c.Toggle(100000);                                                             // the next player
        CHECK(c.Tick(200000, true).player == "PLAYER: Ann");
        c.Toggle(200000);                                                             // past the last: gone early
        CHECK(!c.Tick(200001, true).visible);                                         // gone early, and Ann's waiting card has been seen
        CHECK(!c.Tick(900000, true).visible);
        c.Toggle(900000);                                                             // back on demand after fading
        CHECK(c.Tick(900001, true).player == "PLAYER: Bob");
    }

    // No build or mods say so, and a HUD going away hides the card.
    q.Clear();
    q.Push({"Cy", "", ""});
    (void)q.Tick(0, true);
    v = q.Tick(kCardRevealMs, true);
    CHECK(v.build == "BUILD:  NONE" && v.mods == "MODS:   none");
    CHECK(!q.Tick(kCardRevealMs + 1, false).visible);
    CHECK(q.Idle());

    // Time wrap.
    q.Push({"Di", "X", ""});
    (void)q.Tick(0xFFFFFF00u, true);
    CHECK(q.Tick(0xFFFFFF00u + 100, true).visible);

    if (g_fail == 0) std::printf("ok\n");
    return g_fail != 0;
}
