// Unit tests for hsm_career.h. Run via implementation/shared/run_tests.sh test_hsm_career.
// #620: a Progression win pays the due purse and spends it, a loss spends it too (the bet stays lost, stock). And the rest of
// a co-op fight's end on each player's career: the purse and reputation, a Tourney's rank-up, the start body, the wire.
#include "session/hsm_career.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestPayout() {
    { const ProgPayout p = ProgPayoutFor(true, 250); CHECK(p.fundsDelta == 250 && p.dueAfter == 0); }    // a win pays the purse
    { const ProgPayout p = ProgPayoutFor(false, 250); CHECK(p.fundsDelta == 0 && p.dueAfter == 0); }    // a loss: the bet stays lost, nothing pending
    { const ProgPayout p = ProgPayoutFor(true, 0); CHECK(p.fundsDelta == 0 && p.dueAfter == 0); }       // nothing due pays nothing
    { const ProgPayout p = ProgPayoutFor(false, 0); CHECK(p.fundsDelta == 0 && p.dueAfter == 0); }
    { const ProgPayout p = ProgPayoutFor(true, -9); CHECK(p.fundsDelta == 0 && p.dueAfter == 0); }      // a negative purse is clamped to none
    { const ProgPayout p = ProgPayoutFor(false, -9); CHECK(p.fundsDelta == 0 && p.dueAfter == 0); }
}

static CareerRank Rank(int64_t funds, int64_t rep, int tier, int perm) { CareerRank c; c.funds = funds; c.reputation = rep; c.tier = tier; c.permanentTier = perm; return c; }

static void TestWin() {
    const CareerChange r = CareerAfterFight(Rank(40, 30, 2, 3), true, 75, false);
    CHECK(r.after == Rank(115, 30 + 37, 2, 3));                  // the purse, and half of it (integer) as reputation
    CHECK(r.purse == 75 && !r.tourneyWon && !r.rankedUp);
    CHECK(CareerAfterFight(Rank(40, 30, 2, 3), true, 0, false).after == Rank(40, 30, 2, 3));   // nothing due pays nothing
    CHECK(CareerAfterFight(Rank(40, 30, 2, 3), true, 1, false).after == Rank(41, 30, 2, 3));   // half of 1 is 0
}

static void TestLoss() {
    for (bool tourney : {false, true}) {
        const CareerChange r = CareerAfterFight(Rank(40, 30, 2, 3), false, 75, tourney);
        CHECK(r.after == Rank(40, 30, 2, 3) && r.purse == 0 && !r.tourneyWon && !r.rankedUp);   // the fee went at the start; nothing else moves
    }
}

static void TestTourney() {
    {   // a rank up, the reputation starting again from the purse's half, the permanent tier following
        const CareerChange r = CareerAfterFight(Rank(10, 120, 1, 1), true, 60, true);
        CHECK(r.after == Rank(70, 30, 2, 2) && r.tourneyWon && r.rankedUp && r.purse == 60);
    }
    {   // a permanent tier above the new one stays
        const CareerChange r = CareerAfterFight(Rank(10, 500, 3, 6), true, 0, true);
        CHECK(r.after == Rank(10, 0, 4, 6) && r.rankedUp);
    }
    {   // Lord is the top: no rank up, but the Tourney's other effects (reputation 0, nemeses cleared) still come
        const CareerChange r = CareerAfterFight(Rank(10, 1500, kTopRank, kTopRank), true, 100, true);
        CHECK(r.after == Rank(110, 50, kTopRank, kTopRank) && r.tourneyWon && !r.rankedUp);
    }
    {   // a corrupt tier is brought into range
        CHECK(CareerAfterFight(Rank(0, 0, 40, -3), true, 0, true).after == Rank(0, 0, kTopRank, kTopRank));
        CHECK(CareerAfterFight(Rank(0, 0, -5, 0), true, 0, true).after == Rank(0, 0, 0, 0));
    }
}

static void TestLimits() {
    CHECK(CareerAfterFight(Rank(0, 0, 0, 0), true, int64_t(kMaxPurse) + 5, false).after == Rank(kMaxPurse, kMaxPurse / 2, 0, 0));   // a misread purse is capped
    CHECK(CareerAfterFight(Rank(0, 0, 0, 0), true, -20, false).after == Rank(0, 0, 0, 0));                                       // a negative one pays nothing
    CHECK(CareerAfterFight(Rank(kMaxCareerMoney - 3, kMaxCareerMoney, 0, 0), true, 50, false).after == Rank(kMaxCareerMoney, kMaxCareerMoney, 0, 0));
    CHECK(CareerAfterFight(Rank(-70, -8, 0, 0), true, 10, false).after == Rank(10, 5, 0, 0));    // a negative purse or reputation counts as 0
}

static void TestSpawnBody() {
    CHECK(CareerBodyAtSpawn(false, 1));                 // a Progression arena fight: the career's body
    CHECK(CareerBodyAtSpawn(false, 2));                 // an errand
    CHECK(!CareerBodyAtSpawn(false, kSparringGameMode));   // sparring starts whole
    CHECK(!CareerBodyAtSpawn(true, 1));                 // Free Mode (every lobby fight) starts whole
    CHECK(!CareerBodyAtSpawn(true, kSparringGameMode));
}

// The reported Carnage where no bot hurt another, and the Doubles mercenary adopted as an enemy.
static void TestStockSides() {
    CHECK(StockSideOf(true, 1, 4) == StockSide::Ally);                    // the Doubles mercenary
    CHECK(StockSideOf(true, 0, kCarnageCombatMode) == StockSide::FreeForAll);
    CHECK(StockSideOf(true, 2, 1) == StockSide::Foe);
    CHECK(StockSideOf(true, -1, kCarnageCombatMode) == StockSide::FreeForAll);   // unreadable Team Int: by the mode
    CHECK(StockSideOf(true, -1, 1) == StockSide::Foe);
    CHECK(StockSideOf(false, 1, 4) == StockSide::Foe && StockSideOf(false, 0, kCarnageCombatMode) == StockSide::Foe);   // Survival's bots
    CHECK(MatchTeamOf(StockSide::Ally) == kHumanTeam && MatchTeamOf(StockSide::Foe) == kBotTeam && MatchTeamOf(StockSide::FreeForAll) == kBotTeam);
    const int8_t ffa = HitTeamOf(StockSide::FreeForAll), foe = HitTeamOf(StockSide::Foe), ally = HitTeamOf(StockSide::Ally);
    CHECK(HitAllowed(true, ffa, ffa, false));                             // Carnage: bot on bot lands (the report)
    CHECK(!HitAllowed(true, foe, foe, false));                            // two stock opponents never fight (stock teams)
    CHECK(HitAllowed(true, ally, foe, false) && HitAllowed(true, foe, ally, false));   // the mercenary and the foes
    CHECK(!HitAllowed(true, kHumanTeam, ally, false) && HitAllowed(true, kHumanTeam, ally, true));   // a player and the ally
    CHECK(HitAllowed(true, kHumanTeam, ffa, false) && HitAllowed(true, ffa, kHumanTeam, false));
    // the win: every opponent out; allies never need to fall
    using S = StockSide;
    CHECK(CoopFoesDown({{S::Ally, false}, {S::Foe, true}}));
    CHECK(!CoopFoesDown({{S::Ally, true}, {S::Foe, false}}));
    CHECK(!CoopFoesDown({{S::Ally, false}}));                              // only the mercenary seen yet: not won
    CHECK(CoopFoesDown({{S::FreeForAll, true}, {S::FreeForAll, true}, {S::FreeForAll, true}}));
    CHECK(!CoopFoesDown({{S::FreeForAll, true}, {S::FreeForAll, false}}));
    CHECK(!CoopFoesDown({}));
}

// The reported Brawl with a weapon: a career Brawl starts everyone in pants, Blossfechten in its training gear.
static void TestSpawnKit() {
    CareerSpawnKit k = CareerSpawnKitFor(false, kFistingCombatMode);
    CHECK(k.pants && !k.blossfechten);
    k = CareerSpawnKitFor(false, kBlossfechtenCombatMode);
    CHECK(!k.pants && k.blossfechten);
    for (int mode : {1, 2, 3, 4, 5, 6, 7, kTourneyCombatMode, 9, -1}) {     // Duel ... Bridge, and an unreadable mode: own kit
        k = CareerSpawnKitFor(false, mode);
        CHECK(!k.pants && !k.blossfechten);
    }
    for (int mode : {kFistingCombatMode, kBlossfechtenCombatMode}) {        // Free Mode: the lobby's rows decide, not the career's
        k = CareerSpawnKitFor(true, mode);
        CHECK(!k.pants && !k.blossfechten);
    }
}

static void TestWire() {
    ProgResultMsg m; m.fight = 0x01020304; m.flags = kResultWon | kResultTourney; m.purse = 240;
    std::vector<uint8_t> b = Encode(m);
    MsgType t; CHECK(PeekType(b.data(), b.size(), t) && t == MsgType::ProgressionResult && uint8_t(t) == 69);
    CHECK(kLastDecodedMsgType == 69 && kLastReservedMsgType == 69);
    CHECK(b.size() == 4 + 4 + 1 + 4);
    ProgResultMsg o;
    CHECK(Decode(b.data(), b.size(), o) && o.fight == 0x01020304 && o.won() && o.tourney() && o.purse == 240);
    for (size_t cut = 0; cut < b.size(); ++cut) CHECK(!Decode(b.data(), cut, o));       // every truncation
    { std::vector<uint8_t> x = b; x.push_back(0); CHECK(!Decode(x.data(), x.size(), o)); }
    auto ok = [](ProgResultMsg x) { ProgResultMsg y; std::vector<uint8_t> e = Encode(x); return Decode(e.data(), e.size(), y); };
    { ProgResultMsg x; CHECK(ok(x)); }                                                  // a loss, nothing paid
    { ProgResultMsg x; x.flags = kResultTourney; CHECK(ok(x)); }                         // a Tourney lost
    { ProgResultMsg x; x.flags = kResultWon; x.purse = kMaxPurse; CHECK(ok(x)); }
    { ProgResultMsg x; x.flags = kResultWon; x.purse = kMaxPurse + 1; CHECK(!ok(x)); }   // a misread purse is refused
    { ProgResultMsg x; x.flags = 0; x.purse = 5; CHECK(!ok(x)); }                         // a purse for a lost fight
    { ProgResultMsg x; x.flags = 16; CHECK(!ok(x)); }                                     // unknown flag bits
    { ProgResultMsg x; x.flags = 0xFF; x.purse = 1; CHECK(!ok(x)); }
    // rev 41: a round won with more to come
    { ProgResultMsg x; x.flags = kResultWon | kResultNextRound; CHECK(ok(x)); x.flags |= kResultTimeStep; CHECK(ok(x)); x.flags |= kResultTourney; CHECK(ok(x)); }
    { ProgResultMsg x; x.flags = kResultNextRound; CHECK(!ok(x)); }                       // a next round is a round won
    { ProgResultMsg x; x.flags = kResultWon | kResultNextRound; x.purse = 5; CHECK(!ok(x)); }   // nothing paid before the end
    { ProgResultMsg x; x.flags = kResultWon | kResultTimeStep; CHECK(!ok(x)); }           // the step flag only between rounds
    { ProgResultMsg x; x.flags = kResultWon | kResultNextRound; CHECK(x.nextRound() && !x.timeStep()); x.flags |= kResultTimeStep; CHECK(x.timeStep()); }
    { ProgResultMsg x; x.flags = kResultWon; CHECK(!x.nextRound() && x.timeStep()); x.flags = 0; CHECK(x.timeStep()); }   // a contest's end steps
    std::vector<uint8_t> other = b; other[3] = uint8_t(MsgType::ProgressionVote); CHECK(!Decode(other.data(), other.size(), o));
}

// The reported one-round Tourney: a contest plays its rounds, a time step every third, the champion before the last.
static void TestContest() {
    auto step = [](int rw, int rtw, int gm, int cm, int left = 5) { return ContestAfterFight(true, rw, rtw, gm, cm, left); };
    // a Tourney of 7 rounds, 14 opponents: six next rounds, then the win
    int steps = 0, finals = 0;
    for (int rw = 0; rw < 6; ++rw) {
        const ContestAfter a = step(rw, 7, kArenaGameMode, kTourneyCombatMode);
        CHECK(a.step == ContestStep::NextRound && a.roundsWonAfter == rw + 1);
        CHECK(a.timeStep == ((rw + 1) % 3 == 0));
        steps += a.timeStep; finals += a.tourneyFinal;
        CHECK(a.tourneyFinal == (rw + 1 == 6));
    }
    CHECK(steps == 2 && finals == 1);                        // rounds 3 and 6 step; the champion comes before round 7
    ContestAfter last = step(6, 7, kArenaGameMode, kTourneyCombatMode);
    CHECK(last.step == ContestStep::Won && last.roundsWonAfter == 7 && last.timeStep && !last.tourneyFinal);
    // a Duel of 3: two next rounds, the third wins; never a champion
    CHECK(step(0, 3, kArenaGameMode, 1).step == ContestStep::NextRound && !step(1, 3, kArenaGameMode, 1).tourneyFinal);
    CHECK(step(2, 3, kArenaGameMode, 1).step == ContestStep::Won);
    // one-fight contests, errands (Rounds to WIn 0) and training (game mode 3): won at once
    CHECK(step(0, 1, kArenaGameMode, 1).step == ContestStep::Won);
    CHECK(step(0, 0, 2, 1).step == ContestStep::Won && step(0, 0, 2, 1).roundsWonAfter == 0);
    CHECK(step(0, 1, 3, 1).step == ContestStep::Won && step(0, 5, 3, 1).step == ContestStep::Won);
    // a round lost loses the contest, whichever round it was; the count stays
    for (int rw : {0, 3, 6}) {
        const ContestAfter a = ContestAfterFight(false, rw, 7, kArenaGameMode, kTourneyCombatMode, 5);
        CHECK(a.step == ContestStep::Lost && a.roundsWonAfter == rw && a.timeStep && !a.tourneyFinal);
    }
    // misreads: one fight, as before
    CHECK(step(-1, 7, kArenaGameMode, 1).step == ContestStep::Won && step(-1, 7, kArenaGameMode, 1).roundsWonAfter == -1);
    CHECK(step(0, kMaxContestRounds + 1, kArenaGameMode, 1).step == ContestStep::Won);
    CHECK(step(0, -3, kArenaGameMode, 1).step == ContestStep::Won);
    CHECK(step(9, 7, kArenaGameMode, 1).step == ContestStep::Won && step(9, 7, kArenaGameMode, 1).roundsWonAfter == 9);
    CHECK(step(0, kMaxContestRounds, kArenaGameMode, 1).step == ContestStep::NextRound);
    // nobody left to fight: won as it stands, but a Tourney's final brings its champion
    CHECK(step(1, 3, kArenaGameMode, 1, 0).step == ContestStep::Won);
    CHECK(step(1, 3, kArenaGameMode, 1, -1).step == ContestStep::NextRound);          // unreadable: the rounds go on
    CHECK(step(5, 7, kArenaGameMode, kTourneyCombatMode, 0).step == ContestStep::NextRound && step(5, 7, kArenaGameMode, kTourneyCombatMode, 0).tourneyFinal);
    CHECK(TourneyChampionBudget(0) == 50 && TourneyChampionBudget(8) == 2000 && TourneyChampionBudget(-1) == 50 && TourneyChampionBudget(99) == 2000);
}

// Opponents out of the fight: given up, downed or dead, and they stay out.
static void TestOutAndResults() {
    OutLatch l;
    CHECK(!l.Note(10, false) && !l.Out(10));
    CHECK(l.Note(10, true) && l.Out(10));
    CHECK(l.Note(10, false));                                // a "Get Up, Pal" clearing Give Up: still out
    CHECK(!l.Note(11, false));
    l.Forget(10); CHECK(!l.Out(10));
    l.Note(12, true); l.Clear(); CHECK(!l.Out(12));
    CHECK(CoopResultsMs(true, true) == kCoopClaimFallbackMs && CoopResultsMs(true, false) == kCoopResultsMs);
    CHECK(CoopResultsMs(false, true) == kCoopResultsMs && CoopResultsMs(false, false) == kCoopResultsMs);
}

static void TestOnce() {
    ResultOnce once;
    CHECK(once.Take(7));
    CHECK(!once.Take(7));                               // the same fight again: not applied twice
    CHECK(once.Take(8));
    CHECK(!once.Take(8));
    CHECK(once.Take(7));                                // another fight (a travel number is no order): taken
    ResultOnce fresh;
    CHECK(fresh.Take(0));                               // fight 0 is a fight like any
    CHECK(!fresh.Take(0));
}

// The reported loss of a won fight: a Progression co-op match (one player, the event's opponent adopted as a bot) driven to
// its end and on to the rematch event, where the host applies the fight's end.
static void TestOutcomeAtRematch() {
    for (int winner : {int(kHumanTeam), int(kBotTeam)}) {
        MatchConfig c; c.format = Format::PlayersVsBots; c.scoreLimit = 100; c.timeLimitS = 0;
        Match m; m.SetResultsMs(3000);
        m.Start(c, {{1, kHumanTeam, true}}, 0);
        CHECK(m.Tick(Match::kCountdownMs).live);
        m.AddFighter({7, kBotTeam, false});
        m.Finish(winner, Match::kCountdownMs + 100);
        CoopOutcome o;
        bool ended = false, rematch = false;
        for (uint32_t t = Match::kCountdownMs + 100; t < Match::kCountdownMs + 10000 && !rematch; t += 50) {
            const Match::Events ev = m.Tick(t);
            if (ev.ended) { ended = true; CHECK(m.winner() == winner); o.Ended(m.winner()); }
            rematch = ev.rematch;
        }
        CHECK(ended && rematch);
        CHECK(m.winner() == Match::kNoWinner);              // the trap: by the rematch the match has forgotten who won
        CHECK(o.TakeWon() == (winner == kHumanTeam));
        CHECK(!o.TakeWon());                                // once per fight
    }
    CoopOutcome none;
    CHECK(!none.TakeWon());                                 // a fight that never ended is no win
}

int main() {
    TestPayout();
    TestWin();
    TestLoss();
    TestTourney();
    TestLimits();
    TestSpawnBody();
    TestSpawnKit();
    TestStockSides();
    TestWire();
    TestContest();
    TestOutAndResults();
    TestOnce();
    TestOutcomeAtRematch();
    if (g_fail) { std::printf("test_hsm_career: %d failure(s)\n", g_fail); return 1; }
    std::puts("test_hsm_career: ok");
    return 0;
}
