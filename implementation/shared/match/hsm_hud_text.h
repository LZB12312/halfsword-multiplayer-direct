// Match HUD text, built from the wire MatchInfo so host and clients show the same thing. Game-independent.
#pragma once
#include "hsm_protocol.h"
#include "match/hsm_match.h"
#include <algorithm>
#include <cctype>
#include <string>

namespace hsm {

// The sides' names in a match of format `f`: Infection's two are Survivors and Infected, the rest are the colours.
inline std::string TeamName(Format f, int team) {
    if (IsInfection(f)) return team == kSurvivorTeam ? "SURVIVORS" : team == kInfectedTeam ? "INFECTED" : "-";
    return TeamName(team);
}

inline std::string NameOf(const MatchInfo& m, uint8_t entity) {
    for (const FighterEntry& f : m.fighters) if (f.entity == entity && !f.name.empty()) return f.name;
    return "#" + std::to_string(entity);
}

inline int ScoreOf(const MatchInfo& m, int key) {
    for (const ScoreEntry& s : m.scores) if (s.key == key) return s.score;
    return 0;
}

// Who took the round, for the gap: a team round names the team of the survivor in roundWinner ("your team" when it is ours),
// else the fighter ("you").
inline std::string RoundTaker(const MatchInfo& m, uint8_t myEntity) {
    if (IsTeamRound(Format(m.format))) {
        int8_t won = kNoTeam, mine = kNoTeam;
        for (const FighterEntry& e : m.fighters) { if (e.entity == m.roundWinner) won = e.team; if (e.entity == myEntity) mine = e.team; }
        return won != kNoTeam && won == mine ? "your team" : TeamName(won);
    }
    return m.roundWinner == myEntity ? "you" : NameOf(m, m.roundWinner);
}

// ---------------------------------------------------------------- Tournament and Pit Bridge Duels, from the wire
// What a machine is in the fight: fighting (in the pairing now or next), waiting its turn on the bench, or out (knocked out of the
// bracket, or not in this tournament: a late joiner). Pit Bridge Duels have no Out.
enum class QueueRole : uint8_t { None, Fighting, Waiting, Out };

// The two fighting (the bracket's current game, the queue's front two); false when there is none.
inline bool QueuePair(const MatchInfo& m, uint8_t& a, uint8_t& b) {
    a = b = 0;
    if (m.format == uint8_t(Format::Tournament)) {
        Bracket br; Bracket::Game g;
        if (!br.Restore(m.queueOrder, m.bracketWinners) || !br.Current(g)) return false;
        a = g.a; b = g.b;
        return true;
    }
    if (m.format == uint8_t(Format::PitBridgeDuel) && m.queueOrder.size() >= 2) { a = m.queueOrder[0]; b = m.queueOrder[1]; return true; }
    return false;
}
inline QueueRole RoleInQueue(const MatchInfo& m, uint8_t e) {
    if (!IsQueueFormat(Format(m.format))) return QueueRole::None;
    if (m.format == uint8_t(Format::Tournament)) {
        Bracket br;
        if (!br.Restore(m.queueOrder, m.bracketWinners) || !br.InRunning(e)) return QueueRole::Out;
    }
    uint8_t a, b;
    return QueuePair(m, a, b) && (e == a || e == b) ? QueueRole::Fighting : QueueRole::Waiting;
}

// The stage the fight is in, "Quarter-final 2/4" (the number among the games played that round, left out when there is one), or
// "Fight 7" in the Pit. Empty when the bracket is decided.
inline std::string QueueStage(const MatchInfo& m) {
    if (m.format == uint8_t(Format::PitBridgeDuel)) return "Fight " + std::to_string(m.round);
    Bracket br; Bracket::Game g;
    if (!br.Restore(m.queueOrder, m.bracketWinners) || !br.Current(g)) return {};
    std::string s = br.StageName(g.round);
    if (br.PlayedGames(g.round) > 1) s += " " + std::to_string(br.GameNumber(g.round, g.index)) + "/" + std::to_string(br.PlayedGames(g.round));
    return s;
}
inline bool QueueFinal(const MatchInfo& m) {
    Bracket br; Bracket::Game g;
    return m.format == uint8_t(Format::Tournament) && br.Restore(m.queueOrder, m.bracketWinners) && br.Current(g) && g.round == br.Rounds() - 1;
}

// The line under the score strip. Tournament: "Quarter-final 2/4: Ann vs Bo - first to 2 (1-0)   Next: Cy vs Di". Pit Bridge Duels:
// "Ann: 3 in a row   Ann vs Bo   Up next: Cy".
inline std::string QueueLine(const MatchInfo& m) {
    if (m.phase == uint8_t(MatchPhase::Idle) || m.phase == uint8_t(MatchPhase::Results)) return {};
    if (m.format == uint8_t(Format::PitBridgeDuel)) {
        if (m.queueOrder.size() < 2) return {};
        std::string s;
        if (m.streak) s = NameOf(m, m.queueOrder[0]) + ": " + std::to_string(m.streak) + " in a row   ";
        s += NameOf(m, m.queueOrder[0]) + " vs " + NameOf(m, m.queueOrder[1]);
        if (m.queueOrder.size() > 2) s += "   Up next: " + NameOf(m, m.queueOrder[2]);
        return s;
    }
    Bracket br; Bracket::Game g, next;
    if (m.format != uint8_t(Format::Tournament) || !br.Restore(m.queueOrder, m.bracketWinners) || !br.Current(g)) return {};
    std::string s = QueueStage(m) + ": " + NameOf(m, g.a) + " vs " + NameOf(m, g.b) + " - first to " + std::to_string(m.scoreLimit);
    if (m.fightWinsA || m.fightWinsB) s += " (" + std::to_string(m.fightWinsA) + "-" + std::to_string(m.fightWinsB) + ")";
    if (br.NextGame(next)) s += "   Next: " + NameOf(m, next.a) + " vs " + NameOf(m, next.b);
    return s;
}

inline std::string Seconds(uint32_t ms) { return std::to_string((ms + 999) / 1000); }

inline std::string Clock(uint32_t ms) {
    uint32_t s = ms / 1000;
    std::string sec = std::to_string(s % 60);
    return std::to_string(s / 60) + ":" + (sec.size() < 2 ? "0" : "") + sec;
}

inline std::string Upper(std::string s) { for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c))); return s; }

inline std::string TeamScores(const MatchInfo& m) {
    std::string s;
    for (int t = 0; t < TeamCount(Format(m.format), m.teams); ++t) s += (t ? "   " : "") + TeamName(Format(m.format), t) + " " + std::to_string(ScoreOf(m, t));
    return s;
}

// MatchInfo `m` as of `nowMs`, when it arrived at `receivedAtMs` (tick counts, wrap-safe): its clocks run down by
// the time since, stopping at 0. A receive time ahead of now (a clock that stepped back) counts as no time.
inline MatchInfo Extrapolate(const MatchInfo& m, uint32_t receivedAtMs, uint32_t nowMs) {
    uint32_t elapsed = nowMs - receivedAtMs;
    if (elapsed > 0x80000000u) elapsed = 0;
    MatchInfo e = m;
    e.timeLeftMs = m.timeLeftMs > elapsed ? m.timeLeftMs - elapsed : 0;
    e.roundGapLeftMs = m.roundGapLeftMs > elapsed ? m.roundGapLeftMs - elapsed : 0;
    for (Objective& o : e.objectives) o.timerMs = o.timerMs > elapsed ? o.timerMs - elapsed : 0;   // a dropped flag's return clock
    e.fuseLeftMs = uint16_t(m.fuseLeftMs > elapsed ? m.fuseLeftMs - elapsed : 0);
    return e;
}

inline bool InRoundGap(const MatchInfo& m) {
    return IsRoundFormat(Format(m.format)) && m.roundGapLeftMs && (m.phase == uint8_t(MatchPhase::Live) || m.phase == uint8_t(MatchPhase::SuddenDeath));
}

// The bot formats' break between waves: MatchInfo carries its time left in roundGapLeftMs and the players' votes to
// skip it in roundWinner (Duel-only fields before: older clients ignore them outside Duel).
// A Progression co-op fight won (every opponent out) and waiting for a player to hold G, as the stock game waits for its
// Talk key (UI_WIN). `oneFight`: a Progression fight, as GiveUpActionFor takes it.
inline bool CoopFightWon(Format f, MatchPhase phase, int winner, bool oneFight) {
    return oneFight && f == Format::PlayersVsBots && phase == MatchPhase::Results && winner == kHumanTeam;
}
inline bool CoopFightWon(const MatchInfo& m, bool oneFight) { return CoopFightWon(Format(m.format), MatchPhase(m.phase), m.winner, oneFight); }
// Whether blows count (hits, claims, severs): while a round or wave is fought, and in a Progression fight won and waiting for
// G, as the stock game lets the player go on hitting, stripping and killing the opponents who gave up until the Talk key is
// held (the report: after the win they were invincible).
inline bool BlowsCount(Format f, MatchPhase phase, int winner, bool oneFight) {
    return CombatPhase(phase) || CoopFightWon(f, phase, winner, oneFight);
}
inline bool InWaveBreak(const MatchInfo& m) {
    return IsBotFormat(Format(m.format)) && m.roundGapLeftMs && (m.phase == uint8_t(MatchPhase::Live) || m.phase == uint8_t(MatchPhase::SuddenDeath));
}

// Tag: who is "it" and how long its fuse has: "YOU ARE IT 12 s", "Ann is it 12 s". Empty without an "it".
inline std::string TagLine(const MatchInfo& m, uint8_t myEntity) {
    if (!m.tagIt) return {};
    const std::string fuse = " " + Seconds(m.fuseLeftMs) + " s";
    return m.tagIt == myEntity ? "YOU ARE IT" + fuse : NameOf(m, m.tagIt) + " is it" + fuse;
}

// `oneFight`: a Progression fight (HUD) - the host takes everyone back to the Tavern after the results.
inline std::string StatusLine(const MatchInfo& m, uint8_t myEntity, bool oneFight = false) {
    Format f = Format(m.format);
    if (InWaveBreak(m)) {                                  // "Wave 2 cleared   next wave in 45   hold G to skip (1/2 ready)"
        int players = 0;
        for (const FighterEntry& e : m.fighters) players += e.team == kHumanTeam;
        return "Wave " + std::to_string(ScoreOf(m, kHumanTeam)) + " cleared   next wave in " + Seconds(m.roundGapLeftMs) +
               "   hold G to skip (" + std::to_string(m.roundWinner) + "/" + std::to_string(players) + " ready)";
    }
    if (InRoundGap(m)) {                                   // "Round 2 to Ann   next round in 3"
        std::string who = !m.roundWinner ? " drawn" : " to " + RoundTaker(m, myEntity);
        return "Round " + std::to_string(m.round) + who + "   next round in " + Seconds(m.roundGapLeftMs);
    }
    std::string limit = "(first to " + std::to_string(m.scoreLimit) + ")";
    switch (MatchPhase(m.phase)) {
        case MatchPhase::Idle: return {};
        case MatchPhase::Countdown: return "Match starts in " + Seconds(m.timeLeftMs);
        case MatchPhase::Results: {
            if (CoopFightWon(m, oneFight)) return "FIGHT WON   hold G to go on (or in " + Seconds(m.timeLeftMs) + ")";
            std::string who;
            if (f == Format::Zombies) who = "OVERRUN - survived " + std::to_string(ScoreOf(m, kHumanTeam)) + " waves";
            else if (f == Format::PlayersVsBots) who = m.winner == kBotTeam ? "BOTS WIN" : "PLAYERS WIN";
            else if (ScoresPerFighter(f)) who = m.winner == int(myEntity) ? "YOU WIN" : Upper(NameOf(m, uint8_t(m.winner))) + " WINS";
            else if (IsInfection(f)) who = m.winner == kSurvivorTeam ? "SURVIVORS WIN" : "INFECTED WIN";
            else who = TeamName(m.winner) + " WINS";
            return who + (oneFight && IsBotFormat(f) ? "   back to the Tavern in " : "   rematch in ") + Seconds(m.timeLeftMs);
        }
        case MatchPhase::SuddenDeath:
            return "SUDDEN DEATH   " + (f == Format::FreeForAll ? std::string("next kill wins") : TeamScores(m));
        case MatchPhase::Live: break;
    }
    std::string s;
    if (IsBotFormat(f)) {
        int cleared = ScoreOf(m, kHumanTeam);
        s = "Wave " + std::to_string(cleared + 1) + "   " + std::to_string(cleared) + " cleared";
    } else if (ScoresPerFighter(f)) {
        int mine = ScoreOf(m, myEntity), best = 0; uint8_t rival = 0;
        for (const ScoreEntry& e : m.scores) if (e.key != myEntity && (rival == 0 || e.score > best)) { best = e.score; rival = uint8_t(e.key); }
        if (IsRoundFormat(f) && !rival)
            for (const FighterEntry& e : m.fighters) if (e.entity != myEntity) { rival = e.entity; break; }
        std::string scores = "You " + std::to_string(mine) + (rival ? "   " + NameOf(m, rival) + " " + std::to_string(best) : "");
        if (IsQueueFormat(f)) s = QueueLine(m);
        else
        s = f == Format::Duel ? "Rounds   " + scores + "   (best of " + std::to_string(2 * m.scoreLimit - 1) + ")"
          : f == Format::LastManStanding ? "Rounds   " + scores + "   " + limit : scores + "   " + limit;   // more than two fighters: no "best of"
    } else s = (IsTeamRound(f) ? "Rounds   " : "") + TeamScores(m) + (IsInfection(f) ? std::string() : "   " + limit);   // Infection has no score limit
    if (IsTag(f) && m.tagIt) s += "   " + TagLine(m, myEntity);
    if (m.timeLeftMs) s += "   " + Clock(m.timeLeftMs);
    return s;
}

// The line under a dead player. Duel: the round is over and everybody returns together after the gap; the bot
// formats: the downed return when the wave is cleared; otherwise the respawn delay counts down.
inline std::string RespawnLine(const MatchInfo& m, uint32_t sinceDeathMs) {
    if (m.phase == uint8_t(MatchPhase::Idle) || m.phase == uint8_t(MatchPhase::Results)) return {};
    Format f = Format(m.format);
    if (IsRoundFormat(f)) return m.roundGapLeftMs ? "Round over   next round in " + Seconds(m.roundGapLeftMs) : "Round over";
    if (IsBotFormat(f)) return "Down   back next wave";
    uint32_t delay = uint32_t(m.respawnDelayS) * 1000u;
    return sinceDeathMs < delay ? "Respawning in " + Seconds(delay - sinceDeathMs) : "Respawning...";
}

// A drawn Duel round (Kill flags 16/32): "Ann and Bo drew the round (both down)".
inline std::string DrawnWhy(const Kill& k) { return (k.flags & 32) ? " drew the round (no hits)" : " drew the round (both down)"; }
inline std::string DrawnLine(const MatchInfo& m, const Kill& k) {
    std::string who = NameOf(m, k.victim);
    if (k.killer != 0) who += " and " + NameOf(m, k.killer);
    return who + DrawnWhy(k);
}

// A mode event as a feed line: "Ann tagged Bo", "Ann burned out", "Ann infected Bo", "Bo was infected".
inline std::string ModeEventLine(const MatchInfo& m, const ModeEventMsg& e) {
    switch (e.kind) {
        case ModeEventKind::TagPass: return NameOf(m, e.a) + " tagged " + NameOf(m, e.b);
        case ModeEventKind::TagBurn: return NameOf(m, e.a) + " burned out";
        case ModeEventKind::Infected: return e.a ? NameOf(m, e.a) + " infected " + NameOf(m, e.b) : NameOf(m, e.b) + " was infected";
    }
    return {};
}

inline std::string KillLine(const MatchInfo& m, const Kill& k) {
    std::string victim = NameOf(m, k.victim);
    if (k.flags & (16 | 32)) return DrawnLine(m, k);
    if (k.flags & 8) return victim + " gave up";
    if (k.flags & 2) return victim + " killed themselves";
    if ((k.flags & 4) || k.killer == 0) return victim + " died";
    return NameOf(m, k.killer) + ((k.flags & 1) ? " killed teammate " : " killed ") + victim;
}

inline std::string Scoreboard(const MatchInfo& m) {
    std::vector<FighterEntry> rows = m.fighters;
    std::stable_sort(rows.begin(), rows.end(), [](const FighterEntry& a, const FighterEntry& b) { return a.kills > b.kills; });
    std::string s = "Player            Team     K / D\n";
    for (const FighterEntry& f : rows) {
        std::string name = f.name.empty() ? "#" + std::to_string(f.entity) : f.name;
        name.resize(18, ' ');
        std::string team = TeamName(f.team); team.resize(9, ' ');
        s += name + team + std::to_string(f.kills) + " / " + std::to_string(f.deaths) + "\n";
    }
    return s;
}

// Lobby panel labels and cycling.
inline std::string FormatName(Format f) {
    switch (f) {
        case Format::TwoTeams: case Format::ThreeTeams: return "Teams";
        case Format::PlayersVsBots: return "Survival";
        case Format::Zombies: return "Zombies";
        case Format::Duel: return "Duel";
        case Format::LastManStanding: return "Last man standing";
        case Format::TeamDuel: return "Team duel";
        case Format::Riot: return "Riot";
        case Format::Juggernaut: return "Juggernaut";
        case Format::CaptureTheFlag: return "Capture the flag";
        case Format::Tag: return "Tag";
        case Format::Infection: return "Infection";
        case Format::CrossbowTDM: return "Crossbow TDM";
        case Format::Tournament: return "Tournament";
        case Format::PitBridgeDuel: return "Pit bridge duels";
        default: return "Free-for-all";
    }
}
// What a match is called to the players: FormatName, but Brawl (free-for-all with bare fists) by its own name.
// `teams` (the Teams format's count, 2..4; 0 = leave it out) makes "Teams (3)".
inline std::string ModeName(Format f, Gear g, int teams = 0) {
    if (IsBrawl(f, g)) return "Brawl";
    return PicksTeams(f) && !IsCtf(f) && teams > 0 ? FormatName(f) + " (" + std::to_string(TeamCount(f, teams)) + ")" : FormatName(f);
}
inline std::string ModeName(uint8_t format, uint8_t gear, int teams = 0) { return ModeName(Format(format), Gear(gear), teams); }
// The value after `cur` in `options`, wrapping; the first option when `cur` is not one of them.
inline int NextOf(const std::vector<int>& options, int cur) {
    if (options.empty()) return cur;
    auto it = std::find(options.begin(), options.end(), cur);
    return it == options.end() || it + 1 == options.end() ? options.front() : *(it + 1);
}
// Auto -> team 0 -> ... -> last team -> Auto.
inline int8_t NextTeamPick(int8_t cur, int teams) { return cur + 1 >= 0 && cur + 1 < teams ? int8_t(cur + 1) : kNoTeam; }
inline std::string TeamLabel(int team) {
    static const char* kNames[] = {"Red", "Blue", "Green", "Gold"};
    return team >= 0 && team < 4 ? kNames[team] : "Auto";
}
// The lobby's gear setting, with when it changes (RegearFor). The bot formats roll once, at the run's start.
inline std::string GearLabel(Gear g, Format f) {
    if (!GearAllowed(f, g)) return "Own loadouts";
    if (g == Gear::Fists) return "Bare fists";
    if (IsBotFormat(f)) return g == Gear::RandomTier ? "Random by tier, kept all run" : g == Gear::Mirror ? "Everyone identical, kept all run" : "Own loadouts";
    if (g == Gear::RandomTier) return IsRoundFormat(f) ? "Random by tier, new each round" : "Random by tier, new each life";
    if (g == Gear::Mirror) return IsRoundFormat(f) ? "Everyone identical, new each round" : "Everyone identical";
    return "Own loadouts";
}
inline std::string TimeLabel(uint16_t seconds) { return seconds ? std::to_string(seconds / 60) + " min" : "off"; }

}  // namespace hsm
