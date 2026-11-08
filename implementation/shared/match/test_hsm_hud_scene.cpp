// Match HUD scene (MATCH_ROUNDS_UI_CHAT_PLAN 3.2): what the HUD shows per phase and format, the banner and kill-feed
// animations as functions of age, banners fired by match events, the dead panel, the scoreboard order, and the
// clocks running between MatchInfo updates.
#include "match/hsm_hud_scene.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using namespace hsm::style;

static bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }
static bool Same(Rgba a, Rgba b) { return Near(a.r, b.r) && Near(a.g, b.g) && Near(a.b, b.b) && Near(a.a, b.a); }
static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

constexpr uint32_t kAt = 1000;   // when the test MatchInfo arrived; scenes built at kAt see it unchanged

static MatchInfo Ffa() {
    MatchInfo m; m.format = uint8_t(Format::FreeForAll); m.scoreLimit = 5; m.phase = uint8_t(MatchPhase::Live);
    m.timeLimitS = 300; m.timeLeftMs = 150000; m.respawnDelayS = 5;
    m.scores = {{1, 2}, {2, 4}, {3, 1}};
    m.fighters = {{1, -1, 2, 1, "Host", 0, true}, {2, -1, 4, 0, "Ann", 80, true}, {3, -1, 1, 3, "Bo", 120, false}};
    return m;
}
static MatchInfo Duel() {
    MatchInfo m; m.format = uint8_t(Format::Duel); m.scoreLimit = 3; m.phase = uint8_t(MatchPhase::Live);
    m.respawnDelayS = 4; m.round = 2;
    m.scores = {{1, 1}, {2, 0}};
    m.fighters = {{1, -1, 1, 0, "Host", 0, true}, {2, -1, 0, 1, "Ann", 60, true}};
    return m;
}
static MatchInfo Teams() {
    MatchInfo m; m.format = uint8_t(Format::TwoTeams); m.scoreLimit = 10; m.phase = uint8_t(MatchPhase::Live);
    m.timeLimitS = 600; m.timeLeftMs = 300000; m.respawnDelayS = 5;
    m.scores = {{0, 3}, {1, 5}};
    m.fighters = {{1, 0, 3, 1, "Host", 0, true}, {2, 1, 5, 0, "Ann", 70, true}, {3, 0, 0, 4, "Bo", 90, false}, {4, 1, 2, 2, "Cy", 50, true}};
    return m;
}
static MatchInfo Survival() {
    MatchInfo m; m.format = uint8_t(Format::PlayersVsBots); m.scoreLimit = 20; m.phase = uint8_t(MatchPhase::Live);
    m.scores = {{kHumanTeam, 3}};
    m.fighters = {{1, kHumanTeam, 2, 1, "Host", 0, true}, {2, kHumanTeam, 1, 0, "Ann", 70, false},
                  {10, kBotTeam, 0, 0, "Bot", 0, true}, {11, kBotTeam, 0, 0, "Bot", 0, true}, {12, kBotTeam, 0, 1, "Bot", 0, false}};
    return m;
}
static LocalHud Me(uint8_t me = 1) { LocalHud l; l.me = me; l.host = 1; l.infoAtMs = kAt; return l; }
static std::string Joined(const FeedRow& r) { std::string s; for (const Span& p : r.spans) s += p.text; return s; }

static void TestBannerLook() {
    CHECK(Near(BannerLook(0).scale, 1.25) && Near(BannerLook(0).opacity, 1));          // pops in large
    CHECK(Near(BannerLook(125).scale, 1.125) && Near(BannerLook(125).opacity, 1));
    CHECK(Near(BannerLook(250).scale, 1.0) && Near(BannerLook(250).opacity, 1));        // settled at 250 ms
    CHECK(Near(BannerLook(1000).scale, 1.0) && Near(BannerLook(1000).opacity, 1));
    CHECK(Near(BannerLook(1750).scale, 1.0) && Near(BannerLook(1750).opacity, 1));      // held 1.5 s
    CHECK(Near(BannerLook(1950).opacity, 0.5) && Near(BannerLook(1950).scale, 1.0));    // then a 400 ms fade
    CHECK(Near(BannerLook(2150).opacity, 0) && Near(BannerLook(2200).opacity, 0) && Near(BannerLook(2249).opacity, 0));
    CHECK(Near(BannerLook(60000).opacity, 0));
    CHECK(kBannerPopMs == 250 && kBannerFadeAtMs == 1750 && kBannerEndMs == 2150);
}

static void TestFeedOpacity() {
    CHECK(Near(FeedOpacity(0), 1) && Near(FeedOpacity(4999), 1) && Near(FeedOpacity(5000), 1));   // 5 s shown
    CHECK(Near(FeedOpacity(5500), 0.5) && Near(FeedOpacity(5900), 0.1, 1e-3));                     // then a 1 s fade
    CHECK(Near(FeedOpacity(6000), 0) && Near(FeedOpacity(90000), 0));
    CHECK(kFeedShowMs == 5000 && kFeedFadeMs == 1000 && kFeedRows == 4);
}

static void TestIdleShowsNothing() {
    MatchInfo m = Ffa(); m.phase = uint8_t(MatchPhase::Idle);
    LocalHud l = Me(); l.tab = true; l.dead = true; l.banner = {"FIGHT", "", kBloodInk, kAt};
    l.feed = {{kAt, MakeFeedRow(m, {2, 1, 0}, 1)}};
    HudScene s = BuildHudScene(m, l, kAt);
    CHECK(!s.visible && s.strip.empty() && !s.timeBar && !s.pill.visible && !s.banner.visible && !s.bannerSub.visible);
    CHECK(!s.dead.visible && s.feed.empty() && !s.board.visible);
    m.phase = uint8_t(MatchPhase::Live);
    CHECK(BuildHudScene(m, l, kAt).visible);
    for (uint8_t p : {uint8_t(MatchPhase::Countdown), uint8_t(MatchPhase::SuddenDeath), uint8_t(MatchPhase::Results)}) {
        m.phase = p;
        HudScene t = BuildHudScene(m, Me(), kAt);
        CHECK(t.visible && !t.strip.empty() && t.pill.visible);   // the strip and the pill in every running phase
    }
}

static void TestStrip() {
    HudScene s = BuildHudScene(Ffa(), Me(1), kAt);
    CHECK(s.strip.size() == 2);
    if (s.strip.size() == 2) {
        CHECK(s.strip[0].label == "YOU" && s.strip[0].score == 2 && s.strip[0].mine && Same(s.strip[0].color, kBone));
        CHECK(s.strip[1].label == "ANN" && s.strip[1].score == 4 && !s.strip[1].mine && Same(s.strip[1].color, kIron));   // the best rival
    }
    s = BuildHudScene(Ffa(), Me(2), kAt);
    CHECK(s.strip.size() == 2 && s.strip[0].label == "YOU" && s.strip[0].score == 4 && s.strip[1].label == "HOST" && s.strip[1].score == 2);
    MatchInfo d = Duel(); d.scores = {{1, 1}};                    // no score yet for the rival: named from the fighters
    s = BuildHudScene(d, Me(1), kAt);
    CHECK(s.strip.size() == 2 && s.strip[1].label == "ANN" && s.strip[1].score == 0);
    MatchInfo alone = Ffa(); alone.scores = {{1, 0}}; alone.fighters = {{1, -1, 0, 0, "Host"}};
    CHECK(BuildHudScene(alone, Me(1), kAt).strip.size() == 1);

    s = BuildHudScene(Teams(), Me(1), kAt);
    CHECK(s.strip.size() == 2);
    if (s.strip.size() == 2) {
        CHECK(s.strip[0].label == "RED" && s.strip[0].score == 3 && s.strip[0].mine && Same(s.strip[0].color, TeamInk(0)));
        CHECK(s.strip[1].label == "BLUE" && s.strip[1].score == 5 && !s.strip[1].mine && Same(s.strip[1].color, TeamInk(1)));
    }
    s = BuildHudScene(Teams(), Me(2), kAt);                       // your own team's block is marked
    CHECK(s.strip.size() == 2 && !s.strip[0].mine && s.strip[1].mine);
    MatchInfo three = Teams(); three.teams = 3; three.scores = {{0, 1}, {2, 4}};
    s = BuildHudScene(three, Me(1), kAt);
    CHECK(s.strip.size() == 3 && s.strip[2].label == "GREEN" && s.strip[2].score == 4 && s.strip[1].score == 0);
    MatchInfo four = Teams(); four.teams = 4; four.scores = {{0, 1}, {3, 6}};
    s = BuildHudScene(four, Me(1), kAt);
    CHECK(s.strip.size() == 4 && s.strip[3].label == "GOLD" && s.strip[3].score == 6 && Same(s.strip[3].color, TeamInk(3)));
    MatchInfo old = Teams(); old.format = uint8_t(Format::ThreeTeams);                    // the old value is three teams, whatever the count
    CHECK(BuildHudScene(old, Me(1), kAt).strip.size() == 3);

    s = BuildHudScene(Survival(), Me(1), kAt);                    // standing humans against standing bots
    CHECK(s.strip.size() == 2);
    if (s.strip.size() == 2) {
        CHECK(s.strip[0].label == "SURVIVORS" && s.strip[0].score == 1 && s.strip[0].mine && Same(s.strip[0].color, kBone));
        CHECK(s.strip[1].label == "BOTS" && s.strip[1].score == 2 && Same(s.strip[1].color, kBloodInk));
    }
}

static void TestTimeBar() {
    HudScene s = BuildHudScene(Ffa(), Me(), kAt);
    CHECK(s.timeBar && Near(s.timeFrac, 0.5) && s.clock == "2:30");
    s = BuildHudScene(Ffa(), Me(), kAt + 30000);                 // runs between updates
    CHECK(s.timeBar && Near(s.timeFrac, 0.4) && s.clock == "2:00");
    s = BuildHudScene(Teams(), Me(), kAt);
    CHECK(s.timeBar && Near(s.timeFrac, 0.5) && s.clock == "5:00");
    s = BuildHudScene(Ffa(), Me(), kAt + 400000);                // stops at 0
    CHECK(Near(s.timeFrac, 0) && s.clock == "0:00");
    MatchInfo m = Ffa(); m.timeLimitS = 0; m.timeLeftMs = 0;
    CHECK(!BuildHudScene(m, Me(), kAt).timeBar);                 // no time limit: no bar
    m = Ffa(); m.phase = uint8_t(MatchPhase::SuddenDeath); m.timeLeftMs = 0;
    CHECK(!BuildHudScene(m, Me(), kAt).timeBar);
    m = Ffa(); m.phase = uint8_t(MatchPhase::Results); m.timeLeftMs = 9000;
    CHECK(!BuildHudScene(m, Me(), kAt).timeBar);                 // the rematch countdown is not the time limit
    m = Duel(); m.timeLimitS = 300; m.timeLeftMs = 100000; m.roundGapLeftMs = 2000;
    CHECK(!BuildHudScene(m, Me(), kAt).timeBar);                 // the Duel gap shows its own countdown
    m.roundGapLeftMs = 0;
    CHECK(BuildHudScene(m, Me(), kAt).timeBar);
}

static void TestPill() {
    MatchInfo m = Ffa(); m.phase = uint8_t(MatchPhase::Countdown); m.timeLeftMs = 3000;
    CHECK(BuildHudScene(m, Me(), kAt).pill.text == "MATCH STARTS IN");
    CHECK(BuildHudScene(Ffa(), Me(), kAt).pill.text == "FIRST TO 5");
    CHECK(BuildHudScene(Teams(), Me(), kAt).pill.text == "FIRST TO 10");
    HudScene s = BuildHudScene(Duel(), Me(), kAt);
    CHECK(s.pill.text == "ROUND 2" && Same(s.pill.color, kBone));
    MatchInfo d = Duel(); d.round = 5; d.scores = {{1, 2}, {2, 2}};
    s = BuildHudScene(d, Me(), kAt);
    CHECK(s.pill.text == "FINAL ROUND" && Same(s.pill.color, kWarn));   // the round decides the match
    d.scores = {{1, 2}, {2, 1}};
    CHECK(BuildHudScene(d, Me(), kAt).pill.text == "ROUND 5");          // match point is not the final round
    d = Duel(); d.scoreLimit = 1; d.round = 1; d.scores = {};
    CHECK(BuildHudScene(d, Me(), kAt).pill.text == "FINAL ROUND");      // one round to win: the first is final
    d = Duel(); d.roundGapLeftMs = 2400; d.roundWinner = 2;
    CHECK(BuildHudScene(d, Me(), kAt).pill.text == "NEXT ROUND IN 3");
    CHECK(BuildHudScene(d, Me(), kAt + 1000).pill.text == "NEXT ROUND IN 2");   // runs between updates
    m = Ffa(); m.phase = uint8_t(MatchPhase::SuddenDeath);
    s = BuildHudScene(m, Me(), kAt);
    CHECK(s.pill.text == "SUDDEN DEATH" && Same(s.pill.color, kBloodInk));
    s = BuildHudScene(Survival(), Me(), kAt);
    CHECK(s.pill.text == "WAVE 4 \xC2\xB7 2 LEFT");                     // bots standing in the wave
    MatchInfo b = Survival(); for (FighterEntry& f : b.fighters) if (f.team == kBotTeam) f.alive = false;
    CHECK(BuildHudScene(b, Me(), kAt).pill.text == "WAVE 4");
    // Results: the winner stays in the pill after the banner fades.
    m = Ffa(); m.phase = uint8_t(MatchPhase::Results); m.timeLeftMs = 9000; m.winner = 2;
    s = BuildHudScene(m, Me(1), kAt);
    CHECK(s.pill.text == "ANN WINS" && Same(s.pill.color, kBloodInk));
    m.winner = 1; s = BuildHudScene(m, Me(1), kAt);
    CHECK(s.pill.text == "YOU WIN" && Same(s.pill.color, kWarn));
    m = Teams(); m.phase = uint8_t(MatchPhase::Results); m.winner = 1; s = BuildHudScene(m, Me(1), kAt);
    CHECK(s.pill.text == "BLUE WINS" && Same(s.pill.color, TeamInk(1)));
    b = Survival(); b.phase = uint8_t(MatchPhase::Results); b.winner = kBotTeam; s = BuildHudScene(b, Me(1), kAt);
    CHECK(s.pill.text == "BOTS WIN" && Same(s.pill.color, kBloodInk));
    b.winner = kHumanTeam; s = BuildHudScene(b, Me(1), kAt);
    CHECK(s.pill.text == "PLAYERS WIN" && Same(s.pill.color, kOk));
    b.format = uint8_t(Format::Zombies); s = BuildHudScene(b, Me(1), kAt);
    CHECK(s.pill.text == "OVERRUN" && Same(s.pill.color, kBloodInk));
    m = Ffa(); m.phase = uint8_t(MatchPhase::Results); m.winner = Match::kNoWinner; s = BuildHudScene(m, Me(1), kAt);
    CHECK(s.pill.text == "DRAW" && Same(s.pill.color, kIron));          // ended with no winner
}

static void TestCountdownDigits() {
    MatchInfo m = Ffa(); m.phase = uint8_t(MatchPhase::Countdown); m.timeLeftMs = 3000;
    LocalHud l = Me(); l.banner = {"FIGHT", "ROUND 1", kBloodInk, kAt};   // the countdown takes the banner
    HudScene s = BuildHudScene(m, l, kAt);
    CHECK(s.banner.visible && s.banner.text == "3" && Near(s.banner.scale, 1.25) && Near(s.banner.opacity, 1));
    CHECK(Same(s.banner.color, kBone) && s.bannerSub.visible);
    m.timeLeftMs = 2900; s = BuildHudScene(m, Me(), kAt);
    CHECK(s.banner.text == "3" && Near(s.banner.scale, 1.15));        // each digit pops as it appears
    m.timeLeftMs = 2400; s = BuildHudScene(m, Me(), kAt);
    CHECK(s.banner.text == "3" && Near(s.banner.scale, 1.0));
    m.timeLeftMs = 2000; s = BuildHudScene(m, Me(), kAt);
    CHECK(s.banner.text == "2" && Near(s.banner.scale, 1.25));
    m.timeLeftMs = 3000;                                               // between updates
    s = BuildHudScene(m, Me(), kAt + 600); CHECK(s.banner.text == "3" && Near(s.banner.scale, 1.0));
    s = BuildHudScene(m, Me(), kAt + 1100); CHECK(s.banner.text == "2" && Near(s.banner.scale, 1.15));
    s = BuildHudScene(m, Me(), kAt + 5000); CHECK(!s.banner.visible);  // run out before Live arrives: no "0"
}

// Survival's loot break: the pill counts down to the next wave; held G fills the player's vote to skip it.
static void TestWaveBreakPill() {
    MatchInfo m = Survival(); m.roundGapLeftMs = 44100; m.roundWinner = 1;
    CHECK(BuildHudScene(m, Me(), kAt).pill.text == "NEXT WAVE IN 45");
    LocalHud l = Me(); l.giveUp = 0.5f;
    HudScene s = BuildHudScene(m, l, kAt);
    CHECK(s.hold.visible && s.hold.text == "SKIPPING THE BREAK 50%" && Same(s.hold.color, kOk) && s.hold.bar && Near(s.hold.frac, 0.5f));
    CHECK(s.pill.text == "NEXT WAVE IN 45");                                     // the pill keeps the countdown
    m.roundGapLeftMs = 0;                                                        // fighting: G gives the wave up
    s = BuildHudScene(m, l, kAt);
    CHECK(s.hold.visible && s.hold.text == "GIVING UP 50%" && Same(s.hold.color, kBloodInk) && s.hold.bar);
    m.format = uint8_t(Format::Zombies);                                         // Abyss too
    CHECK(BuildHudScene(m, l, kAt).hold.text == "GIVING UP 50%");
    m.format = uint8_t(Format::PlayersVsBots);
    LocalHud dead = l; dead.dead = true; dead.diedAtMs = kAt;                    // out of the wave: nothing to give up
    CHECK(!BuildHudScene(m, dead, kAt).hold.visible);
    LocalHud fight = l; fight.oneFight = true;                                   // a Progression fight keeps its wording
    CHECK(BuildHudScene(m, fight, kAt).hold.text == "GIVING UP THE FIGHT 50%");
    m.fighters[0].down = true;                                                   // downed in a wave: the pill says G works
    CHECK(BuildHudScene(m, Me(), kAt).pill.text == "DOWN - GET UP OR HOLD G");
}

// Hold G to give up: while held in a fought round the middle of the screen says so, with a bar for how far the hold
// is; the top pill keeps the match state (the pre1 test: the pill was covered and the hold could not be seen).
static void TestGiveUpPill() {
    LocalHud l = Me(); l.giveUp = 0.5f;
    HudScene s = BuildHudScene(Duel(), l, kAt);
    CHECK(s.hold.visible && s.hold.text == "GIVING UP 50%" && Same(s.hold.color, kBloodInk) && s.hold.bar && Near(s.hold.frac, 0.5f));
    CHECK(s.pill.text == "ROUND 2");
    l.giveUp = 0.999f; CHECK(BuildHudScene(Duel(), l, kAt).hold.text == "GIVING UP 99%");   // never shows 100 before it fires
    l.giveUp = 0.004f; CHECK(BuildHudScene(Duel(), l, kAt).hold.text == "GIVING UP 0%");
    CHECK(!BuildHudScene(Duel(), Me(), kAt).hold.visible);                                // not held
    MatchInfo d = Duel(); d.phase = uint8_t(MatchPhase::SuddenDeath); l.giveUp = 0.25f;
    CHECK(BuildHudScene(d, l, kAt).hold.text == "GIVING UP 25%");
    d = Duel(); d.roundGapLeftMs = 2400; d.roundWinner = 2;
    CHECK(!BuildHudScene(d, l, kAt).hold.visible);                                        // the gap: nothing to give up
    CHECK(BuildHudScene(d, l, kAt).pill.text == "NEXT ROUND IN 3");
    d = Duel(); d.phase = uint8_t(MatchPhase::Countdown); d.timeLeftMs = 3000;
    CHECK(!BuildHudScene(d, l, kAt).hold.visible);
    CHECK(BuildHudScene(Ffa(), l, kAt).hold.text == "GIVING UP 25%");                     // free-for-all and teams too
    CHECK(BuildHudScene(Ffa(), l, kAt).pill.text == "FIRST TO 5");
    LocalHud w = Me(); w.giveUpWait = true;                                               // held while out: said in the middle
    s = BuildHudScene(Ffa(), w, kAt);
    CHECK(s.hold.visible && !s.hold.bar && s.hold.text == "NOTHING TO GIVE UP - YOU ARE OUT");
}

// Item 9c: the host's draw check marks a fighter down; its own pill says so while the round is fought, the board marks
// the row, and holding G still shows the give-up hold over it.
static void TestDownPill() {
    MatchInfo d = Duel(); d.fighters[0].down = true;
    HudScene s = BuildHudScene(d, Me(), kAt);
    CHECK(s.pill.visible && s.pill.text == "DOWN - GET UP OR HOLD G" && Same(s.pill.color, kBloodInk));
    CHECK(BuildHudScene(d, Me(2), kAt).pill.text == "ROUND 2");                          // someone else is down
    LocalHud g = Me(); g.giveUp = 0.5f;
    CHECK(BuildHudScene(d, g, kAt).hold.text == "GIVING UP 50%");
    MatchInfo gap = d; gap.roundGapLeftMs = 2400; gap.roundWinner = 2;
    CHECK(BuildHudScene(gap, Me(), kAt).pill.text == "NEXT ROUND IN 3");
    MatchInfo sd = d; sd.phase = uint8_t(MatchPhase::SuddenDeath);
    CHECK(BuildHudScene(sd, Me(), kAt).pill.text == "DOWN - GET UP OR HOLD G");
    MatchInfo res = d; res.phase = uint8_t(MatchPhase::Results);
    CHECK(BuildHudScene(res, Me(), kAt).pill.text.rfind("DOWN", 0) != 0);
    MatchInfo dead = d; dead.fighters[0].alive = false;                                   // dead is not down
    CHECK(BuildHudScene(dead, Me(), kAt).pill.text.rfind("DOWN", 0) != 0);
    LocalHud gone = Me(); gone.dead = true; gone.diedAtMs = kAt;                          // ... nor dead here first
    CHECK(BuildHudScene(d, gone, kAt).pill.text.rfind("DOWN", 0) != 0);
    LocalHud tab = Me(); tab.tab = true;
    s = BuildHudScene(d, tab, kAt);
    bool sawDown = false, sawUp = false;
    for (const BoardRow& r : s.board.rows) {
        if (r.name == "Host") { sawDown = r.down && !r.dead; }
        if (r.name == "Ann") { sawUp = !r.down; }
    }
    CHECK(sawDown && sawUp);
    dead.fighters[0].down = true;
    s = BuildHudScene(dead, tab, kAt);
    for (const BoardRow& r : s.board.rows) if (r.name == "Host") CHECK(r.dead && !r.down);
}

// The countdown before a fight between players says how to give up, under the digits, in their look.
static void TestDuelCountdownHint() {
    MatchInfo d = Duel(); d.phase = uint8_t(MatchPhase::Countdown); d.timeLeftMs = 3000;
    HudScene s = BuildHudScene(d, Me(), kAt);
    CHECK(s.banner.text == "3" && s.bannerSub.visible && s.bannerSub.text == "HOLD G TO GIVE UP - YOUR LAST ATTACKER GETS THE KILL");
    CHECK(Same(s.bannerSub.color, kIron) && Near(s.bannerSub.opacity, s.banner.opacity) && Near(s.bannerSub.scale, 1));
    d.timeLeftMs = 0; CHECK(!BuildHudScene(d, Me(), kAt).bannerSub.visible);            // no digit, no hint
    MatchInfo f = Ffa(); f.phase = uint8_t(MatchPhase::Countdown); f.timeLeftMs = 3000;
    CHECK(BuildHudScene(f, Me(), kAt).bannerSub.visible);                                // free-for-all too
    f = Survival(); f.phase = uint8_t(MatchPhase::Countdown); f.timeLeftMs = 3000;
    CHECK(BuildHudScene(f, Me(), kAt).bannerSub.text == "STUCK? HOLD G TO GIVE UP");     // against bots: for getting unstuck
    LocalHud one = Me(); one.oneFight = true;
    CHECK(BuildHudScene(f, one, kAt).bannerSub.text == "HOLD G TO GIVE UP THE FIGHT");   // a Progression fight: unchanged
}

static void TestBannerFromShot() {
    LocalHud l = Me(); l.banner = {"FIGHT", "ROUND 2", kBloodInk, 2000};
    HudScene s = BuildHudScene(Duel(), l, 2000);
    CHECK(s.banner.visible && s.banner.text == "FIGHT" && Near(s.banner.scale, 1.25) && Near(s.banner.opacity, 1) && Same(s.banner.color, kBloodInk));
    CHECK(s.bannerSub.visible && s.bannerSub.text == "ROUND 2" && Near(s.bannerSub.scale, 1.25) && Near(s.bannerSub.opacity, 1));
    s = BuildHudScene(Duel(), l, 2250);
    CHECK(Near(s.banner.scale, 1.0) && Near(s.bannerSub.scale, 1.0));
    s = BuildHudScene(Duel(), l, 3950);
    CHECK(s.banner.visible && Near(s.banner.opacity, 0.5) && Near(s.bannerSub.opacity, 0.5));
    s = BuildHudScene(Duel(), l, 4150);
    CHECK(!s.banner.visible && !s.bannerSub.visible);
    s = BuildHudScene(Duel(), l, 1500);                                // fired "after" now (a clock step): fresh
    CHECK(s.banner.visible && Near(s.banner.scale, 1.25));
    l.banner.sub.clear();
    s = BuildHudScene(Duel(), l, 2000);
    CHECK(s.banner.visible && !s.bannerSub.visible);
    CHECK(!BuildHudScene(Duel(), Me(), 2000).banner.visible);          // nothing fired
}

static void TestBannerTracker() {
    BannerTracker t;
    MatchInfo m = Ffa(); m.phase = uint8_t(MatchPhase::Countdown);
    t.Observe(m, 1, 100);
    CHECK(t.shot().text.empty());                                      // the first look only records
    m.phase = uint8_t(MatchPhase::Live); t.Observe(m, 1, 200);
    CHECK(t.shot().text == "FIGHT" && t.shot().sub.empty() && t.shot().atMs == 200 && Same(t.shot().color, kBloodInk));
    t.Observe(m, 1, 300);
    CHECK(t.shot().atMs == 200);                                       // nothing changed: no new banner
    m.phase = uint8_t(MatchPhase::SuddenDeath); t.Observe(m, 1, 400);
    CHECK(t.shot().text == "SUDDEN DEATH" && t.shot().sub == "NEXT KILL WINS" && Same(t.shot().color, kBloodInk));
    m.phase = uint8_t(MatchPhase::Results); m.winner = 2; t.Observe(m, 1, 500);
    CHECK(t.shot().text == "ANN WINS" && Same(t.shot().color, kBloodInk) && t.shot().atMs == 500);
    m.phase = uint8_t(MatchPhase::Countdown); m.winner = Match::kNoWinner; t.Observe(m, 1, 600);
    CHECK(t.shot().atMs == 500);                                       // the rematch countdown fires nothing
    m.phase = uint8_t(MatchPhase::Live); t.Observe(m, 1, 700);
    CHECK(t.shot().text == "FIGHT" && t.shot().atMs == 700);
    m.phase = uint8_t(MatchPhase::Results); m.winner = 1; t.Observe(m, 1, 800);
    CHECK(t.shot().text == "YOU WIN" && Same(t.shot().color, kWarn));

    MatchInfo tm = Teams(); BannerTracker tt; tt.Observe(tm, 1, 100);
    tm.phase = uint8_t(MatchPhase::SuddenDeath); tt.Observe(tm, 1, 200);
    CHECK(tt.shot().sub == "NEXT POINT WINS");
    tm.phase = uint8_t(MatchPhase::Results); tm.winner = 1; tt.Observe(tm, 1, 300);
    CHECK(tt.shot().text == "BLUE WINS" && Same(tt.shot().color, TeamInk(1)));

    // Duel: a round ends, the gap runs, the next round starts.
    BannerTracker d; MatchInfo du = Duel(); du.phase = uint8_t(MatchPhase::Countdown); du.round = 1; du.scores = {};
    d.Observe(du, 1, 100);
    du.phase = uint8_t(MatchPhase::Live); d.Observe(du, 1, 200);
    CHECK(d.shot().text == "FIGHT" && d.shot().sub == "ROUND 1");
    du.roundGapLeftMs = 3500; du.roundWinner = 2; du.scores = {{2, 1}}; d.Observe(du, 1, 300);
    CHECK(d.shot().text == "ROUND TO ANN" && d.shot().sub == "ROUND 1" && Same(d.shot().color, kBloodInk) && d.shot().atMs == 300);
    du.roundGapLeftMs = 2500; d.Observe(du, 1, 400);
    CHECK(d.shot().atMs == 300);                                       // the same gap
    du.roundGapLeftMs = 0; du.roundWinner = 0; du.round = 2; d.Observe(du, 1, 500);
    CHECK(d.shot().text == "FIGHT" && d.shot().sub == "ROUND 2" && d.shot().atMs == 500);
    d.Observe(du, 1, 550);
    CHECK(d.shot().atMs == 500);                                       // the same round
    du.roundGapLeftMs = 4000; du.roundWinner = 1; du.scores = {{1, 2}, {2, 2}}; d.Observe(du, 1, 600);
    CHECK(d.shot().text == "ROUND TO YOU" && d.shot().sub == "ROUND 2" && Same(d.shot().color, kOk));
    du.roundGapLeftMs = 0; du.roundWinner = 0; du.round = 3; d.Observe(du, 1, 700);
    CHECK(d.shot().text == "FIGHT" && d.shot().sub == "FINAL ROUND");
    du.roundGapLeftMs = 4000; du.roundWinner = 0; du.round = 3; d.Observe(du, 1, 800);
    CHECK(d.shot().text == "ROUND DRAWN" && Same(d.shot().color, kIron));
    du.roundGapLeftMs = 0; du.round = 4; du.phase = uint8_t(MatchPhase::SuddenDeath); d.Observe(du, 1, 900);
    CHECK(d.shot().text == "SUDDEN DEATH" && d.shot().sub == "NEXT ROUND WINS");
    du.phase = uint8_t(MatchPhase::Live); d.Observe(du, 1, 950);
    CHECK(d.shot().text == "SUDDEN DEATH");                            // only the countdown's end is FIGHT
    du.round = 5; d.Observe(du, 1, 1000);                              // a missed gap: the new round still fires
    CHECK(d.shot().text == "FIGHT" && d.shot().atMs == 1000);

    // Bot formats: a cleared wave.
    BannerTracker b; MatchInfo sv = Survival(); b.Observe(sv, 1, 100);
    sv.scores = {{kHumanTeam, 4}}; b.Observe(sv, 1, 200);
    CHECK(b.shot().text == "WAVE CLEARED" && b.shot().sub == "4 CLEARED" && Same(b.shot().color, kOk) && b.shot().atMs == 200);
    b.Observe(sv, 1, 300); CHECK(b.shot().atMs == 200);

    // Fired by the session (a regear), newest wins either way.
    b.Fire("NEW GEAR", "TIER 3", kWarn, 400);
    CHECK(b.shot().text == "NEW GEAR" && b.shot().sub == "TIER 3" && b.shot().atMs == 400);
    sv.scores = {{kHumanTeam, 5}}; b.Observe(sv, 1, 450);
    CHECK(b.shot().text == "WAVE CLEARED" && b.shot().atMs == 450);
    b.Fire("NEW GEAR", "TIER 3", kWarn, 460);
    CHECK(b.shot().text == "NEW GEAR");

    // A new match config (another format) and a reset only record.
    BannerTracker r; MatchInfo a = Ffa(); a.phase = uint8_t(MatchPhase::Countdown); r.Observe(a, 1, 100);
    MatchInfo z = Teams(); r.Observe(z, 1, 200);
    CHECK(r.shot().text.empty());
    r.Reset(); a.phase = uint8_t(MatchPhase::Live); r.Observe(a, 1, 300);
    CHECK(r.shot().text.empty());
    a.phase = uint8_t(MatchPhase::Idle); r.Observe(a, 1, 400);
    a.phase = uint8_t(MatchPhase::Countdown); r.Observe(a, 1, 500);
    CHECK(r.shot().text.empty());
    r.Reset(); a.phase = uint8_t(MatchPhase::Live); r.Observe(a, 1, 600);   // after a reset the countdown's end is not seen
    CHECK(r.shot().text.empty());
    r.Fire("NEW GEAR", "", kWarn, 700); r.Reset();
    CHECK(r.shot().text.empty());
    for (uint8_t p : {uint8_t(MatchPhase::SuddenDeath), uint8_t(MatchPhase::Results)}) {   // joining mid-match
        BannerTracker j; MatchInfo jm = Ffa(); jm.phase = p; jm.winner = 2;
        j.Observe(jm, 1, 100);
        CHECK(j.shot().text.empty());
    }
}

static void TestDeadPanel() {
    LocalHud l = Me(); l.dead = true; l.diedAtMs = 1000;
    HudScene s = BuildHudScene(Ffa(), l, 2000);
    CHECK(s.dead.visible && s.dead.title == "RESPAWNING" && s.dead.detail == "in 4" && s.dead.bar && Near(s.dead.barFrac, 0.8));
    CHECK(Has(s.dead.hint, "watch another fighter"));
    s = BuildHudScene(Ffa(), l, 6000);
    CHECK(s.dead.visible && s.dead.title == "RESPAWNING" && s.dead.detail.empty() && !s.dead.bar);
    s = BuildHudScene(Ffa(), l, 500);                                  // died "after" now (a clock step): the full delay
    CHECK(s.dead.detail == "in 5" && Near(s.dead.barFrac, 1));
    MatchInfo m = Ffa(); m.respawnDelayS = 0;
    s = BuildHudScene(m, l, 1000);
    CHECK(s.dead.title == "RESPAWNING" && !s.dead.bar);
    m = Ffa(); for (FighterEntry& f : m.fighters) if (f.entity != 1) f.alive = false;
    CHECK(BuildHudScene(m, l, 2000).dead.hint.empty());               // nobody else standing to watch
    {   // the free camera key: offered while watching someone, "follow <name>" while flying, silent when the host turned it off
        LocalHud w = l; w.watching = 2;
        std::string name = NameOf(Ffa(), 2);
        CHECK(Has(BuildHudScene(Ffa(), w, 2000).dead.hint, "watch another fighter") && Has(BuildHudScene(Ffa(), w, 2000).dead.hint, "F: free camera"));
        w.flying = true;
        CHECK(BuildHudScene(Ffa(), w, 2000).dead.hint == "F: follow " + name);
        w.watching = 0; CHECK(BuildHudScene(Ffa(), w, 2000).dead.hint == "F: follow");
        MatchInfo off = Ffa(); off.freeCam = 0; w.watching = 2;
        CHECK(!Has(BuildHudScene(off, w, 2000).dead.hint, "F:") && Has(BuildHudScene(off, w, 2000).dead.hint, "watch another fighter"));
        w.flying = false; CHECK(!Has(BuildHudScene(off, w, 2000).dead.hint, "F:"));
        w.flying = true; CHECK(!Has(BuildHudScene(off, w, 2000).dead.hint, "F: follow"));
        LocalHud nobody = l; nobody.watching = 0; CHECK(!Has(BuildHudScene(Ffa(), nobody, 2000).dead.hint, "F: free"));   // nothing watched: no camera to free
    }

    // Duel: never "respawning"; everybody comes back together after the gap.
    MatchInfo d = Duel(); d.roundGapLeftMs = 2400; d.roundWinner = 2;
    s = BuildHudScene(d, l, kAt);
    CHECK(s.dead.visible && s.dead.title == "ROUND OVER" && s.dead.detail == "next round in 3" && s.dead.bar && Near(s.dead.barFrac, 0.6));
    s = BuildHudScene(d, l, kAt + 1000);
    CHECK(s.dead.detail == "next round in 2");
    d.roundGapLeftMs = 0;
    s = BuildHudScene(d, l, kAt);
    CHECK(s.dead.title == "ROUND OVER" && s.dead.detail.empty() && !s.dead.bar);
    for (uint32_t gap : {0u, 1u, 999u, 4000u, 60000u})
        for (uint32_t since : {0u, 3000u, 4000u, 100000u}) {
            MatchInfo e = Duel(); e.roundGapLeftMs = gap;
            for (uint8_t p : {uint8_t(MatchPhase::Live), uint8_t(MatchPhase::SuddenDeath)}) {
                e.phase = p;
                LocalHud dl = Me(); dl.dead = true; dl.diedAtMs = kAt;
                HudScene t = BuildHudScene(e, dl, kAt + since);
                std::string all = t.dead.title + t.dead.detail + t.dead.hint;
                CHECK(!Has(all, "RESPAWN") && !Has(all, "espawn"));
            }
        }

    s = BuildHudScene(Survival(), l, 2000);
    CHECK(s.dead.visible && s.dead.title == "DOWN" && s.dead.detail == "back next wave" && !s.dead.bar);

    CHECK(!BuildHudScene(Ffa(), Me(), 2000).dead.visible);            // alive
    for (uint8_t p : {uint8_t(MatchPhase::Countdown), uint8_t(MatchPhase::Results)}) {
        m = Ffa(); m.phase = p;
        CHECK(!BuildHudScene(m, l, 2000).dead.visible);
    }
}

// A Progression fight (Free Mode off) is one fight against the event's opponents: the host plays it as players vs
// bots with no waves, the downed stay down until it ends, and its results go back to the Tavern (LocalHud.oneFight).
static void TestOneFight() {
    LocalHud one = Me(); one.oneFight = true;
    HudScene s = BuildHudScene(Survival(), one, kAt);
    CHECK(s.pill.text == "FIGHT \xC2\xB7 2 LEFT");                      // opponents standing, no wave number
    MatchInfo b = Survival(); for (FighterEntry& f : b.fighters) if (f.team == kBotTeam) f.alive = false;
    CHECK(BuildHudScene(b, one, kAt).pill.text == "FIGHT");
    b = Survival();                                                    // an opponent that gave up is out, alive or not
    for (FighterEntry& f : b.fighters) if (f.team == kBotTeam && f.alive) { f.down = true; break; }
    CHECK(BuildHudScene(b, one, kAt).pill.text == "FIGHT \xC2\xB7 1 LEFT");
    b = Survival(); b.phase = uint8_t(MatchPhase::SuddenDeath);
    CHECK(BuildHudScene(b, one, kAt).pill.text == "SUDDEN DEATH");
    LocalHud down = one; down.dead = true; down.diedAtMs = kAt;
    s = BuildHudScene(Survival(), down, kAt + 1000);
    CHECK(s.dead.visible && s.dead.title == "DOWN" && s.dead.detail == "out until the fight ends" && !s.dead.bar);
    CHECK(Has(s.dead.hint, "watch another fighter"));
    LocalHud tab = one; tab.tab = true; tab.map = "Arena";
    s = BuildHudScene(Survival(), tab, kAt);
    CHECK(s.board.title == "PROGRESSION FIGHT \xC2\xB7 ARENA" && s.board.rows.size() == 2);
    tab.map.clear();
    CHECK(BuildHudScene(Survival(), tab, kAt).board.title == "PROGRESSION FIGHT");
    b = Survival(); b.phase = uint8_t(MatchPhase::Results); b.winner = kHumanTeam; b.timeLeftMs = 4200;
    s = BuildHudScene(b, one, kAt);
    // won: the fight waits for G, as the stock game does (the report); a loss goes back by itself, as before
    CHECK(s.board.footer == "FIGHT WON \xC2\xB7 HOLD G TO GO ON \xC2\xB7 OR IN 5" && s.pill.text == "PLAYERS WIN");
    CHECK(s.hold.visible && s.hold.text == "FIGHT WON - HOLD G TO GO ON (OR IN 5)" && !s.hold.bar);
    b.winner = kBotTeam;
    s = BuildHudScene(b, one, kAt);
    CHECK(s.board.footer == "BACK TO THE TAVERN IN 5" && !s.hold.visible);

    // Elsewhere it changes nothing: Survival keeps its waves, the other formats their rules.
    s = BuildHudScene(Survival(), Me(), kAt);
    CHECK(s.pill.text == "WAVE 4 \xC2\xB7 2 LEFT");
    LocalHud plainTab = Me(); plainTab.tab = true; plainTab.map = "Arena";
    CHECK(BuildHudScene(Survival(), plainTab, kAt).board.title == "SURVIVAL \xC2\xB7 WAVE 4 \xC2\xB7 ARENA");
    CHECK(BuildHudScene(b, Me(), kAt).board.footer == "REMATCH IN 5");
    CHECK(BuildHudScene(Ffa(), one, kAt).pill.text == "FIRST TO 5");
    s = BuildHudScene(Ffa(), down, kAt + 1000);
    CHECK(s.dead.title == "RESPAWNING" && s.dead.detail == "in 4");
    s = BuildHudScene(Duel(), tab, kAt);
    CHECK(Has(s.board.title, "DUEL") && Has(s.board.title, "BEST OF 5"));
    MatchInfo r = Ffa(); r.phase = uint8_t(MatchPhase::Results); r.timeLeftMs = 4200; r.winner = 1;
    CHECK(BuildHudScene(r, one, kAt).board.footer == "REMATCH IN 5");

    // The bot formats' sudden death is decided by the wave (Match: the next wave cleared wins).
    BannerTracker bt; MatchInfo sv = Survival(); bt.Observe(sv, 1, 100);
    sv.phase = uint8_t(MatchPhase::SuddenDeath); bt.Observe(sv, 1, 200);
    CHECK(bt.shot().text == "SUDDEN DEATH" && bt.shot().sub == "CLEAR THE WAVE TO WIN");
    BannerTracker zt; MatchInfo zv = Survival(); zv.format = uint8_t(Format::Zombies); zt.Observe(zv, 1, 100);
    zv.phase = uint8_t(MatchPhase::SuddenDeath); zt.Observe(zv, 1, 200);
    CHECK(zt.shot().sub == "CLEAR THE WAVE TO WIN");
}

static void TestFeedRows() {
    MatchInfo t = Teams();
    FeedRow r = MakeFeedRow(t, {2, 1, 0}, 1);
    CHECK(r.spans.size() == 3 && r.mine);
    if (r.spans.size() == 3) {
        CHECK(r.spans[0].text == "Ann" && Same(r.spans[0].color, TeamInk(1)));
        CHECK(r.spans[1].text == " killed " && Same(r.spans[1].color, kIron));
        CHECK(r.spans[2].text == "Host" && Same(r.spans[2].color, TeamInk(0)));
    }
    CHECK(!MakeFeedRow(t, {2, 3, 0}, 1).mine && MakeFeedRow(t, {1, 3, 0}, 1).mine);
    r = MakeFeedRow(t, {4, 2, 1}, 1);
    CHECK(r.spans.size() == 3 && r.spans[1].text == " killed teammate ");
    r = MakeFeedRow(t, {3, 3, 2}, 1);
    CHECK(r.spans.size() == 2 && r.spans[0].text == "Bo" && Same(r.spans[0].color, TeamInk(0)) && r.spans[1].text == " killed themselves" && Same(r.spans[1].color, kIron));
    r = MakeFeedRow(t, {2, 3, 4}, 1);
    CHECK(r.spans.size() == 2 && r.spans[1].text == " died");
    r = MakeFeedRow(t, {0, 3, 8}, 3);                                    // held G (Duel)
    CHECK(r.mine && r.spans.size() == 2 && r.spans[0].text == "Bo" && r.spans[1].text == " gave up" && Same(r.spans[1].color, kIron));
    r = MakeFeedRow(t, {0, 4, 0}, 1);
    CHECK(r.spans.size() == 2 && r.spans[0].text == "Cy" && r.spans[1].text == " died");
    r = MakeFeedRow(t, {3, 2, 16}, 3);                                   // a drawn Duel round: both names in their inks
    CHECK(r.mine && r.spans.size() == 4 && r.spans[0].text == "Ann" && Same(r.spans[0].color, TeamInk(1)) && r.spans[1].text == " and ");
    CHECK(r.spans.size() == 4 && r.spans[2].text == "Bo" && Same(r.spans[2].color, TeamInk(0)) && Same(r.spans[3].color, kIron));
    r = MakeFeedRow(t, {0, 2, 32}, 1);
    CHECK(!r.mine && r.spans.size() == 2 && r.spans[1].text == " drew the round (no hits)" && Same(r.spans[1].color, kIron));
    for (Kill k : {Kill{2, 1, 0}, Kill{4, 2, 1}, Kill{3, 3, 2}, Kill{2, 3, 4}, Kill{0, 4, 0}, Kill{9, 8, 0}, Kill{3, 2, 16}, Kill{3, 2, 32 | 8}, Kill{0, 2, 16}})
        CHECK(Joined(MakeFeedRow(t, k, 1)) == KillLine(t, k));       // the same words as the text HUD

    r = MakeFeedRow(Ffa(), {2, 3, 0}, 1);                              // no teams: bone names
    CHECK(r.spans.size() == 3 && Same(r.spans[0].color, kBone) && Same(r.spans[2].color, kBone));
    r = MakeFeedRow(Survival(), {10, 2, 0}, 1);                        // bots in blood, humans in bone
    CHECK(r.spans.size() == 3 && Same(r.spans[0].color, kBloodInk) && Same(r.spans[2].color, kBone));
    CHECK(Same(FighterInk(Survival(), 1), kBone) && Same(FighterInk(Survival(), 11), kBloodInk));
    CHECK(Same(FighterInk(Teams(), 2), TeamInk(1)) && Same(FighterInk(Teams(), 99), kBone));
    MatchInfo three = Teams(); three.teams = 3; three.fighters.push_back({5, 2, 0, 0, "Ed"});
    CHECK(Same(FighterInk(three, 5), TeamInk(2)) && Same(FighterInk(three, 1), TeamInk(0)));
    CHECK(Same(FighterInk(Duel(), 2), kBone) && Same(FighterInk(Duel(), 1), kBone));
    for (Format f : {Format::TeamDuel, Format::Riot, Format::Juggernaut}) {   // the team rounds ink their names by team too (the legacy ThreeTeams value is mapped to Teams before it is sent)
        MatchInfo r = Teams(); r.format = uint8_t(f); r.teams = 4; r.fighters.push_back({5, 3, 0, 0, "Ed"});
        CHECK(Same(FighterInk(r, 5), TeamInk(3)) && Same(FighterInk(r, 2), TeamInk(1)) && Same(FighterInk(r, 1), TeamInk(0)) && Same(FighterInk(r, 99), kBone));
    }
    MatchInfo stale = Ffa(); stale.fighters[1].team = 1;              // free-for-all ignores a team left from before
    CHECK(Same(FighterInk(stale, 2), kBone));
}

static void TestFeedView() {
    MatchInfo t = Teams();
    LocalHud l = Me(1);
    const uint32_t now = 20000;
    FeedRow other = MakeFeedRow(t, {2, 3, 0}, 1), mine = MakeFeedRow(t, {2, 1, 0}, 1);
    l.feed = {{now - 7000, other}, {now - 5500, mine}, {now - 3000, other}, {now - 2000, mine}, {now - 1000, other}, {now, other}};
    HudScene s = BuildHudScene(t, l, now);
    CHECK(s.feed.size() == 4);                                         // the newest four still showing, oldest first
    if (s.feed.size() == 4) {
        CHECK(Near(s.feed[0].opacity, kFeedOthersOpacity) && !s.feed[0].row.mine);
        CHECK(Near(s.feed[1].opacity, 1) && s.feed[1].row.mine);       // rows naming you are brighter
        CHECK(Near(s.feed[2].opacity, kFeedOthersOpacity) && Near(s.feed[3].opacity, kFeedOthersOpacity));
    }
    CHECK(kFeedOthersOpacity < 1 && kFeedOthersOpacity >= 0.6f);
    l.feed = {{now - 7000, other}, {now - 5500, mine}, {now - 5500, other}};
    s = BuildHudScene(t, l, now);
    CHECK(s.feed.size() == 2);
    if (s.feed.size() == 2) {
        CHECK(Near(s.feed[0].opacity, 0.5) && Joined(s.feed[0].row) == Joined(mine));
        CHECK(Near(s.feed[1].opacity, 0.5 * kFeedOthersOpacity));
    }
    l.feed = {{now + 500, other}};                                     // stamped "after" now: fresh, not 49 days old
    s = BuildHudScene(t, l, now);
    CHECK(s.feed.size() == 1 && Near(s.feed[0].opacity, kFeedOthersOpacity));
}

static void TestBoard() {
    LocalHud l = Me(1); l.map = "Yard";
    CHECK(!BuildHudScene(Ffa(), l, kAt).board.visible);               // Tab not held
    l.tab = true;
    HudScene s = BuildHudScene(Ffa(), l, kAt);
    CHECK(s.board.visible && Has(s.board.title, "FREE-FOR-ALL") && Has(s.board.title, "FIRST TO 5") && Has(s.board.title, "YARD"));
    CHECK(s.board.footer.empty());
    { MatchInfo br = Ffa(); br.gear = uint8_t(Gear::Fists);           // Brawl: the same board under its own name
      HudScene bs = BuildHudScene(br, l, kAt);
      CHECK(Has(bs.board.title, "BRAWL") && !Has(bs.board.title, "FREE-FOR-ALL") && Has(bs.board.title, "FIRST TO 5") && bs.board.rows.size() == 3); }
    CHECK(s.board.rows.size() == 3);
    if (s.board.rows.size() == 3) {
        CHECK(s.board.rows[0].name == "Ann" && s.board.rows[0].score == 4 && s.board.rows[0].pingMs == 80 && !s.board.rows[0].local);
        CHECK(s.board.rows[1].name == "Host" && s.board.rows[1].host && s.board.rows[1].local && s.board.rows[1].kills == 2 && s.board.rows[1].deaths == 1);
        CHECK(s.board.rows[2].name == "Bo" && s.board.rows[2].dead && Same(s.board.rows[2].ink, kIron) && !s.board.rows[2].host);
        CHECK(Same(s.board.rows[0].ink, kBone) && s.board.rows[0].kind == BoardRow::Kind::Fighter);
    }
    MatchInfo m = Ffa(); m.fighters.push_back({4, -1, 5, 0, "Di", 40, true}); m.scores.push_back({4, 2});
    s = BuildHudScene(m, l, kAt);                                      // by score, then kills
    CHECK(s.board.rows.size() == 4 && s.board.rows[0].name == "Ann" && s.board.rows[1].name == "Di" && s.board.rows[2].name == "Host" && s.board.rows[3].name == "Bo");
    m = Ffa(); m.fighters.push_back({7, -1, 0, 0, "", 0, true});
    s = BuildHudScene(m, l, kAt);
    CHECK(s.board.rows.size() == 4 && s.board.rows[3].name == "#7");

    s = BuildHudScene(Duel(), l, kAt);
    CHECK(Has(s.board.title, "DUEL") && Has(s.board.title, "BEST OF 5"));
    s = BuildHudScene(Survival(), l, kAt);                             // the players only, no bot rows
    CHECK(Has(s.board.title, "WAVE 4") && s.board.rows.size() == 2);
    for (const BoardRow& r : s.board.rows) CHECK(r.kind == BoardRow::Kind::Fighter && r.name != "Bot");

    s = BuildHudScene(Teams(), l, kAt);                                // grouped under a band per team
    CHECK(s.board.rows.size() == 6);
    if (s.board.rows.size() == 6) {
        const auto& r = s.board.rows;
        CHECK(r[0].kind == BoardRow::Kind::Band && r[0].name == "RED" && r[0].score == 3 && Same(r[0].swatch, TeamColor(0)) && Same(r[0].ink, TeamInk(0)));
        CHECK(r[1].name == "Host" && r[2].name == "Bo" && Same(r[1].swatch, TeamColor(0)));
        CHECK(r[3].kind == BoardRow::Kind::Band && r[3].name == "BLUE" && r[3].score == 5);
        CHECK(r[4].name == "Ann" && r[5].name == "Cy" && r[4].score == 5 && Same(r[5].swatch, TeamColor(1)));
    }
    m = Teams(); m.fighters.push_back({6, -1, 9, 0, "Fay", 30, true});   // not on a team yet: after the teams
    s = BuildHudScene(m, l, kAt);
    CHECK(s.board.rows.size() == 7 && s.board.rows[6].name == "Fay" && s.board.rows[6].kind == BoardRow::Kind::Fighter);
    LocalHud other = l; other.me = 2; other.host = 2;                  // the badges follow the entities given
    s = BuildHudScene(Ffa(), other, kAt);
    CHECK(s.board.rows.size() == 3 && s.board.rows[0].host && s.board.rows[0].local && !s.board.rows[1].host && !s.board.rows[1].local);
    other.host = 0;                                                    // a host with no fighter (hosted server): nobody carries the badge
    s = BuildHudScene(Ffa(), other, kAt);
    CHECK(s.board.rows.size() == 3 && !s.board.rows[0].host && !s.board.rows[1].host && !s.board.rows[2].host);
    m = Teams(); m.phase = uint8_t(MatchPhase::Results); m.winner = 1; m.timeLeftMs = 9100;
    s = BuildHudScene(m, Me(1), kAt);                                  // results: shown without Tab
    CHECK(s.board.visible && s.board.footer == "REMATCH IN 10");
    s = BuildHudScene(m, Me(1), kAt + 4000);
    CHECK(s.board.footer == "REMATCH IN 6");
}

// Four teams and Last Man Standing in the HUD: the strip and the board carry every team; the round format's pill, banners and
// board read like Duel's with more fighters.
static void TestFourTeamsAndLms() {
    MatchInfo four = Teams(); four.teams = 4; four.scores = {{0, 1}, {3, 6}};
    four.fighters = {{1, 0, 3, 1, "Host"}, {2, 1, 5, 0, "Ann"}, {3, 2, 0, 4, "Bo"}, {4, 3, 2, 2, "Cy"}, {5, 3, 1, 0, "Di"}};
    LocalHud tab = Me(); tab.tab = true;
    HudScene s = BuildHudScene(four, tab, kAt);
    CHECK(s.board.title == "TEAMS (4) \xC2\xB7 FIRST TO 10");
    int bands = 0;
    for (const BoardRow& r : s.board.rows) if (r.kind == BoardRow::Kind::Band) { ++bands; if (r.name == "GOLD") CHECK(r.score == 6 && Same(r.swatch, TeamColor(3))); }
    CHECK(bands == 4 && s.board.rows.size() == 4 + 5);
    CHECK(Same(FighterInk(four, 4), TeamInk(3)));
    MatchInfo three = four; three.teams = 3;                                        // a fourth-team fighter left outside the teams comes last
    s = BuildHudScene(three, tab, kAt);
    CHECK(s.board.rows.size() == 3 + 5 && (s.board.rows.back().name == "Di" || s.board.rows.back().name == "Cy"));

    MatchInfo l; l.format = uint8_t(Format::LastManStanding); l.scoreLimit = 3; l.phase = uint8_t(MatchPhase::Live); l.round = 2; l.respawnDelayS = 4;
    l.scores = {{1, 1}, {2, 2}, {3, 0}};
    l.fighters = {{1, -1, 1, 0, "Host", 0, true}, {2, -1, 2, 1, "Ann", 60, true}, {3, -1, 0, 2, "Bo", 60, false}};
    s = BuildHudScene(l, Me(), kAt);
    CHECK(s.pill.text == "ROUND 2" && s.strip.size() == 2 && s.strip[1].label == "ANN" && s.strip[1].score == 2);   // you against the top rival
    l.scores = {{1, 2}, {2, 2}, {3, 0}};
    CHECK(BuildHudScene(l, Me(), kAt).pill.text == "FINAL ROUND");
    l.roundGapLeftMs = 2400; l.roundWinner = 2;
    CHECK(BuildHudScene(l, Me(), kAt).pill.text == "NEXT ROUND IN 3");
    LocalHud dead = Me(); dead.dead = true; dead.diedAtMs = kAt;
    s = BuildHudScene(l, dead, kAt);
    CHECK(s.dead.title == "ROUND OVER" && s.dead.detail == "next round in 3");
    l.roundGapLeftMs = 0; l.round = 2; l.phase = uint8_t(MatchPhase::Live);
    s = BuildHudScene(l, tab, kAt);
    CHECK(Has(s.board.title, "LAST MAN STANDING") && Has(s.board.title, "FIRST TO 3") && !Has(s.board.title, "BEST OF"));

    BannerTracker t; MatchInfo c = l; c.phase = uint8_t(MatchPhase::Countdown); c.round = 1; c.scores = {}; c.roundGapLeftMs = 0; c.roundWinner = 0;
    t.Observe(c, 1, 100);
    c.phase = uint8_t(MatchPhase::Live); t.Observe(c, 1, 200);
    CHECK(t.shot().text == "FIGHT" && t.shot().sub == "ROUND 1");
    c.roundGapLeftMs = 3500; c.roundWinner = 3; c.scores = {{3, 1}}; t.Observe(c, 1, 300);
    CHECK(t.shot().text == "ROUND TO BO" && t.shot().sub == "ROUND 1");
    c.roundGapLeftMs = 0; c.roundWinner = 0; c.round = 2; t.Observe(c, 1, 400);
    CHECK(t.shot().text == "FIGHT" && t.shot().sub == "ROUND 2");
    c.phase = uint8_t(MatchPhase::SuddenDeath); t.Observe(c, 1, 500);
    CHECK(t.shot().text == "SUDDEN DEATH" && t.shot().sub == "NEXT ROUND WINS");
    MatchInfo r = l; r.phase = uint8_t(MatchPhase::Results); r.winner = 2; r.timeLeftMs = 4200;
    CHECK(BuildHudScene(r, Me(), kAt).pill.text == "ANN WINS");
    r.winner = 1; CHECK(BuildHudScene(r, Me(), kAt).pill.text == "YOU WIN");
}

int main() {
    TestBannerLook();
    TestFeedOpacity();
    TestIdleShowsNothing();
    TestStrip();
    TestTimeBar();
    TestPill();
    TestOneFight();
    TestCountdownDigits();
    TestBannerFromShot();
    TestGiveUpPill(); TestWaveBreakPill(); TestDownPill();
    TestDuelCountdownHint();
    TestBannerTracker();
    TestDeadPanel();
    TestFeedRows();
    TestFeedView();
    TestBoard();
    TestFourTeamsAndLms();
    if (g_fail) { std::printf("%d hud scene checks failed\n", g_fail); return 1; }
    std::printf("all hud scene tests passed\n");
    return 0;
}
