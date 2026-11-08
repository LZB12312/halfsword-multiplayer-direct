// Lobby page (MATCH_ROUNDS_UI_CHAT_PLAN 3.4): the state pill and badge, the browser rows, the players, the mode cards,
// which setting rows each format shows with their values and hints, host against member, the steppers wrapping both
// ways, the members' Ready toggle, and the map cards with the selected and the refused ones.
#include "match/hsm_lobby_scene.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using namespace hsm::style;

static bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }
static bool Same(Rgba a, Rgba b) { return Near(a.r, b.r) && Near(a.g, b.g) && Near(a.b, b.b) && Near(a.a, b.a); }

static const std::string kDot = "\xC2\xB7";

static LobbyPlayer Player(const char* name, bool host = false, bool me = false, int team = kNoTeam, int over = kNoTeam) {
    LobbyPlayer p; p.name = name; p.host = host; p.me = me; p.team = team; p.hostOverride = over;
    return p;
}
static size_t PickableCount() { size_t n = 0; for (size_t i = 0; i < MapCount(); ++i) if (kMaps[i].pick) ++n; return n; }

static LobbyLocal InLobby(bool host, Format f = Format::FreeForAll, Gear g = Gear::Own) {
    LobbyLocal l;
    l.phase = LobbyPhase::InLobby; l.host = host; l.version = "0.2.0"; l.myName = host ? "Ann" : "Bob";
    l.config.format = f; l.config.gear = g; l.config.scoreLimit = DefaultScoreLimit(f);
    l.players = {Player("Ann", true, host), Player("Bob", false, !host)};
    return l;
}
static LobbyLocal Teamed(bool host, int teams, Gear g = Gear::Own) { LobbyLocal l = InLobby(host, Format::TwoTeams, g); l.config.teams = uint8_t(teams); return l; }
static std::vector<LobbyRow> Ids(const LobbyScene& s) { std::vector<LobbyRow> v; for (const SettingRow& r : s.settings) v.push_back(r.id); return v; }
static const SettingRow* RowOf(const LobbyScene& s, LobbyRow id) { for (const SettingRow& r : s.settings) if (r.id == id) return &r; return nullptr; }
static const SettingRow& Need(const LobbyScene& s, LobbyRow id) {
    static const SettingRow kNone{};
    const SettingRow* r = RowOf(s, id);
    CHECK(r != nullptr);
    return r ? *r : kNone;
}

static void TestPrevNext() {
    const std::vector<int> o = {3, 5, 10};
    CHECK(NextOf(o, 3) == 5 && NextOf(o, 10) == 3 && NextOf(o, 7) == 3);
    CHECK(PrevOf(o, 5) == 3 && PrevOf(o, 10) == 5);
    CHECK(PrevOf(o, 3) == 10);                           // wraps back to the last
    CHECK(PrevOf(o, 7) == 10);                           // unknown: the last (NextOf: the first)
    CHECK(PrevOf({}, 7) == 7 && NextOf({}, 7) == 7);
    CHECK(StepOf(o, 5, 1) == 10 && StepOf(o, 5, -1) == 3 && StepOf(o, 5, 0) == 10);
}

static void TestNewRows() {
    MatchConfig b; b.format = Format::PlayersVsBots;
    CHECK(WaveBreakShown(b) == 60 && StepSetting(b, LobbyRow::WaveBreak, 1).waveBreakS == 120 && StepSetting(b, LobbyRow::WaveBreak, -1).waveBreakS == 30);
    MatchConfig z; z.format = Format::Zombies;
    CHECK(WaveBreakShown(z) == 10 && StepSetting(z, LobbyRow::WaveBreak, 1).waveBreakS == 30 && StepSetting(z, LobbyRow::WaveBreak, -1).waveBreakS == 120);
    MatchConfig f;
    CHECK(StepSetting(f, LobbyRow::ClearGround, 1).clearGroundMin == 2 && StepSetting(f, LobbyRow::ClearGround, -1).clearGroundMin == 5);
    f.clearGroundMin = 5; CHECK(StepSetting(f, LobbyRow::ClearGround, 1).clearGroundMin == 0);
    auto has = [](const MatchConfig& c, LobbyRow r) { for (LobbyRow x : SettingsFor(c)) if (x == r) return true; return false; };
    CHECK(has(b, LobbyRow::WaveBreak) && has(z, LobbyRow::WaveBreak) && !has(b, LobbyRow::ClearGround));
    MatchConfig d; d.format = Format::Duel;
    CHECK(!has(d, LobbyRow::ClearGround) && !has(d, LobbyRow::WaveBreak));
    CHECK(has(MatchConfig{}, LobbyRow::ClearGround));
    MatchConfig br; br.gear = Gear::Fists; CHECK(has(br, LobbyRow::ClearGround) && !has(br, LobbyRow::Tier) && !has(br, LobbyRow::Gear));   // Brawl: fixed strength, no gear
    MatchConfig t; t.format = Format::TwoTeams; CHECK(has(t, LobbyRow::ClearGround));
}

static void TestStepSetting() {
    MatchConfig c;                                       // free-for-all, limit 5
    CHECK(StepSetting(c, LobbyRow::Limit, 1).scoreLimit == 10);
    CHECK(StepSetting(c, LobbyRow::Limit, -1).scoreLimit == 3);
    c.scoreLimit = 50;
    CHECK(StepSetting(c, LobbyRow::Limit, 1).scoreLimit == 3);
    c.scoreLimit = 3;
    CHECK(StepSetting(c, LobbyRow::Limit, -1).scoreLimit == 50);
    CHECK((LimitOptions(Format::TwoTeams) == std::vector<int>{3, 5, 10, 15, 20, 30, 50}));
    MatchConfig d; d.format = Format::Duel; d.scoreLimit = 3;
    CHECK((LimitOptions(Format::Duel) == std::vector<int>{1, 2, 3, 5, 7, 10}));
    CHECK(StepSetting(d, LobbyRow::Limit, 1).scoreLimit == 5 && StepSetting(d, LobbyRow::Limit, -1).scoreLimit == 2);
    d.scoreLimit = 1;
    CHECK(StepSetting(d, LobbyRow::Limit, -1).scoreLimit == 10);
    d.scoreLimit = 10;
    CHECK(StepSetting(d, LobbyRow::Limit, 1).scoreLimit == 1);

    MatchConfig t;
    CHECK(StepSetting(t, LobbyRow::Time, 1).timeLimitS == 300 && StepSetting(t, LobbyRow::Time, -1).timeLimitS == 1200);
    t.timeLimitS = 1200;
    CHECK(StepSetting(t, LobbyRow::Time, 1).timeLimitS == 0 && StepSetting(t, LobbyRow::Time, -1).timeLimitS == 900);

    MatchConfig r;                                       // respawn 5
    CHECK((kRespawnOptions == std::vector<int>{3, 5, 10, 15, 20}));
    CHECK(StepSetting(r, LobbyRow::Respawn, 1).respawnDelayS == 10 && StepSetting(r, LobbyRow::Respawn, -1).respawnDelayS == 3);
    r.respawnDelayS = 3;
    CHECK(StepSetting(r, LobbyRow::Respawn, -1).respawnDelayS == 20);
    r.respawnDelayS = 20;
    CHECK(StepSetting(r, LobbyRow::Respawn, 1).respawnDelayS == 3);

    MatchConfig g;                                       // own gear
    CHECK(StepSetting(g, LobbyRow::Gear, 1).gear == Gear::RandomTier && StepSetting(g, LobbyRow::Gear, -1).gear == Gear::Mirror);
    g.gear = Gear::Mirror;
    CHECK(StepSetting(g, LobbyRow::Gear, 1).gear == Gear::Own && StepSetting(g, LobbyRow::Gear, -1).gear == Gear::RandomTier);
    g.gear = Gear(7);                                    // out of range from the lobby: steps as if own
    CHECK(StepSetting(g, LobbyRow::Gear, 1).gear == Gear::RandomTier);

    MatchConfig k;                                       // tier 2
    CHECK(StepSetting(k, LobbyRow::Tier, 1).tier == 3 && StepSetting(k, LobbyRow::Tier, -1).tier == 1);
    k.tier = 7;
    CHECK(StepSetting(k, LobbyRow::Tier, 1).tier == 0 && StepSetting(k, LobbyRow::Tier, -1).tier == 6);
    k.tier = 0;
    CHECK(StepSetting(k, LobbyRow::Tier, -1).tier == 7);
    k.tier = 9;
    CHECK(StepSetting(k, LobbyRow::Tier, 1).tier == 0);

    MatchConfig f;
    CHECK(StepSetting(f, LobbyRow::FriendlyFire, 1).friendlyFire && StepSetting(f, LobbyRow::FriendlyFire, -1).friendlyFire);
    f.friendlyFire = true;
    CHECK(!StepSetting(f, LobbyRow::FriendlyFire, 1).friendlyFire);

    MatchConfig m; m.scoreLimit = 7; m.tier = 4; m.gear = Gear::Mirror; m.timeLimitS = 600; m.respawnDelayS = 15;
    MatchConfig same = StepSetting(m, LobbyRow::MyTeam, 1);
    CHECK(same.scoreLimit == 7 && same.tier == 4 && same.gear == Gear::Mirror && same.timeLimitS == 600 && same.respawnDelayS == 15 && !same.friendlyFire);
    MatchConfig only = StepSetting(m, LobbyRow::Tier, 1);  // a step changes its own setting alone
    CHECK(only.tier == 5 && only.scoreLimit == 7 && only.gear == Gear::Mirror && only.timeLimitS == 600 && only.respawnDelayS == 15);
}

static void TestTeamPick() {
    CHECK(StepTeamPick(kNoTeam, 2, 1) == 0 && StepTeamPick(0, 2, 1) == 1 && StepTeamPick(1, 2, 1) == kNoTeam);
    CHECK(StepTeamPick(kNoTeam, 2, -1) == 1 && StepTeamPick(1, 2, -1) == 0 && StepTeamPick(0, 2, -1) == kNoTeam);
    CHECK(StepTeamPick(kNoTeam, 3, -1) == 2 && StepTeamPick(2, 3, -1) == 1 && StepTeamPick(2, 3, 1) == kNoTeam);
    CHECK(StepTeamPick(2, 2, -1) == 1 && StepTeamPick(2, 2, 1) == kNoTeam);   // a pick the format lacks is Auto
    CHECK(StepTeamPick(5, 2, -1) == 1 && StepTeamPick(5, 3, -1) == 2);
    CHECK(StepTeamPick(kNoTeam, 0, -1) == kNoTeam && StepTeamPick(kNoTeam, 0, 1) == kNoTeam);
}

static void TestSelectFormat() {
    MatchConfig c; c.scoreLimit = 20; c.timeLimitS = 600; c.gear = Gear::Mirror; c.tier = 5; c.respawnDelayS = 10; c.friendlyFire = true;
    MatchConfig d = SelectFormat(c, Format::Duel);
    CHECK(d.format == Format::Duel && d.scoreLimit == 3);
    CHECK(d.timeLimitS == 600 && d.gear == Gear::Mirror && d.tier == 5 && d.respawnDelayS == 10 && d.friendlyFire);
    CHECK(SelectFormat(c, Format::TwoTeams).scoreLimit == 10);
    MatchConfig same = SelectFormat(c, Format::FreeForAll);
    CHECK(same.format == Format::FreeForAll && same.scoreLimit == 20);   // the card already chosen: nothing changes
}

static void TestPill() {
    LobbyLocal l; l.version = "0.2.0";
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.pill.text == "Network not ready" && Same(s.pill.color, kWarn));
    CHECK(!s.idle && !s.in);
    l.phase = LobbyPhase::Idle;
    s = BuildLobbyScene(l);
    CHECK(s.pill.text == "Not in a lobby" && Same(s.pill.color, kIron) && s.idle && !s.in);
    l.phase = LobbyPhase::Creating;
    s = BuildLobbyScene(l);
    CHECK(s.pill.text == "Creating a lobby" && Same(s.pill.color, kWarn) && !s.idle && !s.in);
    l.phase = LobbyPhase::Joining;
    s = BuildLobbyScene(l);
    CHECK(s.pill.text == "Joining a lobby" && Same(s.pill.color, kWarn) && !s.idle && !s.in);

    LobbyLocal h = InLobby(true);
    s = BuildLobbyScene(h);
    CHECK(s.pill.text == "Hosting via direct UDP" && Same(s.pill.color, kOk) && s.in && !s.idle);
    h.publicLobby = true;
    CHECK(BuildLobbyScene(h).pill.text == "Hosting via direct UDP");
    LobbyLocal m = InLobby(false);
    s = BuildLobbyScene(m);
    CHECK(s.pill.text == "In Ann's lobby" && Same(s.pill.color, kOk));
    m.players = {Player("Bob", false, true)};             // the owner not in the list yet
    CHECK(BuildLobbyScene(m).pill.text == "In a lobby");
    m.players = {Player("", true), Player("Bob", false, true)};
    CHECK(BuildLobbyScene(m).pill.text == "In a lobby");
}

static void TestHeader() {
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0"; l.myName = "Ann"; l.status = "3 public lobbies found";
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.me == "Ann");
    CHECK(s.badge == "v0.2.0 " + kDot + " protocol " + std::to_string(int(kProtocolRevision)));
    CHECK(s.status == "3 public lobbies found");
    l.version = "1.4.2";
    CHECK(BuildLobbyScene(l).badge == "v1.4.2 " + kDot + " protocol " + std::to_string(int(kProtocolRevision)));
    l.phase = LobbyPhase::Joining; l.status = "Joining lobby...";
    CHECK(BuildLobbyScene(l).status == "Joining lobby...");
    LobbyLocal in = InLobby(true);
    in.status = "Hosting - invite friends, then start any fight";
    CHECK(BuildLobbyScene(in).status.empty());            // in a lobby the pill says it
}

static void TestBrowser() {
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0";
    LobbyFound ok; ok.name = "Ann's fight"; ok.map = "Yard"; ok.mode = "Duel"; ok.players = 2; ok.compatible = true;
    LobbyFound old; old.name = "Old"; old.players = 1; old.compatible = false; old.reason = "different mod version (host 0.1.0, you 0.2.0)";
    LobbyFound bare; bare.players = 3; bare.compatible = false;
    LobbyFound full; full.name = "Full"; full.map = "Pit"; full.players = 8; full.compatible = true;
    LobbyFound modeOnly; modeOnly.name = "M"; modeOnly.mode = "Zombies"; modeOnly.players = 5; modeOnly.compatible = true;
    l.found = {ok, old, bare, full, modeOnly};
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 5);
    if (s.browser.size() == 5) {
        CHECK(s.browser[0].name == "Ann's fight" && s.browser[0].players == "2/8" && s.browser[0].detail == "Yard " + kDot + " Duel");
        CHECK(s.browser[0].joinable && s.browser[0].note.text == "Join" && Same(s.browser[0].note.color, kOk));
        CHECK(!s.browser[1].joinable && s.browser[1].note.text == old.reason && Same(s.browser[1].note.color, kWarn));
        CHECK(s.browser[1].detail.empty() && s.browser[1].players == "1/8");
        CHECK(s.browser[2].name == "Unnamed lobby" && s.browser[2].note.text == "Incompatible" && !s.browser[2].joinable);
        CHECK(!s.browser[3].joinable && s.browser[3].note.text == "Full" && Same(s.browser[3].note.color, kWarn) && s.browser[3].detail == "Pit");
        CHECK(s.browser[4].joinable && s.browser[4].detail == "Zombies" && s.browser[4].players == "5/8");
    }
    l.found.assign(8, ok);
    CHECK(BuildLobbyScene(l).browser.size() == kBrowserRows && kBrowserRows == 6);
    for (LobbyFound& f : l.found) f.maxPlayers = 2;          // the limit is each lobby's own (hsm_max), not one global
    s = BuildLobbyScene(l);
    CHECK(!s.browser.empty() && !s.browser[0].joinable && s.browser[0].players == "2/2" && s.browser[0].note.text == "Full");
    l.phase = LobbyPhase::Joining;
    CHECK(BuildLobbyScene(l).browser.empty());
    LobbyLocal in = InLobby(true);
    in.found = {ok};
    CHECK(BuildLobbyScene(in).browser.empty());           // the browser is for finding a lobby
}

// The host's player limit: each listing carries its own, the row reads "3/4", Full and Has room go by it, the title and the
// Players row show the lobby's.
static void TestPlayerLimit() {
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0";
    LobbyFound four; four.name = "Four"; four.players = 3; four.maxPlayers = 4; four.compatible = true;
    LobbyFound fourFull = four; fourFull.name = "FourFull"; fourFull.players = 4;
    LobbyFound two; two.name = "Two"; two.players = 2; two.maxPlayers = 2; two.compatible = true;
    LobbyFound legacy; legacy.name = "Legacy"; legacy.players = 5; legacy.compatible = true;      // no hsm_max: 8
    l.found = {four, fourFull, two, legacy};
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 4);
    if (s.browser.size() == 4) {
        CHECK(s.browser[0].players == "3/4" && s.browser[0].joinable && s.browser[0].note.text == "Join");
        CHECK(s.browser[1].players == "4/4" && !s.browser[1].joinable && s.browser[1].note.text == "Full");
        CHECK(s.browser[2].players == "2/2" && !s.browser[2].joinable && s.browser[2].note.text == "Full");
        CHECK(s.browser[3].players == "5/8" && s.browser[3].joinable);
    }
    l.maxPlayers = 3;                                                   // the in-lobby limit never leaks into the browser
    CHECK(BuildLobbyScene(l).browser[0].players == "3/4");
    l.maxPlayers = 8;
    // Has room goes by the lobby's own limit.
    CHECK(PassesFilter(four, BrowserFilter{}) && PassesFilter(fourFull, BrowserFilter{}));
    BrowserFilter room; room.hasRoom = true;
    CHECK(PassesFilter(four, room) && !PassesFilter(fourFull, room) && !PassesFilter(two, room) && PassesFilter(legacy, room));
    LobbyFound seven = legacy; seven.players = 7;
    CHECK(PassesFilter(seven, room));
    seven.players = 8;
    CHECK(!PassesFilter(seven, room));
    l.filter = room;
    s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 2 && s.browser[0].players == "3/4" && s.browser[1].players == "5/8" && s.browserCount == "2 of 4 lobbies shown");
    // In the lobby: the title is n/limit and the Players row reads the limit (the host steps it, members read it).
    LobbyLocal in = InLobby(true);
    in.maxPlayers = 4;
    LobbyScene h = BuildLobbyScene(in);
    CHECK(h.playersTitle == "PLAYERS 2/4" && h.hasPlayerLimit);
    CHECK(h.playerLimit.id == LobbyRow::Players && h.playerLimit.label == "Players" && h.playerLimit.value == "4" && h.playerLimit.enabled && !h.playerLimit.hint.empty());
    for (const SettingRow& r : h.settings) CHECK(r.id != LobbyRow::Players);   // it is not a match setting
    LobbyLocal mem = InLobby(false);
    mem.maxPlayers = 6;
    LobbyScene m = BuildLobbyScene(mem);
    CHECK(m.hasPlayerLimit && m.playerLimit.value == "6" && !m.playerLimit.enabled && m.playersTitle == "PLAYERS 2/6");
    LobbyLocal prog = InLobby(true); prog.progression = true;
    CHECK(BuildLobbyScene(prog).hasPlayerLimit);
    LobbyLocal idle; idle.phase = LobbyPhase::Idle;
    CHECK(!BuildLobbyScene(idle).hasPlayerLimit);
    CHECK(SectionOf(LobbyRow::Players) == LobbySection::Match);
    // StepSetting leaves the config alone for it: the limit is the lobby's (StepPlayerLimit).
    MatchConfig c;
    const MatchConfig stepped = StepSetting(c, LobbyRow::Players, 1);
    CHECK(stepped.scoreLimit == c.scoreLimit && stepped.format == c.format);
}

// A lobby with a modpack: the row names it, and a lobby this machine cannot join says why in its note.
static void TestBrowserPack() {
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0";
    LobbyFound have; have.name = "A"; have.map = "Yard"; have.players = 1; have.compatible = true; have.pack = "pack ArenaPack";
    LobbyFound lack; lack.name = "B"; lack.players = 1; lack.compatible = false; lack.reason = "Install the pack"; lack.pack = "pack ArenaPack (not installed)";
    LobbyFound plain; plain.name = "C"; plain.players = 1; plain.compatible = true;
    l.found = {have, lack, plain};
    const LobbyScene s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 3);
    if (s.browser.size() == 3) {
        CHECK(s.browser[0].detail == "Yard " + kDot + " pack ArenaPack" && s.browser[0].joinable);
        CHECK(s.browser[1].detail == "pack ArenaPack (not installed)" && !s.browser[1].joinable && s.browser[1].note.text == "Install the pack");
        CHECK(s.browser[2].detail.empty());
    }
    // The lobby's mod rule follows the pack; a lobby whose rule refuses this machine's mods says why in its note.
    LobbyFound rule; rule.name = "D"; rule.map = "Yard"; rule.players = 1; rule.compatible = true; rule.pack = "pack ArenaPack"; rule.mods = "mods: Vanilla";
    LobbyFound blocked; blocked.name = "E"; blocked.players = 1; blocked.compatible = false; blocked.reason = "Blocks HSEnhancer";
    l.found = {rule, blocked};
    const LobbyScene m = BuildLobbyScene(l);
    CHECK(m.browser.size() == 2);
    if (m.browser.size() == 2) {
        CHECK(m.browser[0].detail == "Yard " + kDot + " pack ArenaPack " + kDot + " mods: Vanilla" && m.browser[0].joinable);
        CHECK(!m.browser[1].joinable && m.browser[1].note.text == "Blocks HSEnhancer");
    }
}

// Locked lobbies: the row says so in plain text and can be hidden with the No password chip.
static void TestPasswordFilter() {
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0";
    LobbyFound open; open.name = "Open"; open.players = 1; open.compatible = true;
    LobbyFound lockedL; lockedL.name = "Private"; lockedL.players = 2; lockedL.compatible = true; lockedL.locked = true;
    LobbyFound noName; noName.players = 1; noName.compatible = true; noName.locked = true;
    l.found = {open, lockedL, noName};
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 3 && s.filters.size() == 5 && kFilterChips == 5);
    if (s.browser.size() == 3) {
        CHECK(s.browser[0].name == "Open" && !s.browser[0].locked);
        CHECK(s.browser[1].name == std::string(kLockedMarker) + "Private" && s.browser[1].locked && s.browser[1].joinable && s.browser[1].note.text == "Join");
        CHECK(s.browser[2].name == std::string(kLockedMarker) + "Unnamed lobby" && s.browser[2].locked);
        CHECK(s.browser[0].name.find(kLockedMarker) == std::string::npos);
    }
    for (unsigned char ch : std::string(kLockedMarker)) CHECK(ch >= 0x20 && ch < 0x80);   // plain ASCII: no glyph the font may lack
    CHECK(s.filters[4].label == "No password" && !s.filters[4].on);
    BrowserFilter f = ToggleFilter(BrowserFilter{}, 4);
    CHECK(f.noPassword && !f.hasRoom && !f.compatibleOnly && !f.mostPlayers && f.mode == 0);
    CHECK(!ToggleFilter(f, 4).noPassword);
    CHECK(PassesFilter(open, f) && !PassesFilter(lockedL, f) && PassesFilter(lockedL, BrowserFilter{}));
    l.filter = f;
    s = BuildLobbyScene(l);
    CHECK(s.filters[4].on && s.browser.size() == 1 && s.browser[0].name == "Open" && s.browserCount == "1 of 3 lobbies shown");
    BrowserFilter both = ToggleFilter(f, 1);                                           // with Has room: both apply
    LobbyFound fullOpen = open; fullOpen.players = 8;
    CHECK(PassesFilter(open, both) && !PassesFilter(fullOpen, both) && !PassesFilter(lockedL, both));
}

// The host's password box: host only, in a lobby; its note says whether the lobby is locked.
static void TestHostPassword() {
    LobbyLocal h = InLobby(true);
    LobbyScene s = BuildLobbyScene(h);
    CHECK(s.hostPassword && s.passwordNote.rfind("Open", 0) == 0);
    h.locked = true;
    s = BuildLobbyScene(h);
    CHECK(s.hostPassword && s.passwordNote.rfind("Locked", 0) == 0 && s.passwordNote.find("kicks nobody") != std::string::npos);
    LobbyLocal m = InLobby(false); m.locked = true;
    s = BuildLobbyScene(m);
    CHECK(!s.hostPassword && s.passwordNote.empty());                      // members have no box
    LobbyLocal idle; idle.phase = LobbyPhase::Idle; idle.host = true; idle.locked = true;
    s = BuildLobbyScene(idle);
    CHECK(!s.hostPassword && s.passwordNote.empty());
}

static void TestOutOfLobby() {
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0"; l.players = {Player("Ann", true, true)};
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.roster.empty() && s.modes.empty() && s.settings.empty() && s.maps.empty());
    CHECK(!s.start && s.waiting.empty() && s.hostOnly.empty() && !s.balance && s.playersTitle.empty());
}

static void TestHostAgainstMember() {
    LobbyScene h = BuildLobbyScene(InLobby(true, Format::TwoTeams, Gear::RandomTier));
    CHECK(h.start && h.waiting.empty() && h.hostOnly.empty() && h.balance);
    for (const ModeCard& c : h.modes) CHECK(c.enabled == ModeAvailable(c.format, 2).ok);   // two players: Riot and Juggernaut need 3
    for (const SettingRow& r : h.settings) CHECK(r.enabled);
    for (const MapCard& m : h.maps) CHECK(m.enabled == m.allowed);
    for (const RosterRow& r : h.roster) CHECK(r.movable);

    LobbyScene m = BuildLobbyScene(InLobby(false, Format::TwoTeams, Gear::RandomTier));
    CHECK(!m.start && m.waiting == "Waiting for the host" && m.hostOnly == "Only the host can change these." && !m.balance);
    CHECK(m.modes.size() == kModeCardCount && m.settings.size() == h.settings.size() && m.maps.size() == PickableCount());
    for (const ModeCard& c : m.modes) CHECK(!c.enabled);
    for (const SettingRow& r : m.settings) CHECK(r.enabled == (r.id == LobbyRow::MyTeam));   // their own team only
    for (const MapCard& c : m.maps) CHECK(!c.enabled);
    for (const RosterRow& r : m.roster) CHECK(!r.movable);

    CHECK(!BuildLobbyScene(InLobby(true, Format::FreeForAll)).balance);
    CHECK(!BuildLobbyScene(InLobby(true, Format::PlayersVsBots)).balance);
    CHECK(BuildLobbyScene(InLobby(true, Format::LastManStanding)).balance == false && BuildLobbyScene(Teamed(true, 4)).balance);
}

static void TestRowsPerFormat() {
    using V = std::vector<LobbyRow>;
    using enum LobbyRow;
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::FreeForAll))) == (V{Limit, Time, Respawn, FreeCam, Gear, Tier, Damage, ClearGround, Crossbows, Sync}));   // 0.8.0-pre20: the tier is strength, own gear too
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::FreeForAll, Gear::RandomTier))) == (V{Limit, Time, Respawn, FreeCam, Gear, Tier, Weapons, Damage, ClearGround, Crossbows, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::FreeForAll, Gear::Mirror))) == (V{Limit, Time, Respawn, FreeCam, Gear, Tier, Weapons, Damage, ClearGround, Crossbows, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::TwoTeams))) == (V{Limit, Time, Respawn, FreeCam, Gear, Tier, Teams, FriendlyFire, Damage, ClearGround, MyTeam, TeamKit, Crossbows, Sync}));
    CHECK(Ids(BuildLobbyScene(Teamed(true, 3, Gear::Mirror))) == (V{Limit, Time, Respawn, FreeCam, Gear, Tier, Weapons, Teams, FriendlyFire, Damage, ClearGround, MyTeam, TeamKit, Crossbows, Sync}));
    CHECK(Ids(BuildLobbyScene(Teamed(false, 4))) == (V{Limit, Time, Respawn, FreeCam, Gear, Tier, Teams, FriendlyFire, Damage, ClearGround, MyTeam, TeamKit, Crossbows, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::LastManStanding))) == (V{Limit, Time, Respawn, FreeCam, Gear, Tier, Damage, Crossbows, Sync}));   // Duel's rows
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::LastManStanding, Gear::RandomTier))) == Ids(BuildLobbyScene(InLobby(true, Format::Duel, Gear::RandomTier))));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::Duel))) == (V{Limit, Time, Respawn, FreeCam, Gear, Tier, Damage, Crossbows, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::Duel, Gear::RandomTier))) == (V{Limit, Time, Respawn, FreeCam, Gear, Tier, Weapons, Damage, Crossbows, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::PlayersVsBots))) == (V{Time, FreeCam, Gear, Tier, WaveBreak, FriendlyFire, Damage, Crossbows, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::Zombies, Gear::RandomTier))) == (V{Time, FreeCam, Gear, Tier, Weapons, WaveBreak, FriendlyFire, Damage, Crossbows, Sync}));   // 0.8.0-pre20: the bot formats take a gear setting
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::PlayersVsBots, Gear::Mirror))) == (V{Time, FreeCam, Gear, Tier, Weapons, WaveBreak, FriendlyFire, Damage, Crossbows, Sync}));
    // Wave 2 integration: the team rounds and the role formats take every row that fits them. Team Duel is Duel's rows with the team rows; Riot and
    // Juggernaut deal gear, tier, build and team by role, so only the match rules, Free camera, Team colours and Crossbows remain.
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::TeamDuel))) == (V{Limit, Time, Respawn, FreeCam, Gear, Tier, Teams, FriendlyFire, Damage, MyTeam, TeamKit, Crossbows, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::Riot))) == (V{Limit, Time, Respawn, FreeCam, FriendlyFire, Damage, TeamKit, Crossbows, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::Juggernaut))) == (V{Limit, Time, Respawn, FreeCam, FriendlyFire, Damage, TeamKit, Crossbows, Sync}));
    for (Format f : {Format::TwoTeams, Format::TeamDuel, Format::Riot, Format::Juggernaut}) { MatchConfig c; c.format = f; CHECK(HasTeams(f)); bool k = false; for (LobbyRow r : SettingsFor(c)) k = k || r == TeamKit; CHECK(k); }
    for (Format f : {Format::FreeForAll, Format::Duel, Format::LastManStanding, Format::PlayersVsBots, Format::Zombies}) { MatchConfig c; c.format = f; for (LobbyRow r : SettingsFor(c)) CHECK(r != TeamKit); }   // no teams (the bot formats always wear the full kit)
    { MatchConfig br; br.gear = Gear::Fists; for (LobbyRow r : SettingsFor(br)) CHECK(r != Crossbows && r != TeamKit); CHECK(SectionOf(TeamKit) == LobbySection::Teams); }
}

// The Weapons row (0.8.0-pre23): only where gear is generated; steps through the five filters; the host's alone.
static void TestWeaponsRow() {
    auto has = [](const MatchConfig& c, LobbyRow r) { for (LobbyRow x : SettingsFor(c)) if (x == r) return true; return false; };
    for (Format f : {Format::FreeForAll, Format::Duel, Format::TwoTeams, Format::ThreeTeams, Format::PlayersVsBots, Format::Zombies}) {
        MatchConfig c; c.format = f;
        c.gear = Gear::Own; CHECK(!has(c, LobbyRow::Weapons));
        c.gear = Gear::RandomTier; CHECK(has(c, LobbyRow::Weapons));
        c.gear = Gear::Mirror; CHECK(has(c, LobbyRow::Weapons));
    }
    MatchConfig br; br.gear = Gear::Fists; CHECK(!has(br, LobbyRow::Weapons));
    CHECK(SectionOf(LobbyRow::Weapons) == LobbySection::Gear);
    CHECK(std::string(WeaponFilterName(WeaponFilter::Any)) == "Any" && std::string(WeaponFilterName(WeaponFilter::Swords1H)) == "1H swords" &&
          std::string(WeaponFilterName(WeaponFilter::Swords2H)) == "2H swords" && std::string(WeaponFilterName(WeaponFilter::Polearms)) == "Polearms" &&
          std::string(WeaponFilterName(WeaponFilter::AxesMaces)) == "Axes & maces");
    MatchConfig c; c.gear = Gear::RandomTier;
    const WeaponFilter fwd[] = {WeaponFilter::Swords1H, WeaponFilter::Swords2H, WeaponFilter::Polearms, WeaponFilter::AxesMaces, WeaponFilter::Any};
    for (WeaponFilter w : fwd) { c = StepSetting(c, LobbyRow::Weapons, 1); CHECK(c.weapons == w); }
    CHECK(StepSetting(c, LobbyRow::Weapons, -1).weapons == WeaponFilter::AxesMaces);   // Any steps back to the last
    MatchConfig o = StepSetting(c, LobbyRow::Weapons, 1);
    CHECK(o.gear == c.gear && o.tier == c.tier && o.format == c.format);                // a step changes its own setting alone
    CHECK(StepSetting(MatchConfig{}, LobbyRow::Weapons, 1).weapons == WeaponFilter::Swords1H);
    LobbyLocal l = InLobby(true, Format::FreeForAll, Gear::RandomTier);
    l.config.weapons = WeaponFilter::Polearms;
    const LobbyScene sc = BuildLobbyScene(l);
    const SettingRow& r = Need(sc, LobbyRow::Weapons);
    CHECK(r.label == "Weapons" && r.value == "Polearms" && r.enabled && !r.hint.empty());
    CHECK(BuildLobbyScene(l).summary.find(", Polearms") != std::string::npos);
    l.config.weapons = WeaponFilter::Any;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Weapons).value == "Any" && BuildLobbyScene(l).summary.find("Any") == std::string::npos);
    l.host = false; l.players = {Player("Ann", true, false), Player("Bob", false, true)};
    CHECK(!Need(BuildLobbyScene(l), LobbyRow::Weapons).enabled);
}

// The Crossbows row (rev 36): in every format but Brawl, steps Off / 1 / 2 / Half / All, the host's alone, marked experimental.
static void TestCrossbowsRow() {
    auto has = [](const MatchConfig& c, LobbyRow r) { for (LobbyRow x : SettingsFor(c)) if (x == r) return true; return false; };
    for (Format f : {Format::FreeForAll, Format::Duel, Format::LastManStanding, Format::TwoTeams, Format::TeamDuel, Format::Riot, Format::Juggernaut, Format::PlayersVsBots, Format::Zombies}) {
        MatchConfig c; c.format = f; CHECK(has(c, LobbyRow::Crossbows));
    }
    MatchConfig brawl; brawl.gear = Gear::Fists; CHECK(!has(brawl, LobbyRow::Crossbows));
    CHECK(SectionOf(LobbyRow::Crossbows) == LobbySection::Gear);
    MatchConfig c;
    CHECK(c.crossbows == 0);                                                          // Off by default
    const uint8_t fwd[] = {1, 2, 3, 4, 0};
    for (uint8_t v : fwd) { c = StepSetting(c, LobbyRow::Crossbows, 1); CHECK(c.crossbows == v); }
    CHECK(StepSetting(c, LobbyRow::Crossbows, -1).crossbows == 4);                    // Off steps back to All
    MatchConfig bad; bad.crossbows = 77; CHECK(StepSetting(bad, LobbyRow::Crossbows, 1).crossbows == 1);   // out of range steps from Off
    MatchConfig o = StepSetting(c, LobbyRow::Crossbows, 1);
    CHECK(o.gear == c.gear && o.tier == c.tier && o.format == c.format && o.weapons == c.weapons);   // a step changes its own setting alone
    CHECK(Sanitize(bad).crossbows == 0 && Sanitize(o).crossbows == 1);
    MatchConfig fists; fists.gear = Gear::Fists; fists.crossbows = 2; CHECK(Sanitize(fists).crossbows == 0);   // bare fists: no crossbow
    LobbyLocal l = InLobby(true, Format::FreeForAll);
    l.config.crossbows = 3;
    const LobbyScene sc = BuildLobbyScene(l);
    const SettingRow& r = Need(sc, LobbyRow::Crossbows);
    CHECK(r.label == "Crossbows" && r.value == "Half" && r.enabled && r.hint.find("Experimental: needs the crossbow test") == 0);
    l.config.crossbows = 0; CHECK(Need(BuildLobbyScene(l), LobbyRow::Crossbows).value == "Off");
    l.host = false; l.players = {Player("Ann", true, false), Player("Bob", false, true)};
    CHECK(!Need(BuildLobbyScene(l), LobbyRow::Crossbows).enabled);
}

static void TestLabelsAndValues() {
    LobbyLocal l = InLobby(true, Format::FreeForAll, Gear::RandomTier);
    l.config.scoreLimit = 15; l.config.tier = 3; l.config.respawnDelayS = 10;
    LobbyScene s = BuildLobbyScene(l);
    CHECK(Need(s, LobbyRow::Limit).label == "Score limit" && Need(s, LobbyRow::Limit).value == "15");
    CHECK(Need(s, LobbyRow::Time).label == "Time limit" && Need(s, LobbyRow::Time).value == "off");
    CHECK(Need(s, LobbyRow::Respawn).label == "Respawn delay" && Need(s, LobbyRow::Respawn).value == "10 s");
    CHECK(Need(s, LobbyRow::Gear).label == "Gear" && Need(s, LobbyRow::Gear).value == "Random by tier");
    CHECK(Need(s, LobbyRow::Tier).label == "Gear tier" && Need(s, LobbyRow::Tier).value == "tier 3 " + kDot + " budget 100");
    l.config.timeLimitS = 600; l.config.gear = Gear::Mirror; l.config.tier = 7;
    s = BuildLobbyScene(l);
    CHECK(Need(s, LobbyRow::Time).value == "10 min" && Need(s, LobbyRow::Gear).value == "Everyone identical");
    CHECK(Need(s, LobbyRow::Tier).value == "tier 7 " + kDot + " budget 888");
    l.config.gear = Gear::Own;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Gear).value == "Own loadouts");
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Tier).value == "tier 7");                       // own gear: a strength, no gear budget
    // 0.9.0: the sync row, per player (owner sync) by default; a step flips it either way.
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Sync).label == "Sync" && Need(BuildLobbyScene(l), LobbyRow::Sync).value == "Per player");
    CHECK(StepSetting(l.config, LobbyRow::Sync, 1).hostPhysics && StepSetting(l.config, LobbyRow::Sync, -1).hostPhysics);
    l.config.hostPhysics = true;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Sync).value == "Host physics" && !StepSetting(l.config, LobbyRow::Sync, 1).hostPhysics);
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Sync).hint.find("one ping late") != std::string::npos);
    l.config.hostPhysics = false;
    // Wave 2: the free camera row, in MATCH, on by default; a step flips it either way and a member sees it read-only.
    CHECK(MatchConfig{}.freeCam && SectionOf(LobbyRow::FreeCam) == LobbySection::Match);
    CHECK(Need(BuildLobbyScene(l), LobbyRow::FreeCam).label == "Free camera" && Need(BuildLobbyScene(l), LobbyRow::FreeCam).value == "On");
    CHECK(!StepSetting(l.config, LobbyRow::FreeCam, 1).freeCam && !StepSetting(l.config, LobbyRow::FreeCam, -1).freeCam);
    l.config.freeCam = false;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::FreeCam).value == "Off" && StepSetting(l.config, LobbyRow::FreeCam, 1).freeCam);
    CHECK(Need(BuildLobbyScene(l), LobbyRow::FreeCam).enabled && !Need(BuildLobbyScene(InLobby(false)), LobbyRow::FreeCam).enabled);
    l.config.freeCam = true;

    LobbyLocal d = InLobby(true, Format::Duel);
    d.config.respawnDelayS = 3;
    s = BuildLobbyScene(d);
    CHECK(Need(s, LobbyRow::Limit).label == "Rounds to win" && Need(s, LobbyRow::Limit).value == "3");
    CHECK(Need(s, LobbyRow::Respawn).label == "Round break" && Need(s, LobbyRow::Respawn).value == "3 s");

    LobbyLocal b = InLobby(true, Format::PlayersVsBots);
    b.config.tier = 4;
    s = BuildLobbyScene(b);
    CHECK(Need(s, LobbyRow::Tier).label == "Starting tier" && Need(s, LobbyRow::Tier).value == "tier 4");
    CHECK(Need(s, LobbyRow::FriendlyFire).label == "Friendly fire" && Need(s, LobbyRow::FriendlyFire).value == "Off");
    b.config.friendlyFire = true;
    CHECK(Need(BuildLobbyScene(b), LobbyRow::FriendlyFire).value == "On");

    CHECK(TierBudget(0) == 0 && TierBudget(1) == 15 && TierBudget(2) == 50 && TierBudget(3) == 100);
    CHECK(TierBudget(4) == 250 && TierBudget(5) == 444 && TierBudget(6) == 666 && TierBudget(7) == 888);
    CHECK(TierBudget(-1) == 0 && TierBudget(9) == 888);
}

static std::string GearHint(Format f, Gear g) { return Need(BuildLobbyScene(InLobby(true, f, g)), LobbyRow::Gear).hint; }

static void TestGearHint() {
    for (Format f : {Format::FreeForAll, Format::TwoTeams, Format::Duel, Format::LastManStanding})
        CHECK(GearHint(f, Gear::Own) == "Everyone fights in their own loadout.");
    for (Format f : {Format::FreeForAll, Format::TwoTeams}) {
        CHECK(GearHint(f, Gear::RandomTier) == "Fresh random gear for each fighter every life.");
        CHECK(GearHint(f, Gear::Mirror) == "One random loadout for everyone, until the rematch.");   // RegearFor: Mirror rolls at a rematch
    }
    CHECK(GearHint(Format::Duel, Gear::RandomTier) == "Fresh random gear for each fighter every round." && GearHint(Format::LastManStanding, Gear::RandomTier) == GearHint(Format::Duel, Gear::RandomTier));
    CHECK(GearHint(Format::Duel, Gear::Mirror) == "One random loadout for everyone, new each round.");
    // Survival and Zombies (0.8.0-pre20): the same three choices; the loadout is rolled at the start and kept for the run.
    for (Format f : {Format::PlayersVsBots, Format::Zombies}) {
        CHECK(GearHint(f, Gear::Own) == "Everyone fights in their own loadout.");
        CHECK(GearHint(f, Gear::RandomTier) == "A random loadout for each player at the start, kept for the whole run.");
        CHECK(GearHint(f, Gear::Mirror) == "One random loadout for everyone, kept for the whole run.");
        CHECK(Need(BuildLobbyScene(InLobby(true, f, Gear::Mirror)), LobbyRow::Gear).value == "Everyone identical");
    }
}

static void TestOtherHints() {
    auto hint = [](Format f, LobbyRow id, auto tweak) { LobbyLocal l = InLobby(true, f, Gear::RandomTier); tweak(l.config); return Need(BuildLobbyScene(l), id).hint; };
    auto none = [](MatchConfig&) {};
    auto clock = [](MatchConfig& c) { c.timeLimitS = 300; };
    auto ff = [](MatchConfig& c) { c.friendlyFire = true; };
    CHECK(hint(Format::FreeForAll, LobbyRow::Limit, none) == "The first to this many kills wins.");
    CHECK(hint(Format::TwoTeams, LobbyRow::Limit, none) == "The first team to this many kills wins.");
    CHECK(hint(Format::LastManStanding, LobbyRow::Limit, none) == "The first to win this many rounds takes the match.");
    CHECK(hint(Format::Duel, LobbyRow::Limit, none) == "The first to win this many rounds takes the match.");
    CHECK(hint(Format::FreeForAll, LobbyRow::Time, none) == "No clock: play to the limit.");
    CHECK(hint(Format::Duel, LobbyRow::Time, clock) == "When it runs out the leader wins; a tie goes to sudden death.");
    CHECK(hint(Format::Zombies, LobbyRow::Time, none) == "No clock: hold out as long as you can.");
    CHECK(hint(Format::PlayersVsBots, LobbyRow::Time, clock) == "Players win if they hold out to the end (one wave cleared).");
    CHECK(hint(Format::Duel, LobbyRow::Respawn, none) == "The pause between the end of a round and the next.");
    CHECK(hint(Format::TwoTeams, LobbyRow::Respawn, none) == "How long the fallen wait to come back.");
    CHECK(hint(Format::FreeForAll, LobbyRow::Respawn, none) == "How long the fallen wait to come back.");
    CHECK(hint(Format::Zombies, LobbyRow::Tier, none) == "Zombies start at this rank and rise one every 3 waves. Also every player's strength and the random gear's budget.");
    CHECK(hint(Format::PlayersVsBots, LobbyRow::Tier, none) == "Bots start at this rank and rise one every 2 waves. Also every player's strength and the random gear's budget.");
    CHECK(hint(Format::Duel, LobbyRow::Tier, none) == "Sets every player's strength; the random gear is rolled within this budget.");
    auto own = [](MatchConfig& c) { c.gear = Gear::Own; };
    CHECK(hint(Format::Duel, LobbyRow::Tier, own) == "Sets every player's strength (muscle and grip).");
    CHECK(hint(Format::PlayersVsBots, LobbyRow::Tier, own) == "Bots start at this rank and rise one every 2 waves. Also every player's strength.");
    CHECK(hint(Format::TwoTeams, LobbyRow::FriendlyFire, none) == "Teammates cannot hurt each other.");
    CHECK(hint(Format::TwoTeams, LobbyRow::FriendlyFire, ff) == "Hits on teammates count.");
    CHECK(hint(Format::PlayersVsBots, LobbyRow::FriendlyFire, none) == "Players cannot hurt each other.");
    CHECK(hint(Format::Zombies, LobbyRow::FriendlyFire, ff) == "Players can hurt each other.");
}

static void TestMyTeam() {
    LobbyLocal l = InLobby(false, Format::TwoTeams);
    LobbyScene s = BuildLobbyScene(l);
    CHECK(Need(s, LobbyRow::MyTeam).label == "My team" && Need(s, LobbyRow::MyTeam).value == "Auto");
    CHECK(Need(s, LobbyRow::MyTeam).hint == "Auto puts you on the smallest team.");
    l.players[1].team = 1;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).value == "Blue");
    l.players[1].team = 2;                                   // Green, picked in three teams: Auto here
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).value == "Auto");
    l.config.teams = 3;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).value == "Green");
    l.players[1].hostOverride = 0;
    s = BuildLobbyScene(l);
    CHECK(Need(s, LobbyRow::MyTeam).value == "Green" && Need(s, LobbyRow::MyTeam).hint == "The host moved you to Red.");
    l.players[1].hostOverride = 3;                           // no fourth team: no move
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).hint == "Auto puts you on the smallest team.");
    l.config.teams = 4;                                      // the fourth team: Gold
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).hint == "The host moved you to Gold.");
    l.players[1].team = 3; l.players[1].hostOverride = -1;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).value == "Gold");
    l.config.teams = 2;                                      // a pick the count no longer has is Auto
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).value == "Auto");
    l.config.teams = 3; l.config.format = Format::ThreeTeams;   // the old value is three teams
    l.players[1].team = 2;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).value == "Green");
    l.players[1].me = false;                                 // not in the list yet
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).value == "Auto");
}

static void TestRoster() {
    LobbyLocal l = InLobby(true, Format::TwoTeams);
    LobbyPlayer cy = Player("Cy", false, false, 0, 1); cy.ready = true;
    l.players = {Player("Ann", true, true, 1), Player("", false, false, 2), cy};
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.playersTitle == "PLAYERS 3/8" && s.roster.size() == 3);
    if (s.roster.size() == 3) {
        const RosterRow& a = s.roster[0];
        CHECK(a.name == "Ann" && a.host && a.me && a.team == "Blue" && Same(a.swatch, TeamColor(1)) && !a.moved && !a.ready);
        const RosterRow& b = s.roster[1];
        CHECK(b.name == "?" && !b.host && !b.me && b.team == "Auto" && Near(b.swatch.a, 0) && !b.moved);
        const RosterRow& c = s.roster[2];
        CHECK(c.team == "Blue" && c.moved && c.ready && Same(c.swatch, TeamColor(1)));   // the host's move wins
    }
    l.players[2].hostOverride = 2;                           // a move to a team this format lacks: their pick stands
    s = BuildLobbyScene(l);
    if (s.roster.size() == 3) CHECK(s.roster[2].team == "Red" && !s.roster[2].moved && Same(s.roster[2].swatch, TeamColor(0)));

    LobbyScene member = BuildLobbyScene(InLobby(false, Format::TwoTeams));   // the host is someone else
    if (member.roster.size() == 2) CHECK(member.roster[0].host && !member.roster[0].me && !member.roster[1].host && member.roster[1].me);

    LobbyLocal f = InLobby(true, Format::FreeForAll);
    f.players = {Player("Ann", true, true, 1, 0)};
    s = BuildLobbyScene(f);
    if (s.roster.size() == 1) CHECK(s.roster[0].team.empty() && Near(s.roster[0].swatch.a, 0) && !s.roster[0].moved && !s.roster[0].movable);
    f.maxPlayers = 4;
    CHECK(BuildLobbyScene(f).playersTitle == "PLAYERS 1/4");
    LobbyLocal z = InLobby(true, Format::Zombies);
    z.players = {Player("Ann", true, true, 1, 1)};
    s = BuildLobbyScene(z);
    if (s.roster.size() == 1) CHECK(s.roster[0].team.empty() && !s.roster[0].moved && !s.roster[0].movable);
}

// A member marks themselves ready (their row's dot); the toggle reads and frames their own ready only. The host starts
// the match instead.
static void TestReady() {
    LobbyLocal l = InLobby(false);                           // Bob, a member
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.ready && !s.readyOn && s.readyLabel == "Mark me ready");
    l.players[1].ready = true;
    s = BuildLobbyScene(l);
    CHECK(s.ready && s.readyOn && s.readyLabel == "Ready " + kDot + " click to undo");
    if (s.roster.size() == 2) CHECK(s.roster[1].me && s.roster[1].ready && !s.roster[0].ready);
    l.players[0].ready = true; l.players[1].ready = false;   // the host's ready is not ours
    s = BuildLobbyScene(l);
    CHECK(s.ready && !s.readyOn && s.readyLabel == "Mark me ready");
    l.players = {Player("Ann", true)};                       // our own row not listed yet
    s = BuildLobbyScene(l);
    CHECK(s.ready && !s.readyOn && s.readyLabel == "Mark me ready");

    LobbyLocal h = InLobby(true);
    h.players[0].ready = true;
    s = BuildLobbyScene(h);
    CHECK(!s.ready && !s.readyOn && s.readyLabel.empty() && s.start);

    for (LobbyPhase p : {LobbyPhase::Offline, LobbyPhase::Idle, LobbyPhase::Creating, LobbyPhase::Joining}) {
        LobbyLocal o = InLobby(false);
        o.phase = p; o.players[1].ready = true;
        s = BuildLobbyScene(o);
        CHECK(!s.ready && !s.readyOn && s.readyLabel.empty());
    }
}

static void TestModes() {
    LobbyScene s = BuildLobbyScene(InLobby(true, Format::Duel));
    CHECK(s.modes.size() == kModeCardCount);
    const Format order[] = {Format::FreeForAll, Format::Duel, Format::LastManStanding, Format::TwoTeams, Format::TeamDuel, Format::Riot,
                            Format::Juggernaut, Format::CaptureTheFlag, Format::Tag, Format::Infection, Format::CrossbowTDM, Format::Tournament, Format::PitBridgeDuel,
                            Format::PlayersVsBots, Format::Zombies};
    const char* names[] = {"FREE-FOR-ALL", "DUEL", "LAST MAN STANDING", "TEAMS", "TEAM DUEL", "RIOT", "JUGGERNAUT", "CAPTURE THE FLAG", "TAG", "INFECTION",
                           "CROSSBOW TDM", "TOURNAMENT", "PIT BRIDGE DUELS", "SURVIVAL", "ZOMBIES"};
    CHECK(kModeOrderCount == 15 && kModeCardCount == 17);
    for (size_t i = 0; i < s.modes.size() && i < kModeOrderCount; ++i) {
        CHECK(s.modes[i].format == order[i] && s.modes[i].name == names[i] && !s.modes[i].description.empty());
        CHECK(s.modes[i].selected == (order[i] == Format::Duel));
    }
    if (s.modes.size() > 1)
        CHECK(s.modes[1].description == "One on one, rounds to the last one standing. Everybody respawns together; new gear each round with random or identical gear.");
    CHECK(std::string(FormatBlurb(Format::LastManStanding)) == "Rounds to the last one standing, for two or more players. Everybody respawns together; new gear each round with random or identical gear.");
    CHECK(std::string(FormatBlurb(Format::FreeForAll)) == "Everyone for themselves. Every kill scores; the first to the limit wins.");
    CHECK(std::string(FormatBlurb(Format::TwoTeams)) == "Two to four teams (Red, Blue, Green, Gold). Every kill scores for the team; the first team to the limit wins.");
    CHECK(std::string(FormatBlurb(Format::ThreeTeams)) == FormatBlurb(Format::TwoTeams));
    CHECK(std::string(FormatBlurb(Format::PlayersVsBots)) == "Everyone against waves of bots that rank up. The fallen return when a wave is cleared.");
    CHECK(std::string(FormatBlurb(Format::Zombies)) == "Zombie waves in the Abyss, one zombie at a time per player. The fallen return when a wave is cleared.");
}

// The Brawl card: free-for-all with bare fists. A mode card (format free-for-all, gear Fists), not a format; no Gear or
// Tier row; picking it and leaving it restore the settings.
static void TestBrawl() {
    LobbyScene s = BuildLobbyScene(InLobby(true, Format::FreeForAll, Gear::Fists));
    CHECK(s.modes.size() == kModeCardCount && kModeCardCount == size_t(kModeOrderCount) + 2);
    const ModeCard& card = s.modes[s.modes.size() - 2];                       // Brawl, then Progression
    CHECK(card.brawl && card.format == Format::FreeForAll && card.name == "BRAWL" && card.selected && card.enabled && !card.progression);
    CHECK(card.description == "Free-for-all with bare fists: everyone spawns naked, no weapons. First to the limit wins.");
    int selected = 0, brawls = 0;
    for (const ModeCard& m : s.modes) { selected += m.selected; brawls += m.brawl; }
    CHECK(selected == 1 && brawls == 1 && !s.modes[0].selected);              // the free-for-all card is not also lit
    CHECK(Ids(s) == (std::vector<LobbyRow>{LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::FreeCam, LobbyRow::Damage, LobbyRow::ClearGround, LobbyRow::Sync}));   // no Gear, no Tier
    CHECK(RowOf(s, LobbyRow::Gear) == nullptr && RowOf(s, LobbyRow::Tier) == nullptr);
    LobbyScene own = BuildLobbyScene(InLobby(true));
    CHECK(own.modes[0].selected && !own.modes[own.modes.size() - 2].selected && RowOf(own, LobbyRow::Gear) != nullptr);
    LobbyScene member = BuildLobbyScene(InLobby(false, Format::FreeForAll, Gear::Fists));
    CHECK(member.modes[member.modes.size() - 2].selected && !member.modes[member.modes.size() - 2].enabled);
    LobbyLocal prog = InLobby(true, Format::FreeForAll, Gear::Fists); prog.progression = true;
    CHECK(!BuildLobbyScene(prog).modes[BuildLobbyScene(prog).modes.size() - 2].selected);   // the Progression card wins

    MatchConfig c; c.format = Format::TwoTeams; c.scoreLimit = 30; c.gear = Gear::Mirror; c.tier = 6; c.timeLimitS = 600;
    MatchConfig b = SelectMode(c, Format::FreeForAll, true);
    CHECK(IsBrawl(b) && b.gear == Gear::Fists && b.tier == kBrawlTier && b.scoreLimit == DefaultScoreLimit(Format::FreeForAll) && b.timeLimitS == 600);
    CHECK(Sanitize(b).gear == Gear::Fists && Sanitize(b).tier == kBrawlTier);
    b.tier = 6; CHECK(Sanitize(b).tier == kBrawlTier); b.tier = kBrawlTier;   // Brawl's tier is fixed
    b.scoreLimit = 20;
    CHECK(SelectMode(b, Format::FreeForAll, true).scoreLimit == 20);          // the card again changes nothing
    MatchConfig back = SelectMode(b, Format::FreeForAll, false);              // the free-for-all card: same format, other rules
    CHECK(!IsBrawl(back) && back.format == Format::FreeForAll && back.gear == Gear::Own && back.tier == MatchConfig{}.tier && back.scoreLimit == 5);
    MatchConfig duel = SelectMode(b, Format::Duel, false);
    CHECK(duel.format == Format::Duel && duel.gear == Gear::Own && duel.tier == MatchConfig{}.tier && duel.scoreLimit == DefaultScoreLimit(Format::Duel));
    CHECK(SelectMode(c, Format::TwoTeams, false).scoreLimit == 30 && SelectMode(c, Format::TwoTeams, false).gear == Gear::Mirror);   // unchanged
    CHECK(SelectMode(c, Format::Duel, false).gear == Gear::Mirror);                                                                    // as SelectFormat
    // The Gear row never lands on Fists, whichever way it is stepped.
    MatchConfig g; g.gear = Gear::Mirror;
    CHECK(StepSetting(g, LobbyRow::Gear, 1).gear == Gear::Own && StepSetting(StepSetting(g, LobbyRow::Gear, -1), LobbyRow::Gear, -1).gear == Gear::Own);
    g.gear = Gear::Fists;
    CHECK(StepSetting(g, LobbyRow::Gear, 1).gear == Gear::RandomTier && StepSetting(g, LobbyRow::Gear, -1).gear == Gear::Mirror);
}

static size_t IndexOf(const char* display) { for (size_t i = 0; i < MapCount(); ++i) if (std::string(kMaps[i].display) == display) return i; return MapCount(); }

static const MapCard* Card(const LobbyScene& s, size_t index) { for (const MapCard& m : s.maps) if (m.index == index) return &m; return nullptr; }

static void TestMaps() {
    LobbyLocal l = InLobby(true, Format::FreeForAll);
    l.mapPick = "/Game/Maps/Arenas/Map_Arena_Yard";
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.maps.size() == PickableCount());                    // Cutting, Tavern and Smithery are not offered
    size_t yard = IndexOf("Yard"), tavern = IndexOf("Tavern"), abyss = IndexOf("Abyss"), alley = IndexOf("Alley");
    CHECK(yard < MapCount() && tavern < MapCount() && abyss < MapCount() && alley < MapCount());
    if (s.maps.size() != PickableCount() || yard >= MapCount() || tavern >= MapCount() || abyss >= MapCount() || alley >= MapCount()) return;
    CHECK(!Card(s, tavern) && !Card(s, IndexOf("Cutting")) && !Card(s, IndexOf("Smithery")) && Card(s, abyss));
    for (const MapCard& m : s.maps) {
        CHECK(m.name == kMaps[m.index].display);
        CHECK(m.selected == (m.index == yard));
        CHECK(m.allowed == (m.index != abyss && m.index != IndexOf("Forest")));   // the Abyss is Zombies only; Forest is Capture the flag only
    }
    CHECK(Card(s, yard)->tag == "Arena");
    CHECK(Card(s, abyss)->tag == "Large level " + kDot + " no spawn points: ring spawn");

    l.config = SelectFormat(l.config, Format::Duel);
    l.mapPick = "/Game/Maps/Map_Hub_Tavern_Frank";              // not pickable: the first arena instead
    s = BuildLobbyScene(l);
    CHECK(Card(s, alley)->selected);
    CHECK(!Card(s, abyss)->allowed && Card(s, abyss)->reason == "Duel needs an arena" && !Card(s, abyss)->enabled);
    CHECK(Card(s, yard)->allowed && Card(s, yard)->enabled && Card(s, yard)->reason.empty());

    l.config = SelectFormat(l.config, Format::Zombies);
    s = BuildLobbyScene(l);
    for (const MapCard& m : s.maps) {
        CHECK(m.allowed == (m.index == abyss) && m.selected == (m.index == abyss));
        if (m.index != abyss) CHECK(m.reason == (m.index == IndexOf("Forest") ? "Capture the flag only" : "Zombies needs the Abyss"));
    }
    CHECK(std::string(MapRefusal(Format::FreeForAll)) == "Not for this mode");
}

// Item 8: another player on a different mod build is named, and the host cannot start until it matches or the
// override is on. Builds not known yet say nothing.
static void TestBuildMismatch() {
    LobbyLocal h = InLobby(true);
    h.myBuild = "0.6.4";
    h.players[1].build = "0.6.4";
    LobbyScene s = BuildLobbyScene(h);
    CHECK(s.start && !s.buildMismatch && s.waiting.empty());
    h.players[1].build = "";                          // Bob's member data has not come yet
    s = BuildLobbyScene(h);
    CHECK(s.start && !s.buildMismatch);
    h.players[1].build = "0.6.3";
    s = BuildLobbyScene(h);
    CHECK(!s.start && s.buildMismatch && s.waiting == "Bob on 0.6.3 - update");
    h.players[0].build = "9.9.9";                     // my own row never counts against me
    CHECK(BuildLobbyScene(h).waiting == "Bob on 0.6.3 - update");
    h.allowBuildMismatch = true;
    s = BuildLobbyScene(h);
    CHECK(s.start && s.buildMismatch && s.waiting == "Bob on 0.6.3 - update (allowed: allow_build_mismatch=1)");
    LobbyLocal m = InLobby(false);                    // a member sees the host's build named instead of "Waiting"
    m.myBuild = "0.6.3";
    m.players[0].build = "0.6.4";
    s = BuildLobbyScene(m);
    CHECK(!s.start && s.buildMismatch && s.waiting == "Ann on 0.6.4 - update");
    m.players[0].build = "0.6.3";
    CHECK(BuildLobbyScene(m).waiting == "Waiting for the host");
}

static void TestDamage() {
    DamageRange r;                                         // until the slider is read: 0..200%, stock 100
    MatchConfig c;
    CHECK(c.damagePct == kDamageStock && Sanitize(c).damagePct == kDamageStock);
    c.damagePct = 5000;
    CHECK(Sanitize(c).damagePct == kDamageMaxPct);
    c.damagePct = kDamageStock;
    CHECK(StepSetting(c, LobbyRow::Damage, 1, r).damagePct == 110 && StepSetting(c, LobbyRow::Damage, -1, r).damagePct == 90);
    c.damagePct = 110;
    CHECK(StepSetting(c, LobbyRow::Damage, -1, r).damagePct == kDamageStock);   // back on the stock value: stored as stock
    c.damagePct = 200;
    CHECK(StepSetting(c, LobbyRow::Damage, 1, r).damagePct == 200);              // the slider's top: no wrap
    c.damagePct = 0;
    CHECK(StepSetting(c, LobbyRow::Damage, -1, r).damagePct == 0);
    MatchConfig only = StepSetting(MatchConfig{}, LobbyRow::Damage, 1, r);
    CHECK(only.tier == MatchConfig{}.tier && only.scoreLimit == MatchConfig{}.scoreLimit);   // a step changes damage alone

    LobbyLocal l = InLobby(true, Format::FreeForAll);
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Damage).label == "Player damage");
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Damage).value == "100% (stock)" && Need(BuildLobbyScene(l), LobbyRow::Damage).enabled);
    l.config.damagePct = 150;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Damage).value == "150%");
    LobbyLocal m = InLobby(false, Format::FreeForAll);
    CHECK(!Need(BuildLobbyScene(m), LobbyRow::Damage).enabled);                 // the host's alone
    LobbyLocal prog = InLobby(true, Format::FreeForAll); prog.progression = true;
    CHECK(Ids(BuildLobbyScene(prog)) == (std::vector<LobbyRow>{LobbyRow::Damage}));   // co-op takes damage too
}

static void TestModdedMaps() {
    LobbyLocal l = InLobby(true, Format::FreeForAll);
    l.modMaps = {{"/Game/Maps/Custom/Arena_Bog", "Arena Bog", "BogMap_P.pak"}, {"/Game/Maps/Custom/Plain", "Plain", ""}};
    l.mapPick = "/Game/Maps/Arenas/Map_Arena_Yard";
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.maps.size() == PickableCount() + 2);
    if (s.maps.size() != PickableCount() + 2) return;
    const MapCard& bog = s.maps[PickableCount()];
    const MapCard& plain = s.maps[PickableCount() + 1];
    CHECK(bog.modded && bog.index == MapCount() && bog.path == "/Game/Maps/Custom/Arena_Bog" && bog.name == "Arena Bog");
    CHECK(bog.tag == "Modded map \xC2\xB7 BogMap_P.pak" && plain.tag == "Modded map" && plain.index == MapCount() + 1);
    CHECK(bog.allowed && bog.enabled && !bog.selected && !plain.selected);
    size_t selectedStock = 0;
    for (const MapCard& m : s.maps) if (!m.modded && m.selected) ++selectedStock;
    CHECK(selectedStock == 1);                                     // the stock pick stays marked while no custom map is picked
    // Picking a custom map: it alone is selected (the object-name form of the pick counts), and START waits for the hash.
    l.mapPick = "/game/maps/custom/arena_bog.Arena_Bog";
    s = BuildLobbyScene(l);
    CHECK(s.maps[PickableCount()].selected && !s.maps[PickableCount() + 1].selected);
    selectedStock = 0;
    for (const MapCard& m : s.maps) if (!m.modded && m.selected) ++selectedStock;
    CHECK(selectedStock == 0);
    CHECK(s.start);
    l.modPickPending = true; s = BuildLobbyScene(l);
    CHECK(!s.start && s.waiting == "Checking the map pack");
    l.mapPick = "/Game/Maps/Arenas/Map_Arena_Yard"; s = BuildLobbyScene(l);   // a stock pick ignores the flag
    CHECK(s.start);
    // A member sees them, disabled; Zombies refuses them (the Abyss is its map); Progression shows no maps.
    LobbyLocal m = InLobby(false); m.modMaps = l.modMaps; m.mapPick = "/Game/Maps/Custom/Plain";
    s = BuildLobbyScene(m);
    CHECK(s.maps.size() == PickableCount() + 2 && !s.maps[PickableCount()].enabled && s.maps[PickableCount() + 1].selected);
    LobbyLocal z = InLobby(true, Format::Zombies); z.modMaps = l.modMaps; z.mapPick = "/Game/Maps/Custom/Plain";
    s = BuildLobbyScene(z);
    CHECK(s.maps.size() == PickableCount() + 2 && !s.maps[PickableCount()].allowed && !s.maps[PickableCount()].enabled &&
          !s.maps[PickableCount()].reason.empty() && !s.maps[PickableCount() + 1].selected);
    LobbyLocal pr = InLobby(true); pr.modMaps = l.modMaps; pr.progression = true;
    CHECK(BuildLobbyScene(pr).maps.empty());
}

static void TestGroupsAndSections() {
    LobbyScene s = BuildLobbyScene(InLobby(true));
    CHECK(s.modes.size() == kModeCardCount);
    for (size_t i = 0; i < s.modes.size(); ++i) CHECK(s.modes[i].group == ModeGroupAt(i));
    for (const ModeCard& c : s.modes) {
        const bool coop = c.progression || (!c.brawl && IsBotFormat(c.format));
        CHECK((c.group == ModeGroup::Coop) == coop);
    }
    CHECK(ModeGroupAt(0) == ModeGroup::Versus && ModeGroupAt(kModeOrderCount) == ModeGroup::Versus &&
          ModeGroupAt(kModeOrderCount + 1) == ModeGroup::Coop);
    for (Format f : {Format::Duel, Format::LastManStanding, Format::TwoTeams}) {         // Duel, Last Man Standing and Teams are versus modes
        for (size_t i = 0; i < kModeOrderCount; ++i) if (kModeOrder[i] == f) CHECK(ModeGroupAt(i) == ModeGroup::Versus);
    }
    CHECK(SectionOf(LobbyRow::Limit) == LobbySection::Match && SectionOf(LobbyRow::Damage) == LobbySection::Match);
    CHECK(SectionOf(LobbyRow::MyTeam) == LobbySection::Teams && SectionOf(LobbyRow::FriendlyFire) == LobbySection::Teams && SectionOf(LobbyRow::Teams) == LobbySection::Teams);
    CHECK(SectionOf(LobbyRow::Gear) == LobbySection::Gear && SectionOf(LobbyRow::Tier) == LobbySection::Gear);
    CHECK(SectionOf(LobbyRow::ClearGround) == LobbySection::World && SectionOf(LobbyRow::Sync) == LobbySection::World);
    CHECK(std::string(SectionName(LobbySection::Match)) == "MATCH" && std::string(SectionName(LobbySection::World)) == "WORLD");
    for (size_t i = 0; i <= size_t(LobbyRow::Sync); ++i) CHECK(SectionOf(LobbyRow(i)) < LobbySection::Count);
}

// The class row is for lobbies where the class counts, and for players in one: the page never has to open on the classes.
static void TestClassLine() {
    LobbyLocal l = InLobby(false, Format::FreeForAll, Gear::Own);
    CHECK(BuildLobbyScene(l).classLine.empty());
    l.myClass = "Crusader";
    CHECK(BuildLobbyScene(l).classLine == "YOUR CLASS: Crusader");
    l.phase = LobbyPhase::Idle;
    CHECK(BuildLobbyScene(l).classLine.empty());
}

static void TestSummary() {
    LobbyScene s = BuildLobbyScene(InLobby(true, Format::TwoTeams, Gear::RandomTier));
    CHECK(s.summary.rfind("TEAMS (2) " + kDot + " ", 0) == 0);
    CHECK(BuildLobbyScene(Teamed(true, 4)).summary.rfind("TEAMS (4) " + kDot + " ", 0) == 0);
    CHECK(s.summary.find("Random by tier, tier ") != std::string::npos);
    LobbyScene own = BuildLobbyScene(InLobby(true));
    CHECK(own.summary.find("Own loadouts") != std::string::npos && own.summary.find("tier") == std::string::npos);
    LobbyScene brawl = BuildLobbyScene(InLobby(true, Format::FreeForAll, Gear::Fists));
    CHECK(brawl.summary.rfind("BRAWL", 0) == 0 && brawl.summary.find("loadouts") == std::string::npos);
    LobbyLocal pr = InLobby(false); pr.progression = true;
    CHECK(BuildLobbyScene(pr).summary.rfind("PROGRESSION", 0) == 0);
    LobbyLocal idle; idle.phase = LobbyPhase::Idle;
    CHECK(BuildLobbyScene(idle).summary.empty());
}

static void TestFilters() {
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0";
    LobbyFound duel; duel.name = "D"; duel.mode = "Duel"; duel.players = 2; duel.compatible = true;
    LobbyFound full; full.name = "F"; full.mode = "Free-for-all"; full.players = 8; full.compatible = true;
    LobbyFound old; old.name = "O"; old.mode = "Duel"; old.players = 5; old.compatible = false;
    l.found = {duel, full, old};
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.filters.size() == kFilterChips && s.browser.size() == 3 && s.browserCount == "3 lobbies");
    for (const FilterChip& c : s.filters) CHECK(!c.on);
    CHECK(s.filters[0].label == "Any mode");
    // The mode chip steps through every mode and wraps to any.
    BrowserFilter f;
    for (size_t i = 0; i < BrowserModes().size(); ++i) f = ToggleFilter(f, 0);
    CHECK(f.mode == 0);
    f = ToggleFilter(f, 0); f = ToggleFilter(f, 0);
    CHECK(BrowserModes()[f.mode] == "Duel");
    l.filter = f;
    s = BuildLobbyScene(l);
    CHECK(s.filters[0].on && s.filters[0].label == "Duel" && s.browser.size() == 2 && s.browserCount == "2 of 3 lobbies shown");
    l.filter = ToggleFilter(l.filter, 2);                 // compatible only
    s = BuildLobbyScene(l);
    CHECK(s.filters[2].on && s.browser.size() == 1 && s.browser[0].name == "D");
    l.filter = {};
    l.filter = ToggleFilter(l.filter, 1);                 // has room
    s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 2 && s.browser[0].name == "D" && s.browser[1].name == "O");
    l.filter = {};
    l.filter = ToggleFilter(l.filter, 3);                 // most players first
    s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 3 && s.browser[0].name == "F" && s.browser[1].name == "O" && s.browser[2].name == "D");
    CHECK(s.browser[0].source == 1 && s.browser[1].source == 2 && s.browser[2].source == 0);   // what JOIN joins
    CHECK(s.browserEmpty.empty());
    CHECK(s.browserCount == "3 lobbies");
    l.filter = ToggleFilter(l.filter, 3);
    CHECK(!l.filter.mostPlayers && ToggleFilter(l.filter, 9).mode == l.filter.mode);
    // Every browser mode is a name a host can publish.
    for (Format m : kModeOrder) {
        bool listed = false;
        for (const std::string& n : BrowserModes()) listed = listed || n == ModeName(m, Gear::Own);
        CHECK(listed);
    }
    l.filter = {}; l.filter.mode = uint8_t(std::find(BrowserModes().begin(), BrowserModes().end(), "Zombies") - BrowserModes().begin());   // Zombies: none listed
    s = BuildLobbyScene(l);
    CHECK(s.browser.empty() && s.browserEmpty == "No lobby matches these filters." && s.browserCount == "0 of 3 lobbies shown");
    l.found.clear(); l.filter = {};
    s = BuildLobbyScene(l);
    CHECK(s.browserCount.empty() && !s.browserEmpty.empty() && s.browserEmpty != "No lobby matches these filters.");
    LobbyLocal in = InLobby(true);
    CHECK(BuildLobbyScene(in).filters.empty());
}

// Teams 2..4: one Teams card with a Teams row (2, 3, 4 teams), the stepper, the value, and the browser filter.
static void TestTeamsRow() {
    MatchConfig c; c.format = Format::TwoTeams;
    CHECK(c.teams == 2);
    CHECK(StepSetting(c, LobbyRow::Teams, 1).teams == 3 && StepSetting(StepSetting(c, LobbyRow::Teams, 1), LobbyRow::Teams, 1).teams == 4);
    c.teams = 4; CHECK(StepSetting(c, LobbyRow::Teams, 1).teams == 2 && StepSetting(c, LobbyRow::Teams, -1).teams == 3);   // wraps
    c.teams = 2; CHECK(StepSetting(c, LobbyRow::Teams, -1).teams == 4);
    CHECK(StepSetting(c, LobbyRow::Teams, 1).format == Format::TwoTeams && StepSetting(c, LobbyRow::Teams, 1).scoreLimit == c.scoreLimit);
    LobbyLocal l = Teamed(true, 3);
    const LobbyScene ts = BuildLobbyScene(l);
    const SettingRow& row = Need(ts, LobbyRow::Teams);
    CHECK(row.label == "Teams" && row.value == "3 teams" && row.enabled && !row.hint.empty());
    CHECK(Need(BuildLobbyScene(Teamed(true, 4)), LobbyRow::Teams).value == "4 teams" && Need(BuildLobbyScene(Teamed(true, 2)), LobbyRow::Teams).value == "2 teams");
    CHECK(!Need(BuildLobbyScene(Teamed(false, 3)), LobbyRow::Teams).enabled);                  // members see it, the host sets it
    for (Format f : {Format::FreeForAll, Format::Duel, Format::LastManStanding, Format::PlayersVsBots, Format::Zombies})
        CHECK(RowOf(BuildLobbyScene(InLobby(true, f)), LobbyRow::Teams) == nullptr);            // only the Teams format has the row
    // The roster shows four teams.
    LobbyLocal r = Teamed(true, 4);
    r.players = {Player("A", true, true, 3), Player("B", false, false, kNoTeam, 3), Player("C", false, false, 1)};
    LobbyScene s = BuildLobbyScene(r);
    CHECK(s.roster.size() == 3 && s.roster[0].team == "Gold" && s.roster[1].team == "Gold" && s.roster[1].moved && s.roster[2].team == "Blue");
    r.config.teams = 3;
    s = BuildLobbyScene(r);
    CHECK(s.roster[0].team == "Auto" && !s.roster[1].moved);
    // One Teams card, selected for the Teams format at any count; no ThreeTeams card.
    for (int t = 2; t <= 4; ++t) {
        int cards = 0, lit = 0;
        for (const ModeCard& m : BuildLobbyScene(Teamed(true, t)).modes) { cards += m.format == Format::TwoTeams || m.format == Format::ThreeTeams; lit += m.selected; }
        CHECK(cards == 1 && lit == 1);
    }
    CHECK(FormatName(Format::TwoTeams) == "Teams" && ModeName(Format::TwoTeams, Gear::Own, 3) == "Teams (3)" && ModeName(Format::TwoTeams, Gear::Own) == "Teams");
    CHECK(ModeName(uint8_t(Format::TwoTeams), uint8_t(Gear::Own), 4) == "Teams (4)" && ModeName(Format::Duel, Gear::Own, 4) == "Duel");
    CHECK(ModeName(Format::LastManStanding, Gear::Own) == "Last man standing");
    // The browser: one "Teams" entry matches "Teams (2)", "Teams (3)" and "Teams (4)", and nothing else.
    const auto& modes = BrowserModes();
    size_t at = 0;
    for (size_t i = 0; i < modes.size(); ++i) if (modes[i] == "Teams") at = i;
    CHECK(at != 0 && std::find(modes.begin(), modes.end(), "2 teams") == modes.end() && std::find(modes.begin(), modes.end(), "3 teams") == modes.end());
    BrowserFilter f; f.mode = uint8_t(at);
    LobbyFound x; x.compatible = true;
    for (const char* m : {"Teams", "Teams (2)", "Teams (3)", "Teams (4)"}) { x.mode = m; CHECK(PassesFilter(x, f)); }
    for (const char* m : {"Duel", "Teams of", "Teamsx (3)", "teams (3)", "", "Last man standing"}) { x.mode = m; CHECK(!PassesFilter(x, f)); }
    f.mode = uint8_t(std::find(modes.begin(), modes.end(), "Last man standing") - modes.begin());
    x.mode = "Last man standing"; CHECK(PassesFilter(x, f));
    x.mode = "Duel"; CHECK(!PassesFilter(x, f));
}

// The Team colours row (Wave 2): Tabard / Full kit / Off, default Full kit, in the TEAMS section, for the Teams format only.
static void TestTeamKitRow() {
    MatchConfig c; c.format = Format::TwoTeams;
    CHECK(c.teamKit == TeamKit::Full);
    CHECK(StepSetting(c, LobbyRow::TeamKit, 1).teamKit == TeamKit::Off && StepSetting(c, LobbyRow::TeamKit, -1).teamKit == TeamKit::Tabard);
    c.teamKit = TeamKit::Off;    CHECK(StepSetting(c, LobbyRow::TeamKit, 1).teamKit == TeamKit::Tabard && StepSetting(c, LobbyRow::TeamKit, -1).teamKit == TeamKit::Full);
    c.teamKit = TeamKit::Tabard; CHECK(StepSetting(c, LobbyRow::TeamKit, 1).teamKit == TeamKit::Full && StepSetting(c, LobbyRow::TeamKit, -1).teamKit == TeamKit::Off);
    c.teamKit = TeamKit(9);      CHECK(StepSetting(c, LobbyRow::TeamKit, 1).teamKit == TeamKit::Off);      // out of range steps from Full
    c = MatchConfig{}; c.format = Format::TwoTeams;
    const MatchConfig only = StepSetting(c, LobbyRow::TeamKit, 1);
    CHECK(only.teams == c.teams && only.friendlyFire == c.friendlyFire && only.tier == c.tier && only.scoreLimit == c.scoreLimit && only.damagePct == c.damagePct);
    LobbyLocal l = Teamed(true, 2);
    const LobbyScene scene = BuildLobbyScene(l);
    const SettingRow& r = Need(scene, LobbyRow::TeamKit);
    CHECK(r.label == "Team colours" && r.value == "Full kit" && r.enabled && !r.hint.empty() && SectionOf(LobbyRow::TeamKit) == LobbySection::Teams);
    l.config.teamKit = TeamKit::Tabard; CHECK(Need(BuildLobbyScene(l), LobbyRow::TeamKit).value == "Tabard");
    l.config.teamKit = TeamKit::Off;    CHECK(Need(BuildLobbyScene(l), LobbyRow::TeamKit).value == "Off");
    CHECK(!Need(BuildLobbyScene(Teamed(false, 2)), LobbyRow::TeamKit).enabled);       // members see it, the host sets it
    for (Format f : {Format::FreeForAll, Format::Duel, Format::LastManStanding, Format::PlayersVsBots, Format::Zombies})
        CHECK(RowOf(BuildLobbyScene(InLobby(true, f)), LobbyRow::TeamKit) == nullptr);   // team formats only
}

// Duel is for 2 players, Last Man Standing for 2 or more (the host's count).
static void TestModeAvailability() {
    for (int n : {0, 1, 3, 4, 8}) {
        CHECK(!ModeAvailable(Format::Duel, n).ok && std::string(ModeAvailable(Format::Duel, n).hint) == "Duel is for 2 players");
    }
    CHECK(ModeAvailable(Format::Duel, 2).ok && std::string(ModeAvailable(Format::Duel, 2).hint).empty());
    for (int n : {0, 1}) CHECK(!ModeAvailable(Format::LastManStanding, n).ok && std::string(ModeAvailable(Format::LastManStanding, n).hint) == "Needs 2 or more players");
    for (int n : {2, 3, 8}) CHECK(ModeAvailable(Format::LastManStanding, n).ok);
    for (Format f : {Format::FreeForAll, Format::TwoTeams, Format::PlayersVsBots, Format::Zombies})
        for (int n : {0, 1, 2, 5}) CHECK(ModeAvailable(f, n).ok);

    auto card = [](const LobbyScene& s, Format f) { for (const ModeCard& m : s.modes) if (m.format == f && !m.brawl && !m.progression) return m; return ModeCard{}; };
    auto lobby = [](bool host, int n, Format f) {
        LobbyLocal l = InLobby(host, f);
        l.players.clear();
        for (int i = 0; i < n; ++i) l.players.push_back(Player(i ? "P" : "Ann", i == 0, i == (host ? 0 : n - 1)));
        return l;
    };
    LobbyScene one = BuildLobbyScene(lobby(true, 1, Format::FreeForAll));
    CHECK(!card(one, Format::Duel).enabled && card(one, Format::Duel).hint == "Duel is for 2 players");
    CHECK(!card(one, Format::LastManStanding).enabled && card(one, Format::LastManStanding).hint == "Needs 2 or more players");
    CHECK(card(one, Format::FreeForAll).enabled && card(one, Format::FreeForAll).hint.empty() && card(one, Format::TwoTeams).enabled);
    LobbyScene two = BuildLobbyScene(lobby(true, 2, Format::FreeForAll));
    CHECK(card(two, Format::Duel).enabled && card(two, Format::Duel).hint.empty() && card(two, Format::LastManStanding).enabled);
    LobbyScene three = BuildLobbyScene(lobby(true, 3, Format::FreeForAll));
    CHECK(!card(three, Format::Duel).enabled && card(three, Format::Duel).hint == "Duel is for 2 players" && card(three, Format::LastManStanding).enabled);
    LobbyScene member = BuildLobbyScene(lobby(false, 2, Format::FreeForAll));
    CHECK(!card(member, Format::Duel).enabled && card(member, Format::Duel).hint.empty());     // a member's cards are off, with no hint to read into
    // The server seat is no player: playerCount overrides the roster's size.
    LobbyLocal srv = lobby(true, 3, Format::FreeForAll); srv.playerCount = 2;
    CHECK(card(BuildLobbyScene(srv), Format::Duel).enabled);
    srv.playerCount = 1;
    CHECK(!card(BuildLobbyScene(srv), Format::LastManStanding).enabled);
    // START: Duel needs exactly two, Last Man Standing two or more; the waiting line says why.
    LobbyScene d1 = BuildLobbyScene(lobby(true, 1, Format::Duel));
    CHECK(!d1.start && d1.waiting == "Duel is for 2 players" && d1.modes[1].selected);
    CHECK(BuildLobbyScene(lobby(true, 2, Format::Duel)).start && BuildLobbyScene(lobby(true, 2, Format::Duel)).waiting.empty());
    CHECK(!BuildLobbyScene(lobby(true, 3, Format::Duel)).start);
    LobbyScene l1 = BuildLobbyScene(lobby(true, 1, Format::LastManStanding));
    CHECK(!l1.start && l1.waiting == "Needs 2 or more players");
    CHECK(BuildLobbyScene(lobby(true, 3, Format::LastManStanding)).start && BuildLobbyScene(lobby(true, 4, Format::LastManStanding)).start);
    CHECK(BuildLobbyScene(lobby(true, 1, Format::FreeForAll)).start);                          // alone in free-for-all: as before
    LobbyLocal prog = lobby(true, 1, Format::Duel); prog.progression = true;
    CHECK(BuildLobbyScene(prog).start);                                                         // Progression ignores the mode
    CHECK(BuildLobbyScene(lobby(false, 1, Format::Duel)).waiting == "Waiting for the host");   // a member waits for the host, whatever the mode
}

// A third player joins a lobby set to Duel: Last Man Standing, with the round count and the gear rows kept.
static void TestDuelOutgrown() {
    MatchConfig d; d.format = Format::Duel; d.scoreLimit = 5; d.gear = Gear::Mirror; d.tier = 4; d.timeLimitS = 300; d.respawnDelayS = 10; d.damagePct = 150;
    for (int n : {0, 1, 2}) CHECK(!DuelOutgrown(d, n) && AdjustForPlayers(d, n).format == Format::Duel);
    for (int n : {3, 4, 8}) {
        CHECK(DuelOutgrown(d, n));
        const MatchConfig a = AdjustForPlayers(d, n);
        CHECK(a.format == Format::LastManStanding && a.scoreLimit == 5 && a.gear == Gear::Mirror && a.tier == 4 && a.timeLimitS == 300 && a.respawnDelayS == 10 && a.damagePct == 150);
    }
    for (Format f : {Format::FreeForAll, Format::TwoTeams, Format::LastManStanding, Format::PlayersVsBots, Format::Zombies}) {
        MatchConfig o; o.format = f;
        CHECK(!DuelOutgrown(o, 5) && AdjustForPlayers(o, 5).format == f);
    }
    CHECK(std::string(kDuelOutgrownLine) == "Duel is for 2 players: switched to Last man standing");
    // A lobby back down to two players stays on Last Man Standing (the host picks Duel again).
    MatchConfig l = AdjustForPlayers(d, 3);
    CHECK(AdjustForPlayers(l, 2).format == Format::LastManStanding);
}


// Capture the flag: the card, two players at least, two fixed teams, the captures and flag-return rows, the maps.
static void TestCaptureTheFlag() {
    CHECK(uint8_t(Format::CaptureTheFlag) == 10 && kFormatCount == 16 && FormatName(Format::CaptureTheFlag) == "Capture the flag");
    CHECK(ModeName(Format::CaptureTheFlag, Gear::Own, 2) == "Capture the flag");     // the team count is not part of its name
    for (int n : {0, 1}) CHECK(!ModeAvailable(Format::CaptureTheFlag, n).ok && std::string(ModeAvailable(Format::CaptureTheFlag, n).hint) == "Needs 2 or more players");
    for (int n : {2, 3, 8}) CHECK(ModeAvailable(Format::CaptureTheFlag, n).ok);
    LobbyLocal h = InLobby(true, Format::CaptureTheFlag);
    LobbyScene s = BuildLobbyScene(h);
    const ModeCard* card = nullptr;
    for (const ModeCard& m : s.modes) if (m.format == Format::CaptureTheFlag) card = &m;
    CHECK(card && card->name == "CAPTURE THE FLAG" && card->selected && card->enabled && card->group == ModeGroup::Versus && !card->brawl && !card->progression);
    CHECK(card && card->description == FormatBlurb(Format::CaptureTheFlag) && card->description.find("flag") != std::string::npos);
    int cards = 0; for (const ModeCard& m : s.modes) cards += m.format == Format::CaptureTheFlag ? 1 : 0;
    CHECK(cards == 1);
    LobbyLocal alone = h; alone.players = {Player("Ann", true, true)};
    s = BuildLobbyScene(alone);
    for (const ModeCard& m : s.modes) if (m.format == Format::CaptureTheFlag) CHECK(!m.enabled && m.hint == "Needs 2 or more players");
    CHECK(!s.start && s.waiting == "Needs 2 or more players");
    // The rows: captures and the flag return time lead; no Limit, no Teams row (two fixed teams).
    s = BuildLobbyScene(h);
    const std::vector<LobbyRow> ids = Ids(s);
    CHECK(ids.size() >= 4 && ids[0] == LobbyRow::CtfCaptures && ids[1] == LobbyRow::FlagReturn && ids[2] == LobbyRow::Time);
    CHECK(!RowOf(s, LobbyRow::Limit) && !RowOf(s, LobbyRow::Teams) && RowOf(s, LobbyRow::FriendlyFire) && RowOf(s, LobbyRow::MyTeam) && RowOf(s, LobbyRow::TeamKit));
    const SettingRow& caps = Need(s, LobbyRow::CtfCaptures);
    CHECK(caps.label == "Captures to win" && caps.value == "3" && caps.enabled && SectionOf(LobbyRow::CtfCaptures) == LobbySection::Match);
    const SettingRow& ret = Need(s, LobbyRow::FlagReturn);
    CHECK(ret.label == "Flag return time" && ret.value == "20 s" && ret.enabled && SectionOf(LobbyRow::FlagReturn) == LobbySection::Match);
    { LobbyLocal timed = h; timed.config.timeLimitS = 300; CHECK(Need(BuildLobbyScene(timed), LobbyRow::Time).hint.find("sudden death") != std::string::npos); }
    CHECK(!BuildLobbyScene(InLobby(false, Format::CaptureTheFlag)).settings[0].enabled);   // members read
    // Steppers: 1..10, wrapping; 10..60 s.
    MatchConfig c; c.format = Format::CaptureTheFlag;
    CHECK(c.ctfCaptures == 3 && c.flagReturnS == 20);
    CHECK(StepSetting(c, LobbyRow::CtfCaptures, 1).ctfCaptures == 4 && StepSetting(c, LobbyRow::CtfCaptures, -1).ctfCaptures == 2);
    c.ctfCaptures = 10; CHECK(StepSetting(c, LobbyRow::CtfCaptures, 1).ctfCaptures == 1);
    c.ctfCaptures = 1; CHECK(StepSetting(c, LobbyRow::CtfCaptures, -1).ctfCaptures == 10);
    c.flagReturnS = 60; CHECK(StepSetting(c, LobbyRow::FlagReturn, 1).flagReturnS == 10);
    c.flagReturnS = 10; CHECK(StepSetting(c, LobbyRow::FlagReturn, -1).flagReturnS == 60 && StepSetting(c, LobbyRow::FlagReturn, 1).flagReturnS == 15);
    for (int v : kCtfCaptureOptions) CHECK(v >= kCtfMinCaptures && v <= kCtfMaxCaptures);
    for (int v : kFlagReturnOptions) CHECK(v >= kFlagReturnMinS && v <= kFlagReturnMaxS);
    // Selecting the card: the format's default limit; two fixed teams.
    MatchConfig sel = SelectMode(MatchConfig{}, Format::CaptureTheFlag, false);
    CHECK(sel.format == Format::CaptureTheFlag && sel.scoreLimit == 3 && DefaultScoreLimit(Format::CaptureTheFlag) == 3);
    CHECK(Sanitize(sel).teams == 2 && TeamCount(Format::CaptureTheFlag, 4) == 2 && PicksTeams(Format::CaptureTheFlag) && HasTeams(Format::CaptureTheFlag) && IsCtf(Format::CaptureTheFlag));
    CHECK(!IsRoundFormat(Format::CaptureTheFlag) && !IsBotFormat(Format::CaptureTheFlag) && !ScoresPerFighter(Format::CaptureTheFlag));
    CHECK(Need(BuildLobbyScene(InLobby(true, Format::CaptureTheFlag)), LobbyRow::MyTeam).value == "Auto");
    LobbyLocal t = InLobby(true, Format::CaptureTheFlag);
    t.players[0].team = 1; t.players[1].team = 3;                    // Blue; a Gold pick means nothing in two teams
    s = BuildLobbyScene(t);
    CHECK(s.roster[0].team == "Blue" && s.roster[1].team == "Auto" && s.balance);
    // Maps: Alley and Forest are playable, the rest are refused; Forest is flagged experimental.
    LobbyLocal m = InLobby(true, Format::CaptureTheFlag);
    s = BuildLobbyScene(m);
    const size_t alley = IndexOf("Alley"), forest = IndexOf("Forest"), yard = IndexOf("Yard");
    CHECK(Card(s, alley) && Card(s, forest) && Card(s, yard) && Card(s, alley)->allowed && Card(s, forest)->allowed && !Card(s, yard)->allowed);
    CHECK(Card(s, forest)->tag.find("Experimental") == 0 && Card(s, yard)->reason == "Capture the flag: Alley or Forest only" && !Card(s, yard)->enabled);
    CHECK(Card(s, alley)->selected);                                  // no pick: the first map CTF allows
    m.mapPick = "/Game/Maps/Arenas/Map_Arena_Ambush_Test";
    s = BuildLobbyScene(m);
    CHECK(Card(s, forest)->selected && !Card(s, alley)->selected && s.summary == "CAPTURE THE FLAG " + kDot + " Forest " + kDot + " Own loadouts");
    m.mapPick = "/Game/Maps/Arenas/Map_Arena_Yard";                   // refused for CTF: falls back to Alley
    CHECK(Card(BuildLobbyScene(m), alley)->selected);
    // Forest is offered to no other format, and says so.
    for (Format f : {Format::FreeForAll, Format::TwoTeams, Format::Duel, Format::Riot}) {
        s = BuildLobbyScene(InLobby(true, f));
        CHECK(!Card(s, forest)->allowed && !Card(s, forest)->enabled && Card(s, forest)->reason == "Capture the flag only");
    }
    CHECK(std::string(MapRefusal(Format::CaptureTheFlag)) == "Capture the flag: Alley or Forest only");
    // The browser has a chip entry for the mode.
    bool listed = false; for (const std::string& n : BrowserModes()) listed = listed || n == "Capture the flag";
    CHECK(listed && ModeMatches("Capture the flag", "Capture the flag"));
    // Sanitize: captures and the return time are clamped; the limit follows the captures; other formats keep their limit.
    MatchConfig bad; bad.format = Format::CaptureTheFlag; bad.ctfCaptures = 200; bad.flagReturnS = 2; bad.scoreLimit = 77; bad.teams = 4;
    MatchConfig ok = Sanitize(bad);
    CHECK(ok.ctfCaptures == 10 && ok.flagReturnS == 10 && ok.scoreLimit == 10 && ok.teams == 2);
    bad.ctfCaptures = 0; bad.flagReturnS = 255; ok = Sanitize(bad);
    CHECK(ok.ctfCaptures == 1 && ok.flagReturnS == 60 && ok.scoreLimit == 1);
    MatchConfig ffa; ffa.scoreLimit = 7; ffa.ctfCaptures = 9; CHECK(Sanitize(ffa).scoreLimit == 7);
}

int main() {
    TestPrevNext();
    TestStepSetting();
    TestNewRows();
    TestWeaponsRow();
    TestCrossbowsRow();
    TestTeamPick();
    TestSelectFormat();
    TestPill();
    TestHeader();
    TestBrowser();
    TestPlayerLimit();
    TestBrowserPack();
    TestPasswordFilter();
    TestHostPassword();
    TestTeamsRow();
    TestTeamKitRow();
    TestModeAvailability();
    TestDuelOutgrown();
    TestOutOfLobby();
    TestHostAgainstMember();
    TestRowsPerFormat();
    TestLabelsAndValues();
    TestGearHint();
    TestOtherHints();
    TestMyTeam();
    TestRoster();
    TestReady();
    TestModes();
    TestBrawl();
    TestMaps();
    TestModdedMaps();
    TestBuildMismatch();
    TestDamage();
    TestGroupsAndSections();
    TestClassLine();
    TestSummary();
    TestFilters();
    TestCaptureTheFlag();
    if (g_fail) { std::printf("%d lobby scene checks failed\n", g_fail); return 1; }
    std::printf("all lobby scene tests passed\n");
    return 0;
}
