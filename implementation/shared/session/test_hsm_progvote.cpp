// Unit tests for hsm_progvote.h: the Tavern's next-fight vote, its wire, its chat command and the Progression gear rule.
// Run via implementation/shared/run_tests.sh test_hsm_progvote.
#include "session/hsm_progvote.h"
#include "match/hsm_match_impl.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace hsm;
using C = ProgVote::Cast;
using O = ProgVote::Outcome;

static void TestBegin() {
    ProgVote v;
    CHECK(!v.Running() && v.Poll(0) == O::Running && v.CastVote(0, 0) == C::NotRunning && !v.Join(1) && !v.Leave(0));
    CHECK(!v.Begin(1, {0, 1}, 0, 0));                         // one option is nothing to vote on
    CHECK(!v.Begin(kMaxFightOptions + 1, {0, 1}, 0, 0));
    CHECK(!v.Begin(3, {}, 0, 0));                             // nobody to vote
    CHECK(!v.Begin(3, {0, 0}, 0, 0));                         // a slot twice
    CHECK(!v.Begin(3, {0, uint8_t(kMaxPeerMap)}, 0, 0));      // a slot beyond the table
    CHECK(!v.Running());
    CHECK(v.Begin(3, {0, 1, 2}, 1000, 0) && v.Running() && v.Voters() == 3 && v.Options() == 3 && v.Voted() == 0);
    CHECK(!v.Begin(3, {0, 1}, 1000, 0));                      // one at a time
    CHECK(v.SecondsLeft(1000) == 30 && v.SecondsLeft(1001) == 30 && v.SecondsLeft(1000 + 29001) == 1 && v.SecondsLeft(1000 + 30000) == 0 && v.SecondsLeft(1000 + 99999) == 0);
}

static void TestCast() {
    ProgVote v; v.Begin(3, {0, 1, 2}, 0, 0);
    CHECK(v.CastVote(1, 2) == C::Counted && v.PickOf(1) == 2 && v.Tally(2) == 1 && v.Voted() == 1);
    CHECK(v.CastVote(1, 2) == C::Same && v.Voted() == 1 && v.Tally(2) == 1);               // a duplicate changes nothing
    CHECK(v.CastVote(1, 0) == C::Changed && v.Tally(2) == 0 && v.Tally(0) == 1 && v.Voted() == 1);   // a changed mind moves the vote
    CHECK(v.CastVote(1, 3) == C::BadOption && v.PickOf(1) == 0);                            // an invalid option leaves the old pick
    CHECK(v.CastVote(1, 200) == C::BadOption);
    CHECK(v.CastVote(5, 0) == C::NotVoter && v.PickOf(5) == -2 && v.Voted() == 1);          // a slot that is no voter
    CHECK(v.PickOf(2) == -1);
}

static void TestMajority() {
    ProgVote v; v.Begin(3, {0, 1, 2}, 0, 7);
    v.CastVote(0, 1); v.CastVote(1, 2);
    CHECK(v.Poll(1000) == O::Running);                       // one still to vote, time left
    v.CastVote(2, 2);
    CHECK(v.Poll(1000) == O::Decided && v.result().winner == 2 && v.result().how == ProgWin::Votes && !v.Running() && v.outcome() == O::Decided);
    CHECK(v.Poll(2000) == O::Running && v.CastVote(0, 0) == C::NotRunning);      // over: nothing more counts
    // the deadline decides with whoever voted
    ProgVote t; t.Begin(3, {0, 1, 2}, 0, 7);
    t.CastVote(1, 1);
    CHECK(t.Poll(29999) == O::Running);
    CHECK(t.Poll(30000) == O::Decided && t.result().winner == 1 && t.result().how == ProgWin::Votes);
    // and begins again afterwards
    CHECK(t.Begin(2, {0, 1}, 40000, 0) && t.Running());
}

static void TestTies() {
    // a tie goes to the host's pick when it is among the tied
    { ProgVote v; v.Begin(4, {0, 1, 2, 3}, 0, 5); v.CastVote(0, 3); v.CastVote(1, 1); v.CastVote(2, 3); v.CastVote(3, 1);
      CHECK(v.Poll(0) == O::Decided && v.result().winner == 3 && v.result().how == ProgWin::HostPick); }
    // the host's pick is not among the tied: the seed picks among the tied, in list order
    { ProgVote v; v.Begin(4, {0, 1, 2, 3, 4}, 0, 5); v.CastVote(0, 0); v.CastVote(1, 2); v.CastVote(2, 2); v.CastVote(3, 3); v.CastVote(4, 3); v.CastVote(0, 2);   // host changes to 2: tied 2 and 3 on 3 vs 2?
      CHECK(v.Poll(0) == O::Decided && v.result().winner == 2 && v.result().how == ProgWin::Votes); }
    { ProgVote v; v.Begin(4, {0, 1, 2, 3}, 0, 5); v.CastVote(0, 0); v.CastVote(1, 2); v.CastVote(2, 3); v.CastVote(3, 2); v.CastVote(0, 1); v.CastVote(1, 3);
      // votes: 0:0 1:1 2:1 3:2 -> 3 wins? tally(3) = slots 1,2 = 2; tally(2) = slot 3 = 1
      CHECK(v.Poll(0) == O::Decided && v.result().winner == 3 && v.result().how == ProgWin::Votes); }
    { ProgVote v; v.Begin(4, {0, 1, 2, 3}, 0, 5);
      v.CastVote(0, 0); v.CastVote(1, 2); v.CastVote(2, 2); v.CastVote(3, 3); v.CastVote(0, 3);   // 2:2, 3:2, host on 3 -> host pick 3
      CHECK(v.Poll(0) == O::Decided && v.result().winner == 3 && v.result().how == ProgWin::HostPick); }
    { ProgVote v; v.Begin(4, {1, 2, 3, 4}, 0, 5);                            // the host does not vote (a server): the seed decides
      v.CastVote(1, 1); v.CastVote(2, 3); v.CastVote(3, 1); v.CastVote(4, 3);
      CHECK(v.Poll(0) == O::Decided && v.result().how == ProgWin::Seed && v.result().winner == 3);   // tied {1, 3}: seed 5 % 2 = 1 -> the second
      ProgVote w; w.Begin(4, {1, 2, 3, 4}, 0, 4);
      w.CastVote(1, 1); w.CastVote(2, 3); w.CastVote(3, 1); w.CastVote(4, 3);
      CHECK(w.Poll(0) == O::Decided && w.result().winner == 1 && w.result().how == ProgWin::Seed); }   // seed 4 % 2 = 0 -> the first
    // the host's vote is among the tied: it wins the tie whatever the seed
    { ProgVote v; v.Begin(3, {0, 1, 2, 3}, 0, 1); v.CastVote(0, 2); v.CastVote(1, 1); v.CastVote(2, 2); v.CastVote(3, 1);                  // 1: slots 1, 3; 2: slots 0, 2
      CHECK(v.Poll(0) == O::Decided && v.result().winner == 2 && v.result().how == ProgWin::HostPick); }
    // the host voted, but not for a tied option: the seed decides among the tied
    { ProgVote v; v.Begin(3, {0, 1, 2, 3, 4}, 0, 1); v.CastVote(0, 0); v.CastVote(1, 1); v.CastVote(2, 2); v.CastVote(3, 1); v.CastVote(4, 2);   // 1: two, 2: two, 0: one
      CHECK(v.Poll(0) == O::Decided && v.result().how == ProgWin::Seed && v.result().winner == 2); }   // tied {1, 2}: seed 1 % 2 = 1 -> option 2
    // nobody voted by the deadline: every option is tied; the seed picks one
    { ProgVote v; v.Begin(3, {0, 1}, 0, 7);
      CHECK(v.Poll(29999) == O::Running && v.Poll(30000) == O::Decided && v.result().how == ProgWin::Seed && v.result().winner == 1); }   // 7 % 3
    { ProgVote v; v.Begin(3, {0, 1}, 0, 9);
      CHECK(v.Poll(30000) == O::Decided && v.result().winner == 0 && v.result().how == ProgWin::Seed); }   // 9 % 3
}

static void TestLeaveJoin() {
    // a voter who leaves loses their vote and stops counting
    { ProgVote v; v.Begin(3, {0, 1, 2}, 0, 0); v.CastVote(1, 2); v.CastVote(0, 1);
      CHECK(v.Leave(1) && !v.Leave(1) && v.Voters() == 2 && v.Voted() == 1 && v.Tally(2) == 0 && v.PickOf(1) == -2 && v.VoterSlots() == (std::vector<uint8_t>{0, 2}));
      CHECK(v.Poll(100) == O::Running);
      v.CastVote(2, 1);
      CHECK(v.Poll(100) == O::Decided && v.result().winner == 1); }                                // everyone left has voted: it ends
    // the last voter to vote leaves: those who voted decide at once
    { ProgVote v; v.Begin(3, {0, 1, 2}, 0, 0); v.CastVote(0, 2); v.CastVote(1, 2); v.Leave(2);
      CHECK(v.Poll(0) == O::Decided && v.result().winner == 2 && v.result().how == ProgWin::Votes); }
    // everyone leaves: cancelled
    { ProgVote v; v.Begin(3, {0, 1}, 0, 0); v.Leave(0); v.Leave(1);
      CHECK(v.Poll(0) == O::Cancelled && v.result().how == ProgWin::Cancelled && !v.Running()); }
    // a late joiner can vote, and the vote waits for them while time is left
    { ProgVote v; v.Begin(3, {0, 1}, 0, 0); v.CastVote(0, 1); v.CastVote(1, 1);
      CHECK(v.Join(2) && !v.Join(2) && !v.Join(0) && !v.Join(uint8_t(kMaxPeerMap)) && v.Voters() == 3 && v.Voted() == 2);
      CHECK(v.Poll(100) == O::Running);
      CHECK(v.CastVote(2, 0) == C::Counted);
      CHECK(v.Poll(100) == O::Decided && v.result().winner == 1); }
    // ... and the deadline does not wait for them
    { ProgVote v; v.Begin(3, {0, 1}, 0, 0); v.CastVote(0, 1); v.CastVote(1, 1); v.Join(2);
      CHECK(v.Poll(30000) == O::Decided && v.result().winner == 1); }
    // cancel
    { ProgVote v; v.Begin(3, {0}, 0, 0); v.Cancel(); CHECK(!v.Running() && v.outcome() == O::Cancelled && v.result().how == ProgWin::Cancelled); }
    // wrap-safe deadline
    { ProgVote v; v.Begin(2, {0, 1}, 0xFFFFFF00u, 0); CHECK(v.Poll(0xFFFFFFFFu) == O::Running && v.Poll(0xFFFFFF00u + 29999u) == O::Running);
      CHECK(v.Poll(0xFFFFFF00u + 30000u) == O::Decided); }
}

static ProgVoteMsg State() {
    ProgVoteMsg m; m.kind = ProgVoteKind::State; m.seq = 513; m.secondsLeft = 27; m.mine = 1; m.voters = 3; m.voted = 2;
    m.options = {{"Pit Brawl", 1}, {"The Old Yard", 1}, {"Tourney of the Baron", 0}};
    return m;
}
static bool Dec(const std::vector<uint8_t>& b, ProgVoteMsg& o) { return Decode(b.data(), b.size(), o); }

static void TestWire() {
    ProgVoteMsg s = State(), o;
    std::vector<uint8_t> b = Encode(s);
    MsgType t; CHECK(PeekType(b.data(), b.size(), t) && t == MsgType::ProgressionVote && uint8_t(t) == 66);
    CHECK(Dec(b, o) && o.kind == ProgVoteKind::State && o.seq == 513 && o.secondsLeft == 27 && o.mine == 1 && o.voters == 3 && o.voted == 2 &&
          o.options.size() == 3 && o.options[1].name == "The Old Yard" && o.options[1].votes == 1 && o.options[2].votes == 0);
    for (size_t cut = 0; cut < b.size(); ++cut) { ProgVoteMsg x; CHECK(!Decode(b.data(), cut, x)); }   // every truncation fails
    std::vector<uint8_t> extra = b; extra.push_back(0); CHECK(!Dec(extra, o));
    // validation of a state
    { ProgVoteMsg x = s; x.options.pop_back(); x.options.pop_back(); CHECK(!Dec(Encode(x), o)); }                         // one option
    { ProgVoteMsg x = s; x.options.resize(kMaxFightOptions, {"X", 0}); x.voted = 2; x.voters = 3; x.options[0].votes = 1; x.options[1].votes = 1; x.options[2].votes = 0; CHECK(Dec(Encode(x), o) && o.options.size() == kMaxFightOptions); }
    { ProgVoteMsg x = s; x.options.resize(kMaxFightOptions + 1, {"X", 0}); CHECK(Dec(Encode(x), o) && o.options.size() == kMaxFightOptions); }   // the writer cuts at the cap
    { std::vector<uint8_t> big = Encode(s); big[11] = uint8_t(kMaxFightOptions + 1); ProgVoteMsg x; CHECK(!Dec(big, x)); }                // a count over the cap
    { ProgVoteMsg x = s; x.secondsLeft = 61; CHECK(!Dec(Encode(x), o)); }
    { ProgVoteMsg x = s; x.voters = uint8_t(kMaxPeerMap + 1); CHECK(!Dec(Encode(x), o)); }
    { ProgVoteMsg x = s; x.voted = 4; CHECK(!Dec(Encode(x), o)); }                                                         // voted > voters
    { ProgVoteMsg x = s; x.voted = 1; CHECK(!Dec(Encode(x), o)); }                                                         // the tallies add up to more
    { ProgVoteMsg x = s; x.mine = 3; CHECK(!Dec(Encode(x), o)); }                                                          // a pick that is no option
    { ProgVoteMsg x = s; x.mine = -2; CHECK(!Dec(Encode(x), o)); }
    { ProgVoteMsg x = s; x.mine = -1; CHECK(Dec(Encode(x), o) && o.mine == -1); }
    { ProgVoteMsg x = s; x.options[0].name = ""; CHECK(!Dec(Encode(x), o)); }                                              // empty name
    { ProgVoteMsg x = s; x.options[0].name = "Bad\nName"; CHECK(!Dec(Encode(x), o)); }                                     // control character
    { ProgVoteMsg x = s; x.options[0].name = "Caf\xC3\xA9"; CHECK(!Dec(Encode(x), o)); }                                   // not ASCII
    { ProgVoteMsg x = s; x.options[0].name = std::string(kMaxFightName, 'a'); CHECK(Dec(Encode(x), o) && o.options[0].name.size() == kMaxFightName); }
    { ProgVoteMsg x = s; x.options[0].name = std::string(kMaxFightName + 1, 'a'); CHECK(Dec(Encode(x), o) && o.options[0].name.size() == kMaxFightName); }   // Writer cuts at the cap
    // a pick
    ProgVoteMsg pk; pk.kind = ProgVoteKind::Pick; pk.seq = 9; pk.pick = 3;
    b = Encode(pk);
    CHECK(Dec(b, o) && o.kind == ProgVoteKind::Pick && o.seq == 9 && o.pick == 3);
    for (size_t cut = 0; cut < b.size(); ++cut) { ProgVoteMsg x; CHECK(!Decode(b.data(), cut, x)); }
    pk.pick = uint8_t(kMaxFightOptions); CHECK(!Dec(Encode(pk), o));
    pk.pick = 255; CHECK(!Dec(Encode(pk), o));
    // a result
    ProgVoteMsg rs; rs.kind = ProgVoteKind::Result; rs.seq = 4; rs.winner = 2; rs.how = ProgWin::HostPick; rs.name = "The Old Yard";
    b = Encode(rs);
    CHECK(Dec(b, o) && o.kind == ProgVoteKind::Result && o.winner == 2 && o.how == ProgWin::HostPick && o.name == "The Old Yard");
    for (size_t cut = 0; cut < b.size(); ++cut) { ProgVoteMsg x; CHECK(!Decode(b.data(), cut, x)); }
    { ProgVoteMsg x = rs; x.name = ""; CHECK(!Dec(Encode(x), o)); }                                                         // a winner needs a name
    { ProgVoteMsg x = rs; x.how = ProgWin::Cancelled; x.name = ""; CHECK(Dec(Encode(x), o) && o.how == ProgWin::Cancelled); }
    { ProgVoteMsg x = rs; x.how = ProgWin::Cancelled; CHECK(!Dec(Encode(x), o)); }                                          // cancelled has no name
    { ProgVoteMsg x = rs; x.winner = uint8_t(kMaxFightOptions); CHECK(!Dec(Encode(x), o)); }
    { ProgVoteMsg x = rs; x.how = ProgWin(kProgWinCount); CHECK(!Dec(Encode(x), o)); }
    // a kind that is none
    b = Encode(s); b[4] = 3; CHECK(!Dec(b, o));
    b = Encode(s); b[4] = 255; CHECK(!Dec(b, o));
    // another type or revision is not ours
    b = Encode(s); b[3] = uint8_t(MsgType::VoteState); CHECK(!Dec(b, o));
    b = Encode(s); b[2] = uint8_t(kProtocolRevision + 1); CHECK(!Dec(b, o));
    // a failed decode leaves the output as it was
    ProgVoteMsg keep = State(); b = Encode(s); b.pop_back(); CHECK(!Decode(b.data(), b.size(), keep) && keep.seq == 513 && keep.options.size() == 3);
    // names for the wire
    CHECK(FightNameForWire("") == "Fight" && FightNameForWire("  ") == "Fight" && FightNameForWire("Pit Fight") == "Pit Fight");
    CHECK(FightNameForWire("Caf\xC3\xA9") == "Caf??" && FightNameForWire(std::string(100, 'z')).size() == kMaxFightName);
    CHECK(FightNameForWire("a\tb") == "a?b" && ValidFightName(FightNameForWire(std::string("\x01\xFF\x7F", 3))));
}

static void TestCommand() {
    using K = FightCommand::Kind;
    CHECK(ParseFightCommand("/fight").kind == K::Start && ParseFightCommand("  /FIGHT  ").kind == K::Start && ParseFightCommand("/nextfight").kind == K::Start);
    FightCommand c = ParseFightCommand("/fight 2");
    CHECK(c.kind == K::Pick && c.option == 1);
    c = ParseFightCommand("/Fight   8 "); CHECK(c.kind == K::Pick && c.option == 7);
    c = ParseFightCommand("/fight 1"); CHECK(c.kind == K::Pick && c.option == 0);
    CHECK(ParseFightCommand("/fight 0").kind == K::Bad && ParseFightCommand("/fight 9").kind == K::Bad && ParseFightCommand("/fight 99").kind == K::Bad &&
          ParseFightCommand("/fight 100").kind == K::Bad && ParseFightCommand("/fight x").kind == K::Bad && ParseFightCommand("/fight 1 2").kind == K::Bad &&
          ParseFightCommand("/fight -1").kind == K::Bad && ParseFightCommand("/fight 01").kind == K::Pick);
    CHECK(ParseFightCommand("fight 2").kind == K::None && ParseFightCommand("hello").kind == K::None && ParseFightCommand("").kind == K::None &&
          ParseFightCommand("/fighter").kind == K::None && ParseFightCommand("/votekick 2").kind == K::None && ParseFightCommand("/").kind == K::None);
}

static void TestText() {
    const std::vector<std::string> l = ProgVoteLines(State());
    CHECK(l.size() == 4 && l[0].find("2 of 3 voted") != std::string::npos && l[0].find("27 s") != std::string::npos && l[0].find("/fight") != std::string::npos);
    CHECK(l[1].find("1) Pit Brawl - 1 vote") != std::string::npos && l[1].find("yours") == std::string::npos);
    CHECK(l[2].find("2) The Old Yard - 1 vote") != std::string::npos && l[2].find("yours") != std::string::npos);
    CHECK(l[3].find("0 votes") != std::string::npos);
    CHECK(ProgTallyLine(State()) == "Next fight votes: 1) Pit Brawl 1, 2) The Old Yard 1, 3) Tourney of the Baron 0 (2 of 3 voted)");
    ProgVoteMsg r; r.kind = ProgVoteKind::Result; r.winner = 1; r.how = ProgWin::Seed; r.name = "The Old Yard";
    CHECK(ProgResultLine(r) == "Vote result: The Old Yard (a tie, drawn)");
    r.how = ProgWin::HostPick; CHECK(ProgResultLine(r).find("host's pick") != std::string::npos);
    r.how = ProgWin::Votes; CHECK(ProgResultLine(r).find("by votes") != std::string::npos);
    r.how = ProgWin::Cancelled; CHECK(ProgResultLine(r).find("cancelled") != std::string::npos);
    CHECK(ProgStartByHandLine("Pit Brawl") == "Vote result: Pit Brawl - start it from the Fencer");
}

// Own inventory: whatever the lobby last said about gear, the Tavern's co-op fight uses every player's own career kit.
static void TestOwnKit() {
    for (Gear g : {Gear::Own, Gear::RandomTier, Gear::Mirror, Gear::Fists}) {
        MatchConfig lobby; lobby.format = Format::FreeForAll; lobby.gear = g; lobby.crossbows = 3; lobby.scenario = 2; lobby.timeLimitS = 300; lobby.scoreLimit = 7;
        const MatchConfig p = Sanitize(ProgressionConfig(lobby));
        CHECK(p.format == Format::PlayersVsBots && p.gear == Gear::Own && p.crossbows == 0 && p.scenario == 0 && p.timeLimitS == 0 && p.scoreLimit == 100);
        for (RespawnCause c : {RespawnCause::Rematch, RespawnCause::RoundReset, RespawnCause::Individual}) CHECK(RegearFor(p, c) == Regear::None);   // nobody is dealt a generated kit
        CHECK(JoinRegearFor(p) == Regear::None);
    }
    // the lobby's rows without the rule: random gear would have regeared everyone at the start
    MatchConfig bad; bad.format = Format::PlayersVsBots; bad.gear = Gear::RandomTier;
    CHECK(RegearFor(Sanitize(bad), RespawnCause::Rematch) == Regear::Each);
    // the roles formats roll their gear too; the rule still makes it Own
    MatchConfig riot; riot.format = Format::Riot;
    CHECK(Sanitize(ProgressionConfig(riot)).gear == Gear::Own);
}

// The match on a fight level: a host alone in a Progression lobby gets one (it stalled after the fight with none).
static void TestMatchWanted() {
    CHECK(HostMatchWanted(1, false, true));                  // the reported stall: alone, Free Fight in the lobby's rows, Progression
    CHECK(!HostMatchWanted(1, false, false));                // alone in a PvP format: nobody to fight
    CHECK(!HostMatchWanted(0, false, false));
    CHECK(HostMatchWanted(1, true, false));                  // alone against bots (Survival, Zombies)
    CHECK(HostMatchWanted(2, false, false) && HostMatchWanted(2, false, true) && HostMatchWanted(8, true, true));
}

int main() {
    TestBegin(); TestCast(); TestMajority(); TestTies(); TestLeaveJoin(); TestWire(); TestCommand(); TestText(); TestOwnKit();
    TestMatchWanted();
    if (g_fail == 0) std::printf("hsm_progvote: all checks passed\n");
    return g_fail ? 1 : 0;
}
