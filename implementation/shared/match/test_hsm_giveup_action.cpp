// What held G means per format, phase and fighter state (GiveUpActionFor). The bug: in Survival and Zombies (Abyss) a
// fought wave gave G no meaning at all, so a stuck or crippled player had no way to get a fresh fighter.
#include "match/hsm_giveup_action.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static MatchInfo Info(Format f, MatchPhase p = MatchPhase::Live, uint32_t gapLeftMs = 0) {
    MatchInfo m; m.format = uint8_t(f); m.phase = uint8_t(p); m.roundGapLeftMs = gapLeftMs;
    return m;
}
static GiveUpAction Alive(const MatchInfo& m, bool oneFight = false) { return GiveUpActionFor(m, true, false, oneFight); }
static GiveUpAction Out(const MatchInfo& m, bool oneFight = false) { return GiveUpActionFor(m, false, true, oneFight); }

static void TestSurvivalWave() {
    CHECK(Alive(Info(Format::PlayersVsBots)) == GiveUpAction::GiveUp);                        // the reported bug
    CHECK(Alive(Info(Format::PlayersVsBots, MatchPhase::SuddenDeath)) == GiveUpAction::GiveUp);
    CHECK(Out(Info(Format::PlayersVsBots)) == GiveUpAction::Waiting);                         // dead or waiting: nothing
    CHECK(GiveUpActionFor(Info(Format::PlayersVsBots), false, false, false) == GiveUpAction::None);   // not counted, body here
}

static void TestSurvivalBreak() {
    const MatchInfo br = Info(Format::PlayersVsBots, MatchPhase::Live, 30000);
    CHECK(Alive(br) == GiveUpAction::SkipBreak);                                              // the break keeps its vote
    CHECK(Out(br) == GiveUpAction::SkipBreak);                                                // a downed player still votes
}

static void TestZombies() {
    CHECK(Alive(Info(Format::Zombies)) == GiveUpAction::GiveUp);
    CHECK(Out(Info(Format::Zombies)) == GiveUpAction::Waiting);
    CHECK(Alive(Info(Format::Zombies, MatchPhase::Live, 10000)) == GiveUpAction::SkipBreak);
    CHECK(Alive(Info(Format::Zombies), true) == GiveUpAction::GiveUp);                        // oneFight is a Survival thing only
}

static void TestProgressionUnchanged() {
    const MatchInfo fight = Info(Format::PlayersVsBots);
    CHECK(Alive(fight, true) == GiveUpAction::GiveUpFight);                                   // the fight is lost, as before
    CHECK(Out(fight, true) == GiveUpAction::Waiting);
    CHECK(Alive(Info(Format::PlayersVsBots, MatchPhase::Live, 5000), true) == GiveUpAction::SkipBreak);
}

static void TestPvpUnchanged() {
    for (Format f : {Format::FreeForAll, Format::TwoTeams}) {
        CHECK(Alive(Info(f)) == GiveUpAction::GiveUp);
        CHECK(Alive(Info(f, MatchPhase::SuddenDeath)) == GiveUpAction::GiveUp);
        CHECK(Out(Info(f)) == GiveUpAction::Waiting);
    }
    CHECK(Alive(Info(Format::Duel)) == GiveUpAction::GiveUp);
    CHECK(Alive(Info(Format::Duel, MatchPhase::Live, 3000)) == GiveUpAction::Waiting);        // the round gap: nothing to give up
    CHECK(Out(Info(Format::Duel, MatchPhase::Live, 3000)) == GiveUpAction::Waiting);
    CHECK(Out(Info(Format::Duel)) == GiveUpAction::None);                                     // Duel waits only in the gap (as before)
    CHECK(Alive(Info(Format::LastManStanding)) == GiveUpAction::GiveUp);                      // Last Man Standing: Duel's rules
    CHECK(Alive(Info(Format::LastManStanding, MatchPhase::Live, 3000)) == GiveUpAction::Waiting);
    CHECK(Out(Info(Format::LastManStanding, MatchPhase::Live, 3000)) == GiveUpAction::Waiting);
    CHECK(Out(Info(Format::LastManStanding)) == GiveUpAction::None);
}

static void TestNotFought() {
    for (Format f : {Format::FreeForAll, Format::Duel, Format::LastManStanding, Format::PlayersVsBots, Format::Zombies})
        for (MatchPhase p : {MatchPhase::Idle, MatchPhase::Countdown, MatchPhase::Results}) {
            CHECK(Alive(Info(f, p)) == GiveUpAction::None);
            CHECK(Out(Info(f, p)) == GiveUpAction::None);
        }
    CHECK(Alive(Info(Format::PlayersVsBots, MatchPhase::Countdown, 5000)) == GiveUpAction::None);   // a gap outside a fight is not a break
}

// The report: a Progression fight won should wait for G, as the stock game does, not go back on its own.
static void TestClaimWin() {
    MatchInfo won = Info(Format::PlayersVsBots, MatchPhase::Results); won.winner = kHumanTeam;
    CHECK(Alive(won, true) == GiveUpAction::ClaimWin && Out(won, true) == GiveUpAction::ClaimWin);   // the dead go on too
    CHECK(CoopFightWon(won, true) && !CoopFightWon(won, false));
    CHECK(Alive(won) == GiveUpAction::None);                                                  // Survival's results: as before
    MatchInfo lost = won; lost.winner = kBotTeam;
    CHECK(Alive(lost, true) == GiveUpAction::None && !CoopFightWon(lost, true));              // a loss goes back by itself
    MatchInfo live = won; live.phase = uint8_t(MatchPhase::Live);
    CHECK(Alive(live, true) == GiveUpAction::GiveUpFight);                                    // in the fight G still gives it up
    MatchInfo duel = won; duel.format = uint8_t(Format::Duel);
    CHECK(Alive(duel, true) == GiveUpAction::None);
    CHECK(StatusLine(won, 1, true).find("hold G") != std::string::npos && StatusLine(lost, 1, true).find("Tavern") != std::string::npos);
    // blows still count while the won fight waits for G (the report: the opponents who gave up were invincible)
    auto blows = [](const MatchInfo& m, bool oneFight) { return BlowsCount(Format(m.format), MatchPhase(m.phase), m.winner, oneFight); };
    CHECK(blows(won, true) && blows(live, true) && blows(live, false));
    CHECK(!blows(won, false) && !blows(lost, true) && !blows(duel, true));                     // other results: no blows, as before
    for (MatchPhase p : {MatchPhase::Idle, MatchPhase::Countdown}) { MatchInfo m = won; m.phase = uint8_t(p); CHECK(!blows(m, true)); }
}

int main() {
    TestClaimWin();
    TestSurvivalWave();
    TestSurvivalBreak();
    TestZombies();
    TestProgressionUnchanged();
    TestPvpUnchanged();
    TestNotFought();
    if (g_fail) { std::printf("%d give-up action checks failed\n", g_fail); return 1; }
    std::printf("all give-up action tests passed\n");
    return 0;
}
