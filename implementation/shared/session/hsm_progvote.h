// Progression co-op (0.9.0, protocol rev 36): everyone votes on the next fight and keeps their own inventory.
//
// The vote. In the Tavern hub the host's game offers the next fights (the career's "Available Combat Events"). The host
// lists them, every player in the lobby votes for one (chat: /fight <number>), and when everyone has voted or kProgVoteMs
// are up the host starts the winner. Rules (ProgVote):
//   a vote has 2..kMaxFightOptions options and a set of voters (player slots); a voter who leaves loses their vote and no
//   longer counts; a late joiner becomes a voter (it can still vote until the vote ends)
//   a voter may change their pick until the vote ends; the same pick again changes nothing
//   a pick that is no option, from a slot that is no voter, or when no vote runs, is refused
//   it ends the moment every voter has voted (nobody left: it is cancelled), or at the deadline
//   the most votes win; a tie goes to the option the host (slot 0) voted for if it is one of the tied, else to
//   tied[seed % tied.size()] (the tied options in list order); no votes at all is a tie of every option
//
// Own inventory. Every player fights in the career kit they bring, never a generated or the host's (ProgressionConfig).
// A fight's end goes on every player's own multiplayer career (session/hsm_career.h, ProgressionResult 69).
//
// Wire: ProgressionVote 66, reliable, one type with three kinds:
//   State  (host -> client)  seq, seconds left, this client's pick, voters, voted, then the options (name, votes)
//   Pick   (client -> host)  seq, the 0-based option
//   Result (host -> client)  seq, the winning option, how it was decided and its name (cancelled: no name)
// Game-independent and unit-tested (test_hsm_progvote.cpp).
#pragma once
#include "hsm_protocol.h"
#include "match/hsm_match.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace hsm {

constexpr uint32_t kProgVoteMs = 30000;       // a vote runs this long at most
constexpr size_t kMinFightOptions = 2;        // fewer is nothing to vote on
constexpr size_t kMaxFightOptions = 8;
constexpr size_t kMaxFightName = 40;

// How a vote was decided (ProgVoteMsg::how).
enum class ProgWin : uint8_t { Votes = 0, HostPick = 1, Seed = 2, Cancelled = 3 };
constexpr uint8_t kProgWinCount = 4;

// ------------------------------------------------------------------ the rule for a progression fight's config
// The Tavern's co-op fight: players against the event's opponents, no clock, in the career gear each brings. Whatever the
// lobby's rows last said (random or identical gear, Brawl's fists, the Crossbows draw, a scenario) does not apply.
inline MatchConfig ProgressionConfig(MatchConfig c) {
    c.format = Format::PlayersVsBots; c.scoreLimit = 100; c.timeLimitS = 0;
    c.gear = Gear::Own;
    c.crossbows = 0;
    c.scenario = 0;
    return c;
}

// Whether the host starts a match on a fight level: two players or more, or one against bots. A Progression fight is one
// against bots whatever the lobby's format says (ProgressionConfig), so a host alone in its lobby gets its match too. Without
// it a lobby of one stalled after its fight: the stock end screens are kept off in a session and no match was there to end it.
inline bool HostMatchWanted(size_t players, bool botFormat, bool progression) {
    return players >= 2 || botFormat || progression;
}

// ------------------------------------------------------------------ the vote engine
class ProgVote {
public:
    enum class Cast : uint8_t { Counted, Changed, Same, BadOption, NotVoter, NotRunning };
    enum class Outcome : uint8_t { Running, Decided, Cancelled };
    struct Result { uint8_t winner = 0; ProgWin how = ProgWin::Votes; };

    // Starts a vote on `options` fights among `voters` (player slots, each once). False (nothing started): not 2..8 options,
    // no voter, a slot beyond kMaxPeerMap, or a vote already running.
    bool Begin(size_t options, const std::vector<uint8_t>& voters, uint32_t nowMs, uint32_t seed, uint32_t durationMs = kProgVoteMs) {
        if (running_ || options < kMinFightOptions || options > kMaxFightOptions || voters.empty()) return false;
        std::vector<Voter> v;
        for (uint8_t s : voters) {
            if (s >= kMaxPeerMap || Find(v, s)) return false;
            v.push_back({s, -1});
        }
        voters_ = std::move(v); options_ = options; seed_ = seed; deadline_ = nowMs + durationMs; running_ = true;
        result_ = Result{}; outcome_ = Outcome::Running;
        return true;
    }

    bool Running() const { return running_; }
    size_t Options() const { return options_; }
    size_t Voters() const { return voters_.size(); }
    size_t Voted() const { size_t n = 0; for (const Voter& v : voters_) n += v.pick >= 0; return n; }
    size_t Tally(size_t option) const { size_t n = 0; for (const Voter& v : voters_) n += v.pick == int(option); return n; }
    std::vector<uint8_t> VoterSlots() const { std::vector<uint8_t> out; for (const Voter& v : voters_) out.push_back(v.slot); return out; }
    bool IsVoter(uint8_t slot) const { return Find(voters_, slot) != nullptr; }
    // The option a voter picked, -1 not yet, -2 not a voter.
    int PickOf(uint8_t slot) const { const Voter* v = Find(voters_, slot); return v ? v->pick : -2; }
    uint32_t SecondsLeft(uint32_t nowMs) const {
        if (!running_) return 0;
        const int32_t left = int32_t(deadline_ - nowMs);
        return left <= 0 ? 0 : uint32_t(left + 999) / 1000;
    }

    Cast CastVote(uint8_t slot, size_t option) {
        if (!running_) return Cast::NotRunning;
        Voter* v = Find(voters_, slot);
        if (!v) return Cast::NotVoter;
        if (option >= options_) return Cast::BadOption;
        if (v->pick == int(option)) return Cast::Same;
        const bool changed = v->pick >= 0;
        v->pick = int(option);
        return changed ? Cast::Changed : Cast::Counted;
    }
    // A late joiner can vote from now on. False: no vote runs, or the slot already is a voter / is no slot.
    bool Join(uint8_t slot) {
        if (!running_ || slot >= kMaxPeerMap || IsVoter(slot)) return false;
        voters_.push_back({slot, -1});
        return true;
    }
    // A voter who left: their vote is gone and they no longer count. False: not a voter.
    bool Leave(uint8_t slot) {
        for (size_t i = 0; i < voters_.size(); ++i)
            if (voters_[i].slot == slot) { voters_.erase(voters_.begin() + long(i)); return true; }
        return false;
    }

    // Decides the vote when it is time: everyone remaining has voted, or the deadline passed (tick counts wrap-safe).
    // Decided / Cancelled once, then the vote is over (result()).
    Outcome Poll(uint32_t nowMs) {
        if (!running_) return Outcome::Running;
        if (voters_.empty()) return Finish({0, ProgWin::Cancelled}, Outcome::Cancelled);
        if (Voted() < voters_.size() && int32_t(nowMs - deadline_) < 0) return Outcome::Running;
        size_t top = 0;
        for (size_t o = 0; o < options_; ++o) top = std::max(top, Tally(o));
        std::vector<uint8_t> tied;
        for (size_t o = 0; o < options_; ++o) if (Tally(o) == top) tied.push_back(uint8_t(o));
        if (tied.size() == 1) return Finish({tied[0], ProgWin::Votes}, Outcome::Decided);
        const Voter* host = Find(voters_, 0);
        if (host && host->pick >= 0 && std::find(tied.begin(), tied.end(), uint8_t(host->pick)) != tied.end())
            return Finish({uint8_t(host->pick), ProgWin::HostPick}, Outcome::Decided);
        return Finish({tied[seed_ % tied.size()], ProgWin::Seed}, Outcome::Decided);
    }
    Outcome outcome() const { return outcome_; }
    const Result& result() const { return result_; }
    // Cancels a vote that runs (the hub was left, the host picked another way).
    void Cancel() { if (running_) Finish({0, ProgWin::Cancelled}, Outcome::Cancelled); }

private:
    struct Voter { uint8_t slot; int pick; };
    static Voter* Find(std::vector<Voter>& v, uint8_t slot) { for (Voter& x : v) if (x.slot == slot) return &x; return nullptr; }
    static const Voter* Find(const std::vector<Voter>& v, uint8_t slot) { for (const Voter& x : v) if (x.slot == slot) return &x; return nullptr; }
    Outcome Finish(Result r, Outcome o) { result_ = r; outcome_ = o; running_ = false; return o; }

    std::vector<Voter> voters_;
    size_t options_ = 0;
    uint32_t seed_ = 0, deadline_ = 0;
    bool running_ = false;
    Result result_;
    Outcome outcome_ = Outcome::Running;
};

// ------------------------------------------------------------------ wire
enum class ProgVoteKind : uint8_t { State = 0, Pick = 1, Result = 2 };

struct ProgOption { std::string name; uint8_t votes = 0; };
struct ProgVoteMsg {
    ProgVoteKind kind = ProgVoteKind::State;
    uint16_t seq = 0;                 // the vote's number; a Pick for another is refused
    // State
    uint8_t secondsLeft = 0;          // 0..60
    int8_t mine = -1;                 // this client's pick (an option index), -1 none
    uint8_t voters = 0, voted = 0;    // voted <= voters <= kMaxPeerMap; the options' votes add up to voted
    std::vector<ProgOption> options;  // 2..kMaxFightOptions
    // Pick
    uint8_t pick = 0;                 // 0-based
    // Result
    uint8_t winner = 0;               // 0-based
    ProgWin how = ProgWin::Votes;
    std::string name;                 // the winner's name (Cancelled: empty)
};

// A fight's name as the wire carries it: printable ASCII only (the game's text is localised; the rest becomes '?'), cut to
// kMaxFightName, never empty.
inline std::string FightNameForWire(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        if (out.size() >= kMaxFightName) break;
        out += (c >= 0x20 && c < 0x7F) ? char(c) : '?';
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out.empty() ? std::string("Fight") : out;
}
inline bool ValidFightName(const std::string& s) {
    if (s.empty() || s.size() > kMaxFightName) return false;
    for (unsigned char c : s) if (c < 0x20 || c >= 0x7F) return false;
    return true;
}

inline std::vector<uint8_t> Encode(const ProgVoteMsg& m) {
    Writer w; w.header(MsgType::ProgressionVote); w.u8(uint8_t(m.kind)); w.u16(m.seq);
    switch (m.kind) {
        case ProgVoteKind::State: {
            w.u8(m.secondsLeft); w.u8(uint8_t(m.mine)); w.u8(m.voters); w.u8(m.voted);
            const size_t n = std::min(m.options.size(), kMaxFightOptions);
            w.u8(uint8_t(n));
            for (size_t i = 0; i < n; ++i) { w.str(m.options[i].name, kMaxFightName); w.u8(m.options[i].votes); }
            break;
        }
        case ProgVoteKind::Pick: w.u8(m.pick); break;
        case ProgVoteKind::Result: w.u8(m.winner); w.u8(uint8_t(m.how)); w.str(m.name, kMaxFightName); break;
    }
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, ProgVoteMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::ProgressionVote) return false;
    const uint8_t kind = r.u8(); if (kind > uint8_t(ProgVoteKind::Result)) return false;
    ProgVoteMsg o; o.kind = ProgVoteKind(kind); o.seq = r.u16();
    switch (o.kind) {
        case ProgVoteKind::State: {
            o.secondsLeft = r.u8(); o.mine = int8_t(r.u8()); o.voters = r.u8(); o.voted = r.u8();
            const size_t count = r.u8();
            if (!r.ok() || count < kMinFightOptions || count > kMaxFightOptions) return false;
            size_t sum = 0;
            for (size_t i = 0; i < count; ++i) {
                ProgOption op; op.name = r.str(kMaxFightName); op.votes = r.u8();
                if (!r.ok() || !ValidFightName(op.name)) return false;
                sum += op.votes; o.options.push_back(std::move(op));
            }
            if (!r.done()) return false;
            if (o.secondsLeft > 60 || o.voters > kMaxPeerMap || o.voted > o.voters || sum != o.voted) return false;
            if (o.mine < -1 || o.mine >= int(count)) return false;
            break;
        }
        case ProgVoteKind::Pick:
            o.pick = r.u8();
            if (!r.done() || o.pick >= kMaxFightOptions) return false;
            break;
        case ProgVoteKind::Result: {
            o.winner = r.u8(); const uint8_t how = r.u8(); o.name = r.str(kMaxFightName);
            if (!r.done() || o.winner >= kMaxFightOptions || how >= kProgWinCount) return false;
            o.how = ProgWin(how);
            if (o.how == ProgWin::Cancelled ? !o.name.empty() : !ValidFightName(o.name)) return false;
            break;
        }
    }
    m = std::move(o);
    return true;
}

// ------------------------------------------------------------------ chat command and the text the players read
struct FightCommand {
    enum class Kind : uint8_t { None, Start, Pick, Bad } kind = Kind::None;   // None: not a fight command; Start: "/fight" (the host starts a vote); Bad: wrong argument
    uint8_t option = 0;                                                       // Pick: 0-based
};
// "/fight" (or "/nextfight"), "/fight 2" (the 2nd option). Case-insensitive; the number is 1..kMaxFightOptions.
inline FightCommand ParseFightCommand(const std::string& line) {
    FightCommand c;
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size() || line[i] != '/') return c;
    ++i;
    size_t e = i;
    while (e < line.size() && line[e] != ' ' && line[e] != '\t') ++e;
    std::string word = line.substr(i, e - i);
    for (char& ch : word) if (ch >= 'A' && ch <= 'Z') ch = char(ch + 32);
    if (word != "fight" && word != "nextfight") return c;
    std::string rest = line.substr(e);
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.erase(rest.begin());
    while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t' || rest.back() == '\r' || rest.back() == '\n')) rest.pop_back();
    if (rest.empty()) { c.kind = FightCommand::Kind::Start; return c; }
    c.kind = FightCommand::Kind::Bad;
    if (rest.size() > 2) return c;
    unsigned n = 0;
    for (char ch : rest) { if (ch < '0' || ch > '9') return c; n = n * 10 + unsigned(ch - '0'); }
    if (n < 1 || n > kMaxFightOptions) return c;
    c.kind = FightCommand::Kind::Pick; c.option = uint8_t(n - 1);
    return c;
}

// The vote panel: one line of headline, then one line per option (number, name, votes, a marker on the player's own pick).
inline std::vector<std::string> ProgVoteLines(const ProgVoteMsg& s) {
    std::vector<std::string> out;
    out.push_back("Next fight: " + std::to_string(s.voted) + " of " + std::to_string(s.voters) + " voted, " + std::to_string(s.secondsLeft) +
                  " s left. Type /fight <number> to vote.");
    for (size_t i = 0; i < s.options.size(); ++i)
        out.push_back("  " + std::to_string(i + 1) + ") " + s.options[i].name + " - " + std::to_string(s.options[i].votes) +
                      (s.options[i].votes == 1 ? " vote" : " votes") + (int(i) == s.mine ? "  <- yours" : ""));
    return out;
}
// The tally on one line, for the updates after the panel was shown: "Next fight votes: 1) Pit Brawl 2, 2) The Old Yard 1 (3 of 4 voted)".
inline std::string ProgTallyLine(const ProgVoteMsg& s) {
    std::string out = "Next fight votes: ";
    for (size_t i = 0; i < s.options.size(); ++i)
        out += (i ? ", " : "") + std::to_string(i + 1) + ") " + s.options[i].name + " " + std::to_string(s.options[i].votes);
    return out + " (" + std::to_string(s.voted) + " of " + std::to_string(s.voters) + " voted)";
}
inline const char* ProgWinText(ProgWin w) {
    switch (w) {
        case ProgWin::HostPick: return "a tie, the host's pick";
        case ProgWin::Seed: return "a tie, drawn";
        case ProgWin::Cancelled: return "cancelled";
        case ProgWin::Votes: break;
    }
    return "by votes";
}
inline std::string ProgResultLine(const ProgVoteMsg& r) {
    if (r.how == ProgWin::Cancelled) return "Next fight vote cancelled.";
    return "Vote result: " + r.name + " (" + ProgWinText(r.how) + ")";
}
// When the game's own path to start the winner is not available: the host starts it by hand.
inline std::string ProgStartByHandLine(const std::string& name) { return "Vote result: " + name + " - start it from the Fencer"; }

}  // namespace hsm
