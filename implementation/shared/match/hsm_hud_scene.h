// Match HUD scene (MATCH_ROUNDS_UI_CHAT_PLAN 3.2): what the in-fight HUD shows, built from the wire MatchInfo, this
// machine's state and the time, so host and clients show the same thing. The client maps the scene to widgets and
// decides nothing; animations are functions of age. Game-independent.
//   score strip   top centre: you and the best rival, one block per team, or survivors against bots
//   time bar      under it, with the clock, while a time limit runs
//   pill          the phase: ROUND 3, FINAL ROUND, SUDDEN DEATH, WAVE 4 · 2 LEFT (FIGHT · 2 LEFT in a Progression
//                 fight), the winner at results
//   banner        centre: the countdown digits, or the last event (FIGHT, ROUND TO ANN, NEW GEAR, BLUE WINS, ...)
//   dead panel    lower third while this machine's fighter is down
//   kill feed     top right: the newest four rows, each fading on its own
//   scoreboard    on Tab, and at results
#pragma once
#include "match/hsm_ctf.h"
#include "match/hsm_giveup_action.h"
#include "match/hsm_hud_style.h"
#include "match/hsm_hud_text.h"
#include <algorithm>
#include <string>
#include <vector>

namespace hsm {

// ---------------------------------------------------------------- animation
struct Look { float opacity = 1, scale = 1; };

// A banner pops from 1.25x to 1x over kBannerPopMs, holds until kBannerFadeAtMs, then fades out by kBannerEndMs.
inline constexpr uint32_t kBannerPopMs = 250, kBannerFadeAtMs = 1750, kBannerEndMs = 2150;
inline Look BannerLook(uint32_t ageMs) {
    if (ageMs >= kBannerEndMs) return {0, 1};
    Look l;
    if (ageMs < kBannerPopMs) l.scale = 1.25f - 0.25f * float(ageMs) / float(kBannerPopMs);
    if (ageMs > kBannerFadeAtMs) l.opacity = 1 - float(ageMs - kBannerFadeAtMs) / float(kBannerEndMs - kBannerFadeAtMs);
    return l;
}

// A kill-feed row shows for kFeedShowMs, then fades over kFeedFadeMs. Rows not naming this machine's fighter are
// dimmer.
inline constexpr uint32_t kFeedShowMs = 5000, kFeedFadeMs = 1000;
inline constexpr size_t kFeedRows = 4;
inline constexpr float kFeedOthersOpacity = 0.8f;
inline float FeedOpacity(uint32_t ageMs) {
    if (ageMs <= kFeedShowMs) return 1;
    if (ageMs >= kFeedShowMs + kFeedFadeMs) return 0;
    return 1 - float(ageMs - kFeedShowMs) / float(kFeedFadeMs);
}

// Time since `atMs` (tick counts, wrap-safe); a time ahead of now (a clock that stepped back) counts as no time.
inline uint32_t AgeMs(uint32_t atMs, uint32_t nowMs) {
    uint32_t age = nowMs - atMs;
    return age > 0x80000000u ? 0 : age;
}

// ---------------------------------------------------------------- names and inks
inline const FighterEntry* FighterOf(const MatchInfo& m, uint8_t entity) {
    for (const FighterEntry& f : m.fighters) if (f.entity == entity) return &f;
    return nullptr;
}

// A fighter's name as text: team formats, its team's ink; the bot formats, bots in blood and players in bone;
// free-for-all and Duel, bone.
inline Rgba FighterInk(const MatchInfo& m, uint8_t entity) {
    const FighterEntry* f = FighterOf(m, entity);
    if (!f) return style::kBone;
    Format fmt = Format(m.format);
    if (IsBotFormat(fmt)) return f->team == kBotTeam ? style::kBloodInk : style::kBone;
    if (IsTag(fmt) && m.tagIt == entity) return style::kBloodInk;     // "it" is red for everyone
    if (HasTeams(fmt)) return style::TeamInkFor(fmt, f->team);
    return style::kBone;
}
// The swatch of a side on the board: Infection's infected wear the kit's dark green, its survivors have none.
inline Rgba SwatchFor(Format f, int team) {
    if (IsInfection(f)) return team == kInfectedTeam ? TeamColor(kInfectedKitTeam) : style::kBone;
    return TeamColor(team);
}

// Free-for-all and Duel: the other fighter with the best score, else (nobody else has scored) the first other
// fighter; 0 when alone.
inline uint8_t TopRival(const MatchInfo& m, uint8_t me) {
    uint8_t rival = 0; int best = 0;
    for (const ScoreEntry& e : m.scores)
        if (e.key != me && e.key > 0 && e.key < 256 && (rival == 0 || e.score > best)) { best = e.score; rival = uint8_t(e.key); }
    if (!rival)
        for (const FighterEntry& f : m.fighters) if (f.entity != me) return f.entity;
    return rival;
}

// Duel: the round being fought decides the match (both leaders one round from winning).
inline bool FinalRound(const MatchInfo& m) {
    int first = 0, second = 0;
    for (const ScoreEntry& e : m.scores) {
        if (e.score > first) { second = first; first = e.score; }
        else if (e.score > second) second = e.score;
    }
    return first == m.scoreLimit - 1 && second == m.scoreLimit - 1;
}

inline std::string RoundName(const MatchInfo& m) {
    if (IsQueueFormat(Format(m.format))) { const std::string s = QueueStage(m); return s.empty() ? "FIGHT" : Upper(s); }   // the stage, not "round 3"
    return FinalRound(m) ? "FINAL ROUND" : "ROUND " + std::to_string(m.round);
}

// The FIGHT banner's second line: the round (round formats) and, for a fighter with a role, what it is ("YOU ARE THE KNIGHT").
inline std::string FightSub(const MatchInfo& m, uint8_t me) {
    const std::string round = IsRoundFormat(Format(m.format)) ? RoundName(m) : std::string();
    const FighterEntry* f = FighterOf(m, me);
    std::string role = f ? RoleLine(f->role) : std::string();
    if (IsInfection(Format(m.format)) && f) role = f->team == kInfectedTeam ? "YOU ARE A PATIENT ZERO" : "SURVIVE";
    return role.empty() ? round : round.empty() ? role : round + " \xC2\xB7 " + role;
}

// ---------------------------------------------------------------- banners
struct BannerShot { std::string text, sub; Rgba color = style::kBone; uint32_t atMs = 0; };

// The results banner (and the pill while results show).
inline BannerShot ResultShot(const MatchInfo& m, uint8_t me) {
    Format f = Format(m.format);
    if (f == Format::Zombies) return {"OVERRUN", "SURVIVED " + std::to_string(ScoreOf(m, kHumanTeam)) + " WAVES", style::kBloodInk};
    if (m.winner == Match::kNoWinner) return {"DRAW", "", style::kIron};
    if (f == Format::PlayersVsBots) return m.winner == kBotTeam ? BannerShot{"BOTS WIN", "", style::kBloodInk} : BannerShot{"PLAYERS WIN", "", style::kOk};
    if (ScoresPerFighter(f))
        return m.winner == int(me) ? BannerShot{"YOU WIN", "", style::kWarn} : BannerShot{Upper(NameOf(m, uint8_t(m.winner))) + " WINS", "", style::kBloodInk};
    if (IsInfection(f)) return m.winner == kSurvivorTeam ? BannerShot{"SURVIVORS WIN", "", style::kOk} : BannerShot{"INFECTED WIN", "", style::kInfectedInk};
    return {TeamName(m.winner) + " WINS", "", style::TeamInk(m.winner)};
}

// Fires banners from successive MatchInfo (feed it each one as it is made or arrives): FIGHT when the countdown
// ends and when a Duel round starts, ROUND TO X when a Duel round is taken, SUDDEN DEATH, WAVE CLEARED, the
// winner at results. The session fires the rest (NEW GEAR). One at a time: the newest wins. The first MatchInfo,
// and the first of another format, only record: joining a running match fires nothing.
class BannerTracker {
public:
    void Observe(const MatchInfo& m, uint8_t me, uint32_t nowMs) {
        bool gap = m.roundGapLeftMs > 0;
        int cleared = ScoreOf(m, kHumanTeam);
        if (seen_ && m.format == format_) {
            Format f = Format(m.format);
            MatchPhase p = MatchPhase(m.phase), prev = MatchPhase(phase_);
            bool fighting = p == MatchPhase::Live || p == MatchPhase::SuddenDeath;
            if (p != prev) {
                if (p == MatchPhase::Live && prev == MatchPhase::Countdown) Fire("FIGHT", FightSub(m, me), style::kBloodInk, nowMs);
                else if (p == MatchPhase::SuddenDeath)
                    Fire("SUDDEN DEATH", f == Format::FreeForAll ? "NEXT KILL WINS" : IsRoundFormat(f) ? "NEXT ROUND WINS"
                                         : IsBotFormat(f) ? "CLEAR THE WAVE TO WIN" : IsCtf(f) ? "NEXT CAPTURE WINS" : "NEXT POINT WINS", style::kBloodInk, nowMs);
                else if (p == MatchPhase::Results) { shot_ = ResultShot(m, me); shot_.atMs = nowMs; }
            } else if (fighting && IsRoundFormat(f)) {
                if (gap && !gap_) {
                    if (!m.roundWinner) Fire("ROUND DRAWN", "ROUND " + std::to_string(m.round), style::kIron, nowMs);
                    else {
                        const std::string taker = RoundTaker(m, me);               // "you" / "your team" are ours
                        Fire("ROUND TO " + Upper(taker), "ROUND " + std::to_string(m.round), taker == "you" || taker == "your team" ? style::kOk : style::kBloodInk, nowMs);
                    }
                } else if (m.round > round_) Fire("FIGHT", FightSub(m, me), style::kBloodInk, nowMs);
            } else if (fighting && IsBotFormat(f) && cleared > cleared_) {
                Fire("WAVE CLEARED", std::to_string(cleared) + " CLEARED", style::kOk, nowMs);
            }
            if (fighting && p == prev && IsTag(f) && m.tagIt == me && tagIt_ != me)
                Fire("YOU ARE IT", "HIT SOMEONE TO PASS IT ON", style::kBloodInk, nowMs);
            const FighterEntry* mine = FighterOf(m, me);
            if (fighting && p == prev && IsInfection(f) && mine && mine->team == kInfectedTeam && team_ != kInfectedTeam)
                Fire("INFECTED", "NOW INFECT THE REST", style::kInfectedInk, nowMs);
        }
        { const FighterEntry* mine = FighterOf(m, me); team_ = mine ? mine->team : kNoTeam; }
        tagIt_ = m.tagIt;
        seen_ = true; format_ = m.format; phase_ = m.phase; round_ = m.round; gap_ = gap; cleared_ = cleared;
    }
    void Fire(std::string text, std::string sub, Rgba color, uint32_t nowMs) { shot_ = {std::move(text), std::move(sub), color, nowMs}; }
    const BannerShot& shot() const { return shot_; }
    void Reset() { *this = BannerTracker{}; }

private:
    BannerShot shot_;
    bool seen_ = false, gap_ = false;
    uint8_t format_ = 0, phase_ = 0;
    uint16_t round_ = 0;
    int cleared_ = 0;
    uint8_t tagIt_ = 0;               // Tag: who was "it" in the last MatchInfo
    int8_t team_ = kNoTeam;           // Infection: this machine's fighter's team in the last MatchInfo
};

// ---------------------------------------------------------------- kill feed
struct Span { std::string text; Rgba color = style::kBone; };
struct FeedRow { std::vector<Span> spans; bool mine = false; };   // mine: names this machine's fighter

// A flag event as a feed row: "Ann" (team ink) " took the " "BLUE flag" (that team's ink). Made when the event arrives.
inline FeedRow MakeCtfFeedRow(const MatchInfo& m, const CtfEvent& e, uint8_t me) {
    FeedRow r;
    r.mine = e.entity != 0 && e.entity == me;
    const char* words = e.kind == CtfEventKind::Taken ? " took the " : e.kind == CtfEventKind::Dropped ? " dropped the "
                      : e.kind == CtfEventKind::Returned ? " returned the " : e.kind == CtfEventKind::Captured ? " captured the " : "";
    const Span flag{TeamName(e.flag) + " flag", style::TeamInk(e.flag)};
    if (e.kind == CtfEventKind::Expired) r.spans = {{"The ", style::kIron}, flag, {" went home", style::kIron}};
    else r.spans = {{NameOf(m, e.entity), FighterInk(m, e.entity)}, {words, style::kIron}, flag};
    return r;
}
struct FeedItem { uint32_t atMs = 0; FeedRow row; };

// The words of KillLine, the names in their inks and the rest in iron. Made when the kill arrives, so a fighter
// who leaves later keeps their name in the feed.
inline FeedRow MakeFeedRow(const MatchInfo& m, const Kill& k, uint8_t me) {
    FeedRow r;
    r.mine = k.victim == me || (k.killer == me && k.killer != 0);
    Span victim{NameOf(m, k.victim), FighterInk(m, k.victim)};
    if (k.flags & (16 | 32)) {
        if (k.killer == 0) r.spans = {victim, {DrawnWhy(k), style::kIron}};
        else r.spans = {victim, {" and ", style::kIron}, {NameOf(m, k.killer), FighterInk(m, k.killer)}, {DrawnWhy(k), style::kIron}};
    } else if (k.flags & 8) r.spans = {victim, {" gave up", style::kIron}};
    else if (k.flags & 2) r.spans = {victim, {" killed themselves", style::kIron}};
    else if ((k.flags & 4) || k.killer == 0) r.spans = {victim, {" died", style::kIron}};
    else r.spans = {{NameOf(m, k.killer), FighterInk(m, k.killer)}, {(k.flags & 1) ? " killed teammate " : " killed ", style::kIron}, victim};
    return r;
}

// A mode event's feed row (hsm_modes.h): "Ann tagged Bo" with the names in their inks; mine when it names this machine's fighter.
inline FeedRow MakeModeFeedRow(const MatchInfo& m, const ModeEventMsg& e, uint8_t me) {
    FeedRow r;
    r.mine = (e.a == me && e.a != 0) || (e.b == me && e.b != 0);
    const Span a{NameOf(m, e.a), FighterInk(m, e.a)}, b{NameOf(m, e.b), FighterInk(m, e.b)};
    switch (e.kind) {
        case ModeEventKind::TagPass: r.spans = {a, {" tagged ", style::kIron}, b}; break;
        case ModeEventKind::TagBurn: r.spans = {a, {" burned out", style::kIron}}; break;
        case ModeEventKind::Infected:
            if (e.a) r.spans = {a, {" infected ", style::kIron}, b};
            else r.spans = {b, {" was infected", style::kIron}};
            break;
    }
    return r;
}

// ---------------------------------------------------------------- the scene
struct LocalHud {
    uint8_t me = 0;                  // this machine's fighter
    uint8_t host = 1;                // the host's fighter (HOST badge)
    uint32_t infoAtMs = 0;           // when the MatchInfo was made (host) or arrived (client): its clocks run on
    bool dead = false;
    uint32_t diedAtMs = 0;
    bool tab = false;                // the scoreboard key is held
    std::string map;                 // display name, for the scoreboard title
    bool oneFight = false;           // a Progression fight (Free Mode off): the host plays it as one fight, no waves
    BannerShot banner;               // BannerTracker::shot()
    std::vector<FeedItem> feed;      // oldest first
    bool giveUpWait = false;         // G is held with nothing to give up (dead, or waiting for the next round)
    float giveUp = 0;                // how far the G hold is (GiveUpHold::progress), 0 = not held
    uint8_t watching = 0;            // dead: the fighter the camera follows, 0 = none (the F key needs one)
    bool flying = false;             // dead: the free camera is on
};

struct SceneText { bool visible = false; std::string text; Rgba color = style::kBone; float opacity = 1, scale = 1; };
struct ScoreBlock { std::string label; int score = 0; Rgba color = style::kBone; bool mine = false; };
struct FeedView { FeedRow row; float opacity = 1; };
struct DeadPanel { bool visible = false; std::string title, detail, hint; bool bar = false; float barFrac = 0; };
struct BoardRow {
    enum class Kind : uint8_t { Band, Fighter };
    Kind kind = Kind::Fighter;
    std::string name;
    Rgba swatch = style::kIron, ink = style::kBone;
    int kills = 0, deaths = 0, score = 0;
    uint16_t pingMs = 0;
    bool host = false, local = false, dead = false;
    bool down = false;               // alive, and the host's draw check has it down (Duel)
    std::string tag;                 // the fighter's special role (hsm_roles.h RoleTag), shown beside its name; empty for the rest
};
struct Board { bool visible = false; std::string title, footer; std::vector<BoardRow> rows; };
// The G hold, in the middle of the screen (the top pill was covered by the game's own text in the pre1 test).
struct HoldView { bool visible = false; std::string text; Rgba color = style::kBloodInk; bool bar = false; float frac = 0; };

struct HudScene {
    bool visible = false;
    std::vector<ScoreBlock> strip;
    std::vector<SceneText> flags;    // Capture the flag: one line per flag under the strip ("RED FLAG: taken by Ann")
    bool timeBar = false; float timeFrac = 0; std::string clock;
    SceneText pill, banner, bannerSub;
    SceneText info;                  // under the pill: Tournament's pairing and what is next, Pit Bridge Duels' streak and queue
    HoldView hold;
    DeadPanel dead;
    std::vector<FeedView> feed;
    Board board;
};

namespace hud_detail {

inline std::vector<ScoreBlock> Strip(const MatchInfo& m, uint8_t me) {
    Format f = Format(m.format);
    const FighterEntry* mine = FighterOf(m, me);
    if (IsBotFormat(f)) {
        int humans = 0, bots = 0;
        for (const FighterEntry& e : m.fighters) if (e.alive) ++(e.team == kBotTeam ? bots : humans);
        return {{"SURVIVORS", humans, style::kBone, true}, {"BOTS", bots, style::kBloodInk, false}};
    }
    std::vector<ScoreBlock> s;
    if (HasTeams(f)) {
        for (int t = 0; t < TeamCount(f, m.teams); ++t) s.push_back({TeamName(f, t), ScoreOf(m, t), style::TeamInkFor(f, t), mine && mine->team == t});
        return s;
    }
    s.push_back({"YOU", ScoreOf(m, me), style::kBone, true});
    if (uint8_t rival = TopRival(m, me)) s.push_back({Upper(NameOf(m, rival)), ScoreOf(m, rival), style::kIron, false});
    return s;
}

inline SceneText Pill(const MatchInfo& m, uint8_t me, bool oneFight) {
    Format f = Format(m.format);
    SceneText p; p.visible = true;
    switch (MatchPhase(m.phase)) {
        case MatchPhase::Idle: return {};
        case MatchPhase::Countdown: p.text = "MATCH STARTS IN"; return p;
        case MatchPhase::Results: { BannerShot r = ResultShot(m, me); p.text = r.text; p.color = r.color; return p; }
        default: break;
    }
    if (InRoundGap(m)) { p.text = "NEXT ROUND IN " + Seconds(m.roundGapLeftMs); return p; }
    if (InWaveBreak(m)) { p.text = "NEXT WAVE IN " + Seconds(m.roundGapLeftMs); return p; }
    if (m.phase == uint8_t(MatchPhase::SuddenDeath)) { p.text = "SUDDEN DEATH"; p.color = style::kBloodInk; return p; }
    if (IsRoundFormat(f)) {
        p.text = RoundName(m); if (IsQueueFormat(f) ? QueueFinal(m) : FinalRound(m)) p.color = style::kWarn;
        if (IsTag(f) && m.tagIt) {                                // "YOU ARE IT 12 s" / "ROUND 2 · Ann is it 12 s"
            p.text = m.tagIt == me ? TagLine(m, me) : p.text + " \xC2\xB7 " + TagLine(m, me);
            if (m.tagIt == me) p.color = style::kBloodInk;
        }
        return p;
    }
    if (IsInfection(f)) {
        const FighterEntry* mine = FighterOf(m, me);
        if (mine && mine->team == kInfectedTeam) { p.text = "YOU ARE INFECTED"; p.color = style::kInfectedInk; }
        else p.text = "SURVIVE \xC2\xB7 " + std::to_string(ScoreOf(m, kSurvivorTeam)) + " LEFT";
        return p;
    }
    if (IsBotFormat(f)) {
        int bots = 0;
        for (const FighterEntry& e : m.fighters) if (e.team == kBotTeam && e.alive && !e.down) ++bots;   // an opponent that gave up is out
        std::string left = bots ? " \xC2\xB7 " + std::to_string(bots) + " LEFT" : "";
        if (oneFight) p.text = "FIGHT" + left;                    // a Progression fight has no waves
        else p.text = "WAVE " + std::to_string(ScoreOf(m, kHumanTeam) + 1) + left;
        return p;
    }
    p.text = "FIRST TO " + std::to_string(m.scoreLimit) + (IsCtf(f) ? (m.scoreLimit == 1 ? " CAPTURE" : " CAPTURES") : "");
    return p;
}

inline DeadPanel Dead(const MatchInfo& m, const LocalHud& l, uint32_t nowMs) {
    DeadPanel d;
    const QueueRole role = RoleInQueue(m, l.me);
    const bool benched = role == QueueRole::Waiting || role == QueueRole::Out;
    if ((!l.dead && !benched) || (m.phase != uint8_t(MatchPhase::Live) && m.phase != uint8_t(MatchPhase::SuddenDeath))) return d;
    d.visible = true;
    Format f = Format(m.format);
    if (benched) {                               // Tournament, Pit Bridge Duels: no body, watching the others
        d.title = role == QueueRole::Out ? "OUT OF THE BRACKET" : "ON THE BENCH";
        if (f == Format::PitBridgeDuel) {
            size_t at = 0;
            while (at < m.queueOrder.size() && m.queueOrder[at] != l.me) ++at;
            d.detail = at == 2 ? "you fight next" : at > 2 && at < m.queueOrder.size() ? "#" + std::to_string(at - 1) + " in line" : std::string();
        } else d.detail = role == QueueRole::Out ? "watching the rest" : "your fight is coming";
    } else if (IsRoundFormat(f)) {               // the round is over; everybody returns together after the gap
        d.title = "ROUND OVER";
        if (m.roundGapLeftMs) {
            d.detail = "next round in " + Seconds(m.roundGapLeftMs);
            uint32_t gap = uint32_t(m.respawnDelayS) * 1000u;
            d.bar = true; d.barFrac = gap ? std::clamp(float(m.roundGapLeftMs) / float(gap), 0.f, 1.f) : 0.f;
        }
    } else if (IsBotFormat(f)) {
        d.title = "DOWN"; d.detail = l.oneFight ? "out until the fight ends" : "back next wave";
    } else if (const FighterEntry* mine = FighterOf(m, l.me); IsInfection(f) && mine && mine->team != kInfectedTeam) {
        d.title = "ELIMINATED"; d.detail = "out until the rematch";      // killed by anything but the infected: no respawn
    } else {
        d.title = "RESPAWNING";
        uint32_t delay = uint32_t(m.respawnDelayS) * 1000u, since = AgeMs(l.diedAtMs, nowMs);
        if (since < delay) { d.detail = "in " + Seconds(delay - since); d.bar = true; d.barFrac = float(delay - since) / float(delay); }
    }
    for (const FighterEntry& e : m.fighters)
        if (e.entity != l.me && e.alive) { d.hint = "LEFT / RIGHT   watch another fighter"; break; }
    // F: only when the host allows the free camera (MatchInfo.freeCam); nothing about it otherwise.
    if (m.freeCam) {
        if (l.flying) d.hint = l.watching ? "F: follow " + NameOf(m, l.watching) : std::string("F: follow");
        else if (l.watching) d.hint += std::string(d.hint.empty() ? "" : "   ") + "F: free camera";
    }
    return d;
}

inline std::vector<FeedView> Feed(const LocalHud& l, uint32_t nowMs) {
    std::vector<FeedView> v;
    for (const FeedItem& it : l.feed) {
        uint32_t age = AgeMs(it.atMs, nowMs);
        if (age >= kFeedShowMs + kFeedFadeMs) continue;
        v.push_back({it.row, FeedOpacity(age) * (it.row.mine ? 1.f : kFeedOthersOpacity)});
    }
    if (v.size() > kFeedRows) v.erase(v.begin(), v.end() - kFeedRows);
    return v;
}

inline Board MakeBoard(const MatchInfo& m, const LocalHud& l) {
    Board b;
    if (m.phase == uint8_t(MatchPhase::Idle) || (!l.tab && m.phase != uint8_t(MatchPhase::Results))) return b;
    b.visible = true;
    Format f = Format(m.format);
    std::string limit = f == Format::Tournament ? std::string("ONE ROUND A FIGHT")
                      : f == Format::Duel ? "BEST OF " + std::to_string(2 * m.scoreLimit - 1)
                      : IsBotFormat(f) ? "WAVE " + std::to_string(ScoreOf(m, kHumanTeam) + 1)
                      : IsInfection(f) ? "SURVIVE THE CLOCK"
                      : IsRoundFormat(f) ? "FIRST TO " + std::to_string(m.scoreLimit) + (IsTag(f) ? " ROUNDS" : "")
                      : "FIRST TO " + std::to_string(m.scoreLimit);
    bool oneFight = l.oneFight && IsBotFormat(f);               // Progression: back to the Tavern after the results
    b.title = (oneFight ? std::string("PROGRESSION FIGHT") : Upper(ModeName(f, Gear(m.gear), m.teams)) + " \xC2\xB7 " + limit)
            + (l.map.empty() ? "" : " \xC2\xB7 " + Upper(l.map));
    if (m.phase == uint8_t(MatchPhase::Results))
        b.footer = CoopFightWon(m, l.oneFight) ? "FIGHT WON \xC2\xB7 HOLD G TO GO ON \xC2\xB7 OR IN " + Seconds(m.timeLeftMs)
                 : (oneFight ? "BACK TO THE TAVERN IN " : "REMATCH IN ") + Seconds(m.timeLeftMs);

    // Fighters by score, then kills (free-for-all and Duel score per fighter; elsewhere a fighter's score is kills).
    bool perEntity = ScoresPerFighter(f);
    Bracket bracket;
    const bool tournament = f == Format::Tournament && bracket.Restore(m.queueOrder, m.bracketWinners);
    std::vector<std::pair<int8_t, BoardRow>> rows;               // with the team, for grouping
    for (const FighterEntry& e : m.fighters) {
        if (IsBotFormat(f) && e.team == kBotTeam) continue;     // a wave of "Bot" rows says nothing
        BoardRow r;
        r.name = e.name.empty() ? "#" + std::to_string(e.entity) : e.name;
        r.swatch = HasTeams(f) ? SwatchFor(f, e.team) : style::kIron;
        r.ink = e.alive ? style::kBone : style::kIron;
        r.kills = e.kills; r.deaths = e.deaths; r.score = perEntity ? ScoreOf(m, e.entity) : e.kills;
        r.pingMs = e.pingMs; r.host = e.entity == l.host; r.local = e.entity == l.me; r.dead = !e.alive;
        r.down = e.alive && e.down;
        r.tag = IsTag(f) && m.tagIt == e.entity ? "IT" : RoleTag(e.role);
        if (IsQueueFormat(f)) {                                  // on the bench is not dead; a beaten bracket player is out
            const QueueRole q = RoleInQueue(m, e.entity);
            const bool out = tournament && q == QueueRole::Out;
            r.dead = out; r.ink = out ? style::kIron : style::kBone;
            r.tag = out ? "OUT" : q == QueueRole::Waiting ? "WAIT" : "";
        }
        rows.push_back({e.team, std::move(r)});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& c) {
        return a.second.score != c.second.score ? a.second.score > c.second.score : a.second.kills > c.second.kills;
    });
    if (!HasTeams(f)) {
        for (auto& [team, r] : rows) b.rows.push_back(std::move(r));
        return b;
    }
    // Team formats: a band per team (its name, colour and score), its fighters under it; anyone outside the teams last.
    int teams = TeamCount(f, m.teams);
    for (int t = 0; t <= teams; ++t) {
        if (t < teams) {
            BoardRow band;
            band.kind = BoardRow::Kind::Band; band.name = TeamName(f, t); band.swatch = SwatchFor(f, t); band.ink = style::TeamInkFor(f, t);
            band.score = ScoreOf(m, t);
            b.rows.push_back(std::move(band));
        }
        for (auto& [team, r] : rows)
            if (t < teams ? team == t : (team < 0 || team >= teams)) b.rows.push_back(r);
    }
    return b;
}
}  // namespace hud_detail

inline HudScene BuildHudScene(const MatchInfo& info, const LocalHud& l, uint32_t nowMs) {
    HudScene s;
    if (info.phase == uint8_t(MatchPhase::Idle)) return s;
    const MatchInfo m = Extrapolate(info, l.infoAtMs, nowMs);
    s.visible = true;
    s.strip = hud_detail::Strip(m, l.me);
    if (m.timeLimitS && m.phase == uint8_t(MatchPhase::Live) && !InRoundGap(m)) {
        s.timeBar = true;
        s.timeFrac = std::clamp(float(m.timeLeftMs) / (float(m.timeLimitS) * 1000.f), 0.f, 1.f);
        s.clock = Clock(m.timeLeftMs);
    }
    s.pill = hud_detail::Pill(m, l.me, l.oneFight);
    if (IsCtf(Format(m.format)) && m.phase != uint8_t(MatchPhase::Results))
        for (const Objective& o : m.objectives) {
            if (o.kind != ObjectiveKind::Flag) continue;
            const std::string who = o.state == ObjectiveState::Carried ? NameOf(m, o.carrier) : std::string();
            s.flags.push_back({true, FlagStatusLine(o, who), style::TeamInk(o.owner), 1, 1});
        }
    if (const std::string line = QueueLine(m); !line.empty()) s.info = {true, line, style::kIron, 1, 1};
    const bool fought = m.phase == uint8_t(MatchPhase::Live) || m.phase == uint8_t(MatchPhase::SuddenDeath);
    // Item 9c: down by the draw check (fallen, knocked out, or the pelvis low over the feet), not just the game's flags.
    // A Progression co-op fight has no stock give-up screen in a session: G gives the fight up (Match::GiveUpFight).
    // Free-for-all, teams and Duel: G ends this life at any time; the last attacker within a minute gets the kill.
    // Survival and Zombies waves: G ends this life too (the fighter comes back when the wave is cleared).
    const GiveUpAction act = GiveUpActionFor(m, !l.dead, l.dead, l.oneFight);
    const bool coop = act == GiveUpAction::GiveUpFight;
    const bool pvp = act == GiveUpAction::GiveUp;
    if (const FighterEntry* mine = FighterOf(m, l.me); mine && mine->alive && mine->down && fought && !InRoundGap(m) && !l.dead)
        s.pill = {true, coop || pvp ? "DOWN - GET UP OR HOLD G" : "DOWN - GET UP", style::kBloodInk, 1, 1};
    const float held = std::clamp(l.giveUp, 0.f, 1.f);
    const std::string pct = " " + std::to_string(int(held * 100.f)) + "%";
    if (l.giveUp > 0 && coop) s.hold = {true, "GIVING UP THE FIGHT" + pct, style::kBloodInk, true, held};
    if (l.giveUp > 0 && pvp) s.hold = {true, "GIVING UP" + pct, style::kBloodInk, true, held};
    if (l.giveUp > 0 && InWaveBreak(m))          // the same G hold votes to skip the loot break
        s.hold = {true, "SKIPPING THE BREAK" + pct, style::kOk, true, held};
    if (act == GiveUpAction::ClaimWin)           // a Progression fight won: G goes on, as the stock Talk key does
        s.hold = l.giveUp > 0 ? HoldView{true, "GOING ON" + pct, style::kOk, true, held}
                              : HoldView{true, "FIGHT WON - HOLD G TO GO ON (OR IN " + Seconds(m.timeLeftMs) + ")", style::kOk, false, 0};
    if (l.giveUpWait && fought)
        s.hold = {true, m.roundGapLeftMs ? "NOTHING TO GIVE UP - NEXT ROUND IN " + Seconds(m.roundGapLeftMs) : std::string("NOTHING TO GIVE UP - YOU ARE OUT"), style::kIron, false, 0};
    if (m.phase == uint8_t(MatchPhase::Countdown)) {
        if (m.timeLeftMs) {                      // the digits, each popping as it appears
            Look look = BannerLook((1000 - m.timeLeftMs % 1000) % 1000);
            s.banner = {true, Seconds(m.timeLeftMs), style::kBone, look.opacity, look.scale};
            if (!IsBotFormat(Format(m.format))) s.bannerSub = {true, "HOLD G TO GIVE UP - YOUR LAST ATTACKER GETS THE KILL", style::kIron, look.opacity, 1};
            else if (l.oneFight && Format(m.format) == Format::PlayersVsBots)
                s.bannerSub = {true, "HOLD G TO GIVE UP THE FIGHT", style::kIron, look.opacity, 1};
            else s.bannerSub = {true, "STUCK? HOLD G TO GIVE UP", style::kIron, look.opacity, 1};
        }
    } else if (!l.banner.text.empty()) {
        uint32_t age = AgeMs(l.banner.atMs, nowMs);
        if (age < kBannerEndMs) {
            Look look = BannerLook(age);
            s.banner = {true, l.banner.text, l.banner.color, look.opacity, look.scale};
            if (!l.banner.sub.empty()) s.bannerSub = {true, l.banner.sub, l.banner.color, look.opacity, look.scale};
        }
    }
    s.dead = hud_detail::Dead(m, l, nowMs);
    s.feed = hud_detail::Feed(l, nowMs);
    s.board = hud_detail::MakeBoard(m, l);
    return s;
}

}  // namespace hsm
