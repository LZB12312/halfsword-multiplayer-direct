// A Progression co-op fight's end on the players' careers (pure, no engine types). Each player's career is their own (the
// multiplayer career, session/hsm_mp_career.h, never the player's own save).
//
// The purse (#620). The host took the fight from its own board and paid its fee; the stock win screen's Claim Reward that
// pays the purse (Due Reward) is suppressed in a session. ProgPayoutFor: a win pays it, a loss keeps the bet lost (stock),
// and either way the host's pending Due Reward is spent, so it cannot pay out twice.
//
// The rest of the stock end (owner decisions 2026-11-03). The stock claim, lose and death screens (UI_NextFIght, UI_Lose,
// UI_DeathDoor) never run in a session, so the host tells every player how the fight went (ProgressionResult 69, before the
// Travel back to the Tavern) and each machine does to its own career what those screens would have:
//   the game's own capture  GI_Settings Override Player Character Equipment(false): each limb heals 10, between 25 and 100,
//                           and a clean win keeps the gear held and worn (decomp GI:765-953). A death is no exception: no
//                           Hell, no new character, only the hurt it left.
//   a win                   every player is paid the whole purse, and half of it as reputation on their rank; a Tourney won
//                           ranks every player up (CareerAfterFight)
//   the time                one step on (Skip Time Event), by the host's clock: its Travel carries Day Time and Day Count
// A fight's start carries the body in: the mod spawns the players itself, so it sets the stock spawner's start body
// (CareerBodyAtSpawn).
#pragma once
#include "hsm_protocol.h"
#include <algorithm>
#include <climits>
#include <cstdint>
#include <set>
#include <vector>

namespace hsm {

struct ProgPayout { int64_t fundsDelta = 0; int64_t dueAfter = 0; };
inline ProgPayout ProgPayoutFor(bool won, int64_t dueReward) {
    if (dueReward < 0) dueReward = 0;   // a corrupt negative purse is no purse: nothing is paid and nothing stays due
    return won ? ProgPayout{dueReward, 0} : ProgPayout{0, 0};
}

constexpr int kTopRank = 8;                     // Enum_Ranks: 0 Beggar .. 8 Lord
constexpr int kTourneyCombatMode = 8;           // Enum_CombatEvent_Modes: Tourney, the only fight that ranks up
constexpr int kSparringGameMode = 4;            // Enum_GameMode: Sparring (the body neither carried in nor out)
constexpr uint32_t kMaxPurse = 100000;          // a stock purse is at most 1000 (BP_CombatEvent_Master clamps it); more is a misread
constexpr int64_t kMaxCareerMoney = INT32_MAX;  // the save writes Player Funds and Reputation on Rank as IntProperty

// The stock spawner's rule for a player's start body (BP_SpawnerPoint_Willies.cpp:144): the career's Player Body Condition in
// Progression (Free Mode off) but for Sparring; a whole body otherwise.
inline bool CareerBodyAtSpawn(bool freeMode, int gameMode) { return !freeMode && gameMode != kSparringGameMode; }

// The stock spawner's kit for a career fight, set on every fighter before it finishes spawning (BP_LevelManager.cpp:408-410,
// BP_SpawnerPoint_Willies.cpp:141,145); Willie_BP reads both as it equips. A Brawl (combat mode 0, "Fisting") starts in pants:
// no weapons, the armour left off. Blossfechten (10) starts in its training gear: the right hand only, nothing sheathed. Free
// Mode's own Brawl is the lobby's (Adapter::SetBare). The mod spawns the players itself, so a Brawl had them armed.
constexpr int kFistingCombatMode = 0, kCarnageCombatMode = 3, kBlossfechtenCombatMode = 10;
struct CareerSpawnKit { bool pants = false; bool blossfechten = false; };
inline CareerSpawnKit CareerSpawnKitFor(bool freeMode, int combatMode) {
    CareerSpawnKit k;
    if (freeMode) return k;
    k.pants = combatMode == kFistingCombatMode;
    k.blossfechten = combatMode == kBlossfechtenCombatMode;
    return k;
}

// A stock fighter's side in a career fight, by the Team Int the stock spawner gave it (BP_SpawnerPoint_Willies.cpp:133): 1 the
// player's side (a hired mercenary in Doubles, Buhurt, Riot, Champion, Tourney), 0 in Carnage, 2 every other opponent. Team 0
// is no team: the stock AI and weapons take everyone for a foe (AI_BP.cpp:1014, ModularWeaponBP.cpp:510), so Carnage's
// opponents fight each other too. The mod's bots of Survival and Zombies (not a career fight) are all foes.
enum class StockSide : uint8_t { Foe = 0, Ally = 1, FreeForAll = 2 };
inline StockSide StockSideOf(bool career, int teamInt, int combatMode) {
    if (!career) return StockSide::Foe;
    if (teamInt == 1) return StockSide::Ally;
    if (teamInt == 0 || (teamInt < 0 && combatMode == kCarnageCombatMode)) return StockSide::FreeForAll;   // unreadable: by the mode
    return StockSide::Foe;
}
// The match team of an adopted fighter: an ally fights on the players' side, everyone else on the bots'.
inline int8_t MatchTeamOf(StockSide s) { return s == StockSide::Ally ? kHumanTeam : kBotTeam; }
// The team the hit gate (HitAllowed) sees. A Carnage fighter is on no team, so its blows land on every other fighter; with the
// bots' team they were all dropped as friendly fire (the report: "bots can't damage each other in Carnage").
inline int8_t HitTeamOf(StockSide s) { return s == StockSide::FreeForAll ? kNoTeam : MatchTeamOf(s); }

// A co-op fight is won once every opponent is out. Allies need not fall: the stock Enemy Count leaves them out
// (BP_SpawnerPoint_Willies.cpp:160). With no opponent adopted yet nothing is won (a mercenary seen before the foes).
struct CoopFighter { StockSide side = StockSide::Foe; bool out = false; };
inline bool CoopFoesDown(const std::vector<CoopFighter>& fighters) {
    bool any = false;
    for (const CoopFighter& f : fighters) {
        if (f.side == StockSide::Ally) continue;
        any = true;
        if (!f.out) return false;
    }
    return any;
}

struct CareerRank {
    int64_t funds = 0;        // Player Funds
    int64_t reputation = 0;   // Reputation on Rank
    int tier = 0;             // Player Tier
    int permanentTier = 0;    // Permanent Tier: the highest a new character may start at
    bool operator==(const CareerRank&) const = default;
};
struct CareerChange {
    CareerRank after;
    bool tourneyWon = false;  // Nemesis Opponents cleared; the board, the merchant and the Tavern's people renewed (NF:146-153)
    bool rankedUp = false;    // the tier went up (a Tourney won below Lord)
    int64_t purse = 0;        // what the win paid (0..kMaxPurse), and half of it the reputation
};

// A won fight (decomp UI_NextFIght NF:155-170): the purse into the funds and half of it (integer) into the reputation. A won
// Tourney first (NF:146-153): the tier one up (Lord at most), the permanent tier at least that, the reputation back to 0, so
// the purse's half is the first of the new rank. A lost fight changes none of these (the fee went at its start). A negative
// value (a corrupt career) counts as 0, and the sums stop at kMaxCareerMoney.
inline CareerChange CareerAfterFight(const CareerRank& c, bool won, int64_t purse, bool tourney) {
    CareerChange r;
    r.after = c;
    if (!won) return r;
    r.purse = std::clamp<int64_t>(purse, 0, kMaxPurse);
    if (tourney) {
        r.tourneyWon = true;
        r.after.tier = std::clamp(c.tier + 1, 0, kTopRank);
        r.after.permanentTier = std::max(std::clamp(c.permanentTier, 0, kTopRank), r.after.tier);
        r.after.reputation = 0;
        r.rankedUp = r.after.tier > c.tier;
    }
    auto add = [](int64_t a, int64_t b) { return std::min<int64_t>(std::clamp<int64_t>(a, 0, kMaxCareerMoney) + b, kMaxCareerMoney); };
    r.after.funds = add(r.after.funds, r.purse);
    r.after.reputation = add(r.after.reputation, r.purse / 2);
    return r;
}

// ------------------------------------------------------------------ a contest's rounds
// A stock contest is Rounds to WIn fights in one arena (UI_List_MapSelect sets it: a Tourney 7, a Duel 3 to 6, Doubles 2 or 3).
// UI_WinScreen counts each round won (suppressed in a session, so the mod counts it); a won round before the last plays the next
// one in the same arena, a time step every third (UI_NextFIght NF:204-210), and the next-to-last round of a Tourney brings its
// champion in (NF:85-98). The last round pays the purse; a round lost loses the contest. Errands and training are one fight.
constexpr int kArenaGameMode = 1;               // Enum_GameMode: Arena, the only mode with rounds
constexpr int kMaxContestRounds = 16;           // a stock contest has at most 7; more is a misread
enum class ContestStep : uint8_t { Lost, NextRound, Won };
struct ContestAfter {
    ContestStep step = ContestStep::Lost;
    int roundsWonAfter = 0;      // Rounds Won to write back
    bool timeStep = false;       // the time moves one step now (always at the contest's end)
    bool tourneyFinal = false;   // the next round is a Tourney's last: its champion is generated before it
};
// `availableLeft`: the host's Available Combatants still to fight (-1 unreadable): none left and no champion to come is the end.
inline ContestAfter ContestAfterFight(bool won, int roundsWon, int roundsToWin, int gameMode, int combatMode, int availableLeft) {
    ContestAfter a;
    a.roundsWonAfter = roundsWon;
    if (!won) { a.timeStep = true; return a; }
    const bool sane = roundsWon >= 0 && roundsToWin > 0 && roundsToWin <= kMaxContestRounds && roundsWon < roundsToWin;
    if (sane) a.roundsWonAfter = roundsWon + 1;
    a.step = ContestStep::Won;
    a.timeStep = true;
    if (gameMode != kArenaGameMode || !sane || a.roundsWonAfter >= roundsToWin) return a;
    const bool final = combatMode == kTourneyCombatMode && a.roundsWonAfter == roundsToWin - 1;
    if (availableLeft == 0 && !final) return a;     // nobody left to fight: the contest is won as it stands
    a.step = ContestStep::NextRound;
    a.timeStep = a.roundsWonAfter % 3 == 0;
    a.tourneyFinal = final;
    return a;
}
// The equipment budget of a Tourney's champion by Player Tier (UI_NextFIght NF:97).
inline double TourneyChampionBudget(int tier) {
    static constexpr double kBudget[] = {50, 100, 150, 250, 400, 666, 888, 1333, 2000};
    return kBudget[std::clamp(tier, 0, kTopRank)];
}

// An opponent out of a co-op fight stays out: dead, gave up (Give Up) or downed (Downed), as the stock Enemy Count takes it
// (Willie_BP Event Lose Match). A stock "Get Up, Pal" can clear a Give Up again (Willie_BP.cpp:4718); the count never does.
class OutLatch {
public:
    bool Note(uint8_t e, bool outNow) { if (outNow) out_.insert(e); return out_.count(e) != 0; }
    bool Out(uint8_t e) const { return out_.count(e) != 0; }
    void Forget(uint8_t e) { out_.erase(e); }
    void Clear() { out_.clear(); }
private:
    std::set<uint8_t> out_;
};

// How long a co-op fight's results stay up. Won: until a player holds G (the stock Talk key after a win), at most the
// fallback (a pad player, someone away). Lost, or progression_claim_g=0: back after 3 s, as before.
constexpr uint32_t kCoopResultsMs = 3000, kCoopClaimFallbackMs = 120000;
inline uint32_t CoopResultsMs(bool won, bool claimByG) { return won && claimByG ? kCoopClaimFallbackMs : kCoopResultsMs; }

// ------------------------------------------------------------------ the wire
// ProgressionResult 69, host -> client, reliable, once per fight, ahead of the Travel: 4 header, u32 fight (the host's travel
// number of the fight's level), u8 flags, u32 purse (0..kMaxPurse; 0 unless won). kResultNextRound (rev 41): a round won with
// more to come; no purse, no rank, only the fight's capture, and the time step only with kResultTimeStep.
constexpr uint8_t kResultWon = 1, kResultTourney = 2, kResultNextRound = 4, kResultTimeStep = 8;
constexpr uint8_t kResultFlags = kResultWon | kResultTourney | kResultNextRound | kResultTimeStep;
struct ProgResultMsg {
    uint32_t fight = 0;
    uint8_t flags = 0;
    uint32_t purse = 0;
    bool won() const { return (flags & kResultWon) != 0; }
    bool tourney() const { return (flags & kResultTourney) != 0; }
    bool nextRound() const { return (flags & kResultNextRound) != 0; }
    bool timeStep() const { return !nextRound() || (flags & kResultTimeStep) != 0; }   // a contest's end always steps
};
inline std::vector<uint8_t> Encode(const ProgResultMsg& m) {
    Writer w; w.header(MsgType::ProgressionResult); w.u32(m.fight); w.u8(m.flags); w.u32(m.purse); return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, ProgResultMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::ProgressionResult) return false;
    ProgResultMsg o; o.fight = r.u32(); o.flags = r.u8(); o.purse = r.u32();
    if (!r.done() || (o.flags & ~kResultFlags) || o.purse > kMaxPurse || (o.purse && !o.won())) return false;
    if (o.nextRound() && (!o.won() || o.purse)) return false;              // a next round is a round won, and pays nothing yet
    if ((o.flags & kResultTimeStep) && !o.nextRound()) return false;       // the step flag only says something between rounds
    m = o;
    return true;
}

// A client takes each fight's result once: the host may send it again (a resend, a host migration).
class ResultOnce {
public:
    bool Take(uint32_t fight) {
        if (seen_ && fight == last_) return false;
        seen_ = true; last_ = fight;
        return true;
    }
private:
    uint32_t last_ = 0;
    bool seen_ = false;
};

// The host's co-op fight outcome, kept from the match's end (Events::ended) to the rematch event, when the fight's end is
// applied. By then the match has cleared its winner for the next run (Match::Advance), so a win read there was always a
// loss: the first solo run logged "match over, winner key 0", then "fight #2 lost", the bet kept and no purse.
class CoopOutcome {
public:
    void Ended(int winnerKey) { won_ = winnerKey == kHumanTeam; }
    bool TakeWon() { const bool w = won_; won_ = false; return w; }   // once per fight; a fight that never ended is no win
private:
    bool won_ = false;
};

}  // namespace hsm
