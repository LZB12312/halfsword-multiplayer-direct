// Lobby page (MATCH_ROUNDS_UI_CHAT_PLAN 3.4): what the multiplayer page shows, as data. A header with the state pill
// and the version badge; left, the lobby browser (not in a lobby) or the players (in one); middle, the mode cards and
// the setting rows with their steppers and one-line hints for the current format; right, the map cards and START
// MATCH. Members see everything the host chose, disabled, except their own team, and mark themselves ready (the
// host starts the match instead). The page (ui/menu.cpp) fills a LobbyLocal from the Steam lobby and maps the scene
// onto widgets; the steppers' arithmetic is here too.
#pragma once
#include "match/hsm_hud_scene.h"
#include "match/hsm_map_list.h"
#include "session/hsm_build.h"
#include "session/hsm_player_limit.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace hsm {

enum class LobbyPhase : uint8_t { Offline, Idle, Creating, Joining, InLobby };   // game::Lobby::State

struct LobbyPlayer {
    std::string name;
    bool host = false, ready = false, me = false;
    int team = kNoTeam;                 // their own pick (Auto: kNoTeam)
    int hostOverride = kNoTeam;         // where the host moved them
    std::string build;                  // their mod build (session/hsm_build.h), empty until their member data comes
};

// The public-lobby list's filters (the page's chips, in this order): mode, room, compatibility, sorting, passwords. All
// off: every lobby Steam returned, in its order.
struct BrowserFilter {
    uint8_t mode = 0;                   // 0: any; else the kBrowserModes entry shown
    bool hasRoom = false, compatibleOnly = false, mostPlayers = false;
    bool noPassword = false;            // hide the locked lobbies
};

// What a locked lobby's name starts with in the browser: plain Latin-1/ASCII, the game's font has no padlock glyph.
// The one place to change it.
inline constexpr const char* kLockedMarker = "[locked] ";

struct LobbyFound {                     // a public lobby from Find
    std::string name, map, mode;
    int players = 0;
    int maxPlayers = 8;                 // the host's player limit (lobby data hsm_max; 8 when the lobby has none)
    bool locked = false;                // it asks for a password (lobby data hsm_lock = "1"); the password itself is never listed
    bool compatible = false;
    std::string reason;                 // why not, when !compatible
    std::string pack;                   // "pack ArenaPack" (session/hsm_modpack.h PackRowText), with a mark when this machine lacks it; empty: no pack
};

struct LobbyModMap { std::string path, display, pak; };   // a custom map from a mod pak (session/hsm_modmaps.h); pak empty when unknown

struct LobbyLocal {
    LobbyPhase phase = LobbyPhase::Offline;
    bool host = false;                  // we own the lobby
    bool publicLobby = false;           // it is listed publicly (else friends only)
    bool locked = false;                // the lobby asks for a password (Lobby::Locked)
    std::string myName, status, version;
    std::string myBuild;                // this mod's build (ModBuildString)
    bool allowBuildMismatch = false;    // allow_build_mismatch=1: the host may start with other builds in
    MatchConfig config;
    DamageRange damage;                 // the game's own damage slider here (game::VanillaDamageRange)
    std::string mapPick;                // the host's pick (package path), empty until picked
    bool progression = false;           // the host chose the Progression card (LobbyData hsm_prog): the stock hub, no match
    std::vector<LobbyModMap> modMaps;   // the custom maps found on this machine (modded maps, after the stock ones)
    bool modPickPending = false;        // the pick is a modded map whose pak is still being hashed: START waits
    std::vector<LobbyPlayer> players;
    int playerCount = -1;               // the host's player count for the mode cards (game::Lobby::PlayerCount: a dedicated server's own seat is no player); -1: the roster's size
    std::vector<LobbyFound> found;
    BrowserFilter filter;
    int maxPlayers = 8;                 // in a lobby: the host's player limit (Lobby::MaxPlayers)
};

// Each section shows its rows in this order. Sync last: ui/menu.cpp makes one stepper per row up to it.
// Players is the lobby's own player limit, not a MatchConfig field: the scene offers it as LobbyScene::playerLimit (it is
// in no SettingsFor list) and the page applies it with Lobby::SetMaxPlayers. First, so it leads the MATCH section.
enum class LobbyRow : uint8_t { Players, Limit, Time, Respawn, FreeCam, Gear, Tier, Weapons, Teams, FriendlyFire, Damage, WaveBreak, ClearGround, MyTeam, TeamKit, Crossbows, CtfCaptures, FlagReturn, Fuse, Scenario, Sync };   // Sync last: the menu builds rows in this order

struct SettingRow { LobbyRow id = LobbyRow::Limit; std::string label, value, hint; bool enabled = false; };
// The mode cards' groups on the MODE tab (Party joins them with the party modes).
enum class ModeGroup : uint8_t { Versus, Coop };
struct ModeCard { Format format = Format::FreeForAll; std::string name, description; bool selected = false, enabled = false; bool progression = false; bool brawl = false;
                  ModeGroup group = ModeGroup::Versus; std::string hint; };   // progression: the extra last card; brawl: free-for-all with gear = Fists; hint: why a card is disabled for the player count (ModeAvailable)
struct MapCard { size_t index = 0; std::string name, tag, reason; bool allowed = false, selected = false, enabled = false;
                 bool modded = false; std::string path; };   // modded: index is MapCount() + the custom map's place, path its package path
struct BrowserRow { std::string name, players, detail; Span note; bool joinable = false; bool locked = false; size_t source = 0; };   // source: its index in LobbyLocal::found
struct RosterRow {
    std::string name, team;             // team: the formats with teams only
    Rgba swatch;                        // a == 0: none
    bool host = false, ready = false, moved = false, me = false, movable = false;
};

inline constexpr size_t kBrowserRows = 6;

// The RULES tab's sections, in page order; each row sits in one.
enum class LobbySection : uint8_t { Match, Teams, Gear, World, Count };
inline LobbySection SectionOf(LobbyRow r) {
    switch (r) {
        case LobbyRow::Teams: case LobbyRow::FriendlyFire: case LobbyRow::MyTeam: case LobbyRow::TeamKit: return LobbySection::Teams;
        case LobbyRow::CtfCaptures: case LobbyRow::FlagReturn: return LobbySection::Match;
        case LobbyRow::Gear: case LobbyRow::Tier: case LobbyRow::Weapons: case LobbyRow::Crossbows: return LobbySection::Gear;
        case LobbyRow::WaveBreak: case LobbyRow::ClearGround: case LobbyRow::Scenario: case LobbyRow::Sync: return LobbySection::World;
        default: return LobbySection::Match;
    }
}
inline const char* SectionName(LobbySection s) {
    switch (s) {
        case LobbySection::Teams: return "TEAMS";
        case LobbySection::Gear: return "GEAR";
        case LobbySection::World: return "WORLD";
        default: return "MATCH";
    }
}

// The browser's mode chip steps through these (0: any).
inline const std::vector<std::string>& BrowserModes() {
    static const std::vector<std::string> m = {"Any mode", "Free-for-all", "Duel", "Last man standing", "Teams", "Team duel", "Riot", "Juggernaut", "Capture the flag", "Tag", "Infection", "Crossbow TDM", "Tournament", "Pit bridge duels", "Brawl", "Survival", "Zombies"};
    return m;
}
inline constexpr size_t kFilterChips = 5;   // mode, has room, compatible only, most players first, no password
struct FilterChip { std::string label; bool on = false; };
inline std::vector<FilterChip> FilterChips(const BrowserFilter& f) {
    const auto& modes = BrowserModes();
    return {{modes[f.mode < modes.size() ? f.mode : 0], f.mode != 0}, {"Has room", f.hasRoom},
            {"Compatible only", f.compatibleOnly}, {"Most players first", f.mostPlayers},
            {"No password", f.noPassword}};
}
// A chip clicked: the mode steps on (wrapping to any), the others turn on or off.
inline BrowserFilter ToggleFilter(BrowserFilter f, size_t chip) {
    switch (chip) {
        case 0: f.mode = uint8_t((f.mode + 1) % BrowserModes().size()); break;
        case 1: f.hasRoom = !f.hasRoom; break;
        case 2: f.compatibleOnly = !f.compatibleOnly; break;
        case 3: f.mostPlayers = !f.mostPlayers; break;
        case 4: f.noPassword = !f.noPassword; break;
        default: break;
    }
    return f;
}
// A lobby's mode string against a browser entry: equal, or the entry plus the detail the host adds ("Teams (3)" is "Teams").
inline bool ModeMatches(const std::string& lobbyMode, const std::string& entry) {
    return lobbyMode == entry || (lobbyMode.size() > entry.size() && lobbyMode.compare(0, entry.size(), entry) == 0 && lobbyMode.compare(entry.size(), 2, " (") == 0);
}
inline bool PassesFilter(const LobbyFound& l, const BrowserFilter& f) {
    const auto& modes = BrowserModes();
    if (f.mode != 0 && f.mode < modes.size() && !ModeMatches(l.mode, modes[f.mode])) return false;
    if (f.hasRoom && LobbyIsFull(l.players, l.maxPlayers)) return false;   // each lobby's own limit
    if (f.compatibleOnly && !l.compatible) return false;
    if (f.noPassword && l.locked) return false;
    return true;
}
// The format cards: every format but ThreeTeams (the old value of the Teams card's 3 teams); then Brawl and Progression.
inline constexpr size_t kModeOrderCount = kFormatCount - 1;
inline constexpr size_t kModeCardCount = kModeOrderCount + 2;

struct LobbyScene {
    Span pill;
    std::string me, badge, status;      // status: not in a lobby only (in one, the pill says it)
    bool idle = false;                  // Host, Find, the browser and the idle hint
    std::vector<BrowserRow> browser;
    std::vector<FilterChip> filters;    // kFilterChips, while idle
    std::string browserEmpty;           // why the list is empty (nothing found, or the filters hide it all)
    std::string browserCount;           // "3 of 7 lobbies shown" (filters on), "7 lobbies", "" (none found)
    bool in = false;                    // the players, Invite and Leave, the modes, the settings, the maps
    std::string playersTitle;
    std::vector<RosterRow> roster;
    bool ready = false;                 // the Ready toggle: members (the host starts the match instead)
    bool readyOn = false;               // we are marked ready: the toggle is framed, our row shows the dot
    std::string readyLabel;
    bool hostPassword = false;          // the host's password box (SET and CLEAR), in a lobby
    std::string passwordNote;           // beside it: locked or open, and that changing it kicks nobody
    bool balance = false;               // Auto-balance: the host, in the formats with teams
    std::vector<ModeCard> modes;        // the grid's reading order, two to a row
    std::vector<SettingRow> settings;
    SettingRow playerLimit;             // the Players stepper (LobbyRow::Players): the host steps it, members read it; in a lobby only
    bool hasPlayerLimit = false;
    SettingRow scenario;                // the Scenario stepper (LobbyRow::Scenario): only when the picked map offers more than Standard; kept apart from `settings` like the player limit
    bool hasScenario = false;
    std::string hostOnly;               // members: "Only the host can change these."
    std::vector<MapCard> maps;          // the pickable maps, kMaps order; none for Progression
    bool start = false;                 // START MATCH: the host, while every known build matches (or the override is on)
    std::string waiting;                // members: "Waiting for the host"; anyone: the build warning, which replaces it
    std::string summary;                // beside START: the mode, the map and the gear, one line
    bool buildMismatch = false;         // another player is on a different build (waiting says who)
    std::string scenarioMap;            // the picked map's package path for the Scenario row's stepper (StepScenario); empty: a modded map
};

// ------------------------------------------------------------------ steppers

// The option before `cur`, wrapping; an unknown value steps to the last (NextOf steps it to the first).
inline int PrevOf(const std::vector<int>& options, int cur) {
    if (options.empty()) return cur;
    auto it = std::find(options.begin(), options.end(), cur);
    return it == options.end() || it == options.begin() ? options.back() : *(it - 1);
}
inline int StepOf(const std::vector<int>& options, int cur, int dir) { return dir < 0 ? PrevOf(options, cur) : NextOf(options, cur); }

inline std::vector<int> LimitOptions(Format f) {
    if (f == Format::Tournament) return {1};                          // one round a fight (no Limit row)
    if (f == Format::PitBridgeDuel) return {3, 5, 7, 10, 15};         // fights won to take the match
    if (IsRoundFormat(f)) return {1, 2, 3, 5, 7, 10};                 // rounds to win
    return {3, 5, 10, 15, 20, 30, 50};
}
inline const std::vector<int> kTimeOptions = {0, 300, 600, 900, 1200};
inline const std::vector<int> kRespawnOptions = {3, 5, 10, 15, 20};
inline const std::vector<int> kCtfCaptureOptions = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};         // Captures to win (kCtfMinCaptures..kCtfMaxCaptures)
inline const std::vector<int> kFlagReturnOptions = {10, 15, 20, 25, 30, 40, 50, 60};        // Flag return time in seconds (kFlagReturnMinS..kFlagReturnMaxS)
inline const std::vector<int> kInfectionTimeOptions = {300, 600, 900, 1200};   // Infection always has a clock: the survivors win by lasting
inline const std::vector<int> kFuseOptions = {10, 15, 20, 30, 45, 60};         // Tag's fuse, seconds
// The wave break in seconds the match runs (0 = the format's own: Survival 60, Zombies 10).
inline int WaveBreakShown(const MatchConfig& c) { return c.waveBreakS ? c.waveBreakS : c.format == Format::Zombies ? int(Match::kZombieBreakMs / 1000) : int(Match::kWaveBreakMs / 1000); }

// The format and team pick go their own ways (SelectFormat, StepTeamPick); every other row steps here, by +1 or -1.
// A value out of range steps from where Sanitize would put it. damage: the game's own slider (the Damage row's range).
inline MatchConfig StepSetting(MatchConfig c, LobbyRow s, int dir, const DamageRange& damage = DamageRange{}) {
    bool back = dir < 0;
    switch (s) {
        case LobbyRow::Limit: c.scoreLimit = uint16_t(StepOf(LimitOptions(c.format), c.scoreLimit, dir)); break;
        case LobbyRow::Time: c.timeLimitS = uint16_t(StepOf(IsInfection(c.format) ? kInfectionTimeOptions : kTimeOptions, c.timeLimitS, dir)); break;
        case LobbyRow::Fuse: c.fuseS = uint8_t(StepOf(kFuseOptions, c.fuseS, dir)); break;
        case LobbyRow::Respawn: c.respawnDelayS = uint16_t(StepOf(kRespawnOptions, c.respawnDelayS, dir)); break;
        case LobbyRow::Gear: {
            uint8_t g = uint8_t(c.gear) < kGearRowCount ? uint8_t(c.gear) : 0;   // Fists (the Brawl card) is no stop of the row
            c.gear = Gear((g + (back ? kGearRowCount - 1 : 1)) % kGearRowCount);
            break;
        }
        case LobbyRow::Tier: c.tier = uint8_t((std::min<uint8_t>(c.tier, 7) + (back ? 7 : 1)) % 8); break;
        case LobbyRow::Weapons: {
            uint8_t w = uint8_t(c.weapons) < kWeaponFilterCount ? uint8_t(c.weapons) : 0;
            c.weapons = WeaponFilter((w + (back ? kWeaponFilterCount - 1 : 1)) % kWeaponFilterCount);
            break;
        }
        case LobbyRow::Crossbows:
            c.crossbows = uint8_t((ValidCrossbows(c.crossbows) ? c.crossbows : 0) + (back ? kCrossbowModeCount - 1 : 1)) % kCrossbowModeCount;
            break;
        case LobbyRow::CtfCaptures: c.ctfCaptures = uint8_t(StepOf(kCtfCaptureOptions, c.ctfCaptures, dir)); break;
        case LobbyRow::FlagReturn: c.flagReturnS = uint8_t(StepOf(kFlagReturnOptions, c.flagReturnS, dir)); break;
        case LobbyRow::Teams: c.teams = uint8_t(StepOf({2, 3, 4}, c.teams, dir)); break;
        case LobbyRow::FriendlyFire: c.friendlyFire = !c.friendlyFire; break;
        case LobbyRow::FreeCam: c.freeCam = !c.freeCam; break;
        case LobbyRow::Damage: c.damagePct = StepDamage(c.damagePct, dir, damage); break;
        case LobbyRow::WaveBreak: {
            std::vector<int> o(std::begin(kWaveBreakOptions), std::end(kWaveBreakOptions));
            c.waveBreakS = uint16_t(StepOf(o, WaveBreakShown(c), dir)); break;
        }
        case LobbyRow::ClearGround: {
            std::vector<int> o(std::begin(kClearGroundOptions), std::end(kClearGroundOptions));
            c.clearGroundMin = uint8_t(StepOf(o, c.clearGroundMin, dir)); break;
        }
        case LobbyRow::TeamKit: {      // the row reads Tabard / Full kit / Off
            static constexpr hsm::TeamKit kOrder[3] = {hsm::TeamKit::Tabard, hsm::TeamKit::Full, hsm::TeamKit::Off};
            int at = 1;                                           // a value out of range steps from Full
            for (int i = 0; i < 3; ++i) if (kOrder[i] == c.teamKit) at = i;
            c.teamKit = kOrder[(at + (back ? 2 : 1)) % 3];
            break;
        }
        case LobbyRow::Players: case LobbyRow::MyTeam: break;   // the player limit is the lobby's (StepPlayerLimit); the team pick goes its own way
        case LobbyRow::Scenario: break;                          // which scenarios there are depends on the map: StepScenario(LobbyScene::scenarioMap, ...)
        case LobbyRow::Sync: c.hostPhysics = !c.hostPhysics; break;
    }
    return c;
}

// A new format starts at its own default limit (the other settings stay); the same format changes nothing. Tag is
// bare-fisted (gear Fists at kBrawlTier) and starts at kTagDamagePct, which the Damage row can still change.
inline MatchConfig LeaveTag(MatchConfig c) {           // gear and tier back at their defaults; Tag's light hits stay in Tag
    if (!IsTag(c.format)) return c;
    c.gear = Gear::Own;
    c.tier = MatchConfig{}.tier;
    if (c.damagePct == kTagDamagePct) c.damagePct = kDamageStock;
    return c;
}
inline MatchConfig SelectFormat(MatchConfig c, Format f) {
    if (c.format == f) return c;
    c = LeaveTag(c);
    c.format = f;
    c.scoreLimit = DefaultScoreLimit(f);
    if (IsTag(f)) { c.gear = Gear::Fists; c.tier = kBrawlTier; c.damagePct = kTagDamagePct; }
    return c;
}

// A mode card picked: the Brawl card is free-for-all with bare fists (gear Fists, kBrawlTier, free-for-all's default limit);
// the format cards are SelectFormat, and leaving Brawl puts the gear and tier back at their defaults (even for the
// free-for-all card: the same format, other rules).
inline MatchConfig SelectMode(MatchConfig c, Format f, bool brawl) {
    if (brawl) {
        if (IsBrawl(c)) return c;
        c = LeaveTag(c);
        c.format = Format::FreeForAll;
        c.scoreLimit = DefaultScoreLimit(c.format);
        c.gear = Gear::Fists;
        c.tier = kBrawlTier;
        return c;
    }
    if (IsBrawl(c)) {                                // leaving Brawl (leaving Tag is SelectFormat's)
        c.gear = Gear::Own;
        c.tier = MatchConfig{}.tier;
        if (c.format == f) { c.scoreLimit = DefaultScoreLimit(f); return c; }
    }
    return SelectFormat(c, f);
}

// Forward: Auto, team 0, ..., the last team, Auto (NextTeamPick). Back the other way. A pick the format has no team
// for counts as Auto, as ResolveTeams counts it.
inline int8_t StepTeamPick(int8_t cur, int teams, int dir) {
    if (dir >= 0) return NextTeamPick(cur, teams);
    if (teams <= 0) return kNoTeam;
    if (cur < 0 || cur >= teams) return int8_t(teams - 1);
    return cur == 0 ? kNoTeam : int8_t(cur - 1);
}

// ------------------------------------------------------------------ text

inline constexpr const char* kBrawlBlurb = "Free-for-all with bare fists: everyone spawns naked, no weapons. First to the limit wins.";

inline const char* FormatBlurb(Format f) {
    switch (f) {
        case Format::Duel: return "One on one, rounds to the last one standing. Everybody respawns together; new gear each round with random or identical gear.";
        case Format::LastManStanding: return "Rounds to the last one standing, for two or more players. Everybody respawns together; new gear each round with random or identical gear.";
        case Format::TwoTeams: case Format::ThreeTeams: return "Two to four teams (Red, Blue, Green, Gold). Every kill scores for the team; the first team to the limit wins.";
        case Format::TeamDuel: return "Teams in rounds: the last team standing takes the round. Two to four teams; everybody respawns together, new gear each round with random or identical gear.";
        case Format::Riot: return "One knight in the best kit against everyone else in lighter gear. The last side standing takes the round; the knight changes every match.";
        case Format::CaptureTheFlag: return "Two teams, one flag each. Take the enemy flag and carry it home; a capture only counts while your own flag is home. The first team to the captures wins.";
        case Format::Juggernaut: return "Tall, heavy juggernauts against shorter, lighter fighters with more gear. The last side standing takes the round; the juggernauts change every match.";
        case Format::Tag: return "Bare fists and light hits. One fighter is it and carries a fuse: hit someone to pass it on, or burn out when it runs down. The last one standing takes the round. Two or more players.";
        case Format::Infection: return "Survivors against the infected. Patient zero starts infected; whoever an infected fighter kills is infected too and comes back. Survive the clock to win.";
        case Format::CrossbowTDM: return "Teams, with a crossbow for everyone as well as the usual kit. Every kill scores for the team; the first team to the limit wins. Experimental: needs the crossbow test.";
        case Format::Tournament: return "A knockout bracket, one on one: two fight, everyone else watches. One round a fight: the winner advances, the last one left takes the match. Three or more players.";
        case Format::PitBridgeDuel: return "On the Pit's bridge, the winner stays: the loser goes to the back of the line and the next player steps up. Most fights won takes the match.";
        case Format::PlayersVsBots: return "Everyone against waves of bots that rank up. The fallen return when a wave is cleared.";
        case Format::Zombies: return "Zombie waves in the Abyss, one zombie at a time per player. The fallen return when a wave is cleared.";
        case Format::FreeForAll: break;
    }
    return "Everyone for themselves. Every kill scores; the first to the limit wins.";
}

inline const char* MapTag(const MapInfo& m) {
    if (CtfOnly(m)) return "Experimental \xC2\xB7 Capture the flag only";
    if (m.abyss) return "Large level \xC2\xB7 no spawn points: ring spawn";
    return m.bigArena ? "Large level" : "Arena";
}

inline const char* MapRefusal(Format f) {
    if (f == Format::Zombies) return "Zombies needs the Abyss";
    if (f == Format::Duel) return "Duel needs an arena";
    if (f == Format::CaptureTheFlag) return "Capture the flag: Alley or Forest only";
    if (f == Format::Tournament) return "Tournament needs an arena";
    if (f == Format::PitBridgeDuel) return "Pit bridge duels need the Pit";
    return "Not for this mode";
}

inline const char* GearName(Gear g) {
    if (g == Gear::RandomTier) return "Random by tier";
    if (g == Gear::Mirror) return "Everyone identical";
    return "Own loadouts";
}

inline int TierBudget(int tier) { return kTierBudget[std::clamp(tier, 0, 7)]; }

namespace lobby_detail {
inline bool SamePath(std::string_view a, std::string_view pick) {
    const std::string_view b = map_detail::StripObjectName(pick);
    if (a.empty() || a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x + 32);
        if (y >= 'A' && y <= 'Z') y = char(y + 32);
        if (x != y) return false;
    }
    return true;
}

// The package path of the map a match of this lobby starts on; empty for a modded map (the scenarios offer Standard only).
inline std::string ScenarioMap(const LobbyLocal& l) {
    if (l.config.format != Format::Zombies)
        for (const LobbyModMap& m : l.modMaps) if (SamePath(m.path, l.mapPick)) return {};
    const size_t i = EffectiveMap(l.config.format, l.mapPick);
    return i < MapCount() ? std::string(kMaps[i].path) : std::string();
}

inline bool ValidTeam(int t, Format f, int teams) { return PicksTeams(f) && t >= 0 && t < TeamCount(f, teams); }
inline int EffectiveTeam(const LobbyPlayer& p, Format f, int teams) {
    return ValidTeam(p.hostOverride, f, teams) ? p.hostOverride : ValidTeam(p.team, f, teams) ? p.team : kNoTeam;
}

inline SettingRow Row(const LobbyLocal& l, LobbyRow s) {
    const MatchConfig& c = l.config;
    Format f = c.format;
    bool duel = IsRoundFormat(f), bots = IsBotFormat(f);
    SettingRow r;
    r.id = s;
    r.enabled = l.host;
    switch (s) {
        case LobbyRow::Players:
            r.label = "Players";
            r.value = std::to_string(l.maxPlayers);
            r.hint = "How many players the lobby takes. It cannot go below the players already in; nobody is kicked.";
            break;
        case LobbyRow::Limit:
            r.label = f == Format::Tournament ? "Rounds per fight" : f == Format::PitBridgeDuel ? "Wins to take the match" : duel ? "Rounds to win" : "Score limit";
            r.value = std::to_string(LimitToKey(f, c.scoreLimit));
            r.hint = f == Format::Tournament ? "How many rounds a fight is (best of): the first to win " + std::to_string(c.scoreLimit) + " takes it and moves on."
                   : f == Format::PitBridgeDuel ? "The first to win this many fights takes the match. The winner of a fight stays on the bridge."
                   : duel ? "The first to win this many rounds takes the match."
                   : HasTeams(f) ? "The first team to this many kills wins." : "The first to this many kills wins.";
            break;
        case LobbyRow::Time:
            r.label = "Time limit";
            r.value = TimeLabel(c.timeLimitS);
            if (bots) r.hint = c.timeLimitS ? "Players win if they hold out to the end (one wave cleared)." : "No clock: hold out as long as you can.";
            else if (IsCtf(f)) r.hint = c.timeLimitS ? "When it runs out the team with more captures wins; a tie goes to sudden death (the next capture wins)." : "No clock: play to the captures.";
            else if (IsInfection(f)) r.hint = "The survivors win if any of them are still standing when it runs out.";
            else r.hint = c.timeLimitS ? "When it runs out the leader wins; a tie goes to sudden death." : "No clock: play to the limit.";
            break;
        case LobbyRow::Respawn:
            r.label = duel ? "Round break" : "Respawn delay";
            r.value = std::to_string(c.respawnDelayS) + " s";
            r.hint = duel ? "The pause between the end of a round and the next." : IsInfection(f) ? "How long the infected wait to come back." : "How long the fallen wait to come back.";
            break;
        case LobbyRow::Gear:
            r.label = "Gear";
            r.value = GearName(c.gear);
            if (c.gear == Gear::RandomTier) r.hint = bots ? "A random loadout for each player at the start, kept for the whole run."
                                                    : duel ? "Fresh random gear for each fighter every round." : "Fresh random gear for each fighter every life.";
            else if (c.gear == Gear::Mirror) r.hint = bots ? "One random loadout for everyone, kept for the whole run."
                                                  : duel ? "One random loadout for everyone, new each round." : "One random loadout for everyone, until the rematch.";
            else r.hint = "Everyone fights in their own loadout.";
            break;
        case LobbyRow::Tier:
            r.label = bots ? "Starting tier" : "Gear tier";
            r.value = "tier " + std::to_string(int(c.tier));
            if (c.gear != Gear::Own) r.value += " \xC2\xB7 budget " + std::to_string(TierBudget(c.tier));
            // 0.8.0-pre20: the tier is every player's strength (muscle and grip, forced: ForcedWeight), with any gear.
            r.hint = f == Format::Zombies ? "Zombies start at this rank and rise one every 3 waves. "
                   : bots ? "Bots start at this rank and rise one every 2 waves. " : "";
            r.hint += c.gear == Gear::Own ? (bots ? "Also every player's strength." : "Sets every player's strength (muscle and grip).")
                                          : (bots ? "Also every player's strength and the random gear's budget." : "Sets every player's strength; the random gear is rolled within this budget.");
            break;
        case LobbyRow::Weapons:
            r.label = "Weapons";
            r.value = WeaponFilterName(c.weapons);
            r.hint = c.weapons == WeaponFilter::Any ? "The random gear may hold any weapon."
                   : "The random gear's weapons are only of this kind (the game's own weapon filter).";
            break;
        case LobbyRow::CtfCaptures:
            r.label = "Captures to win";
            r.value = std::to_string(c.ctfCaptures);
            r.hint = "The first team to take the enemy flag home this many times wins.";
            break;
        case LobbyRow::FlagReturn:
            r.label = "Flag return time";
            r.value = std::to_string(c.flagReturnS) + " s";
            r.hint = "A dropped flag goes home after this long, or at once when a teammate touches it.";
            break;
        case LobbyRow::Teams:
            r.label = "Teams";
            r.value = std::to_string(TeamCount(c.format, c.teams)) + " teams";
            r.hint = "Players without a pick go to the smallest team.";
            break;
        case LobbyRow::FriendlyFire:
            r.label = "Friendly fire";
            r.value = c.friendlyFire ? "On" : "Off";
            if (bots) r.hint = c.friendlyFire ? "Players can hurt each other." : "Players cannot hurt each other.";
            else r.hint = c.friendlyFire ? "Hits on teammates count." : "Teammates cannot hurt each other.";
            break;
        case LobbyRow::Damage:
            r.label = "Player damage";
            r.value = DamageLabel(c.damagePct, l.damage);
            r.hint = "How hard hits land on every player, the game's own slider for everyone. Click the number to type up to 1000%. Yours is back after the session.";
            break;
        case LobbyRow::WaveBreak: {
            const int s = WaveBreakShown(c);
            r.label = "Wave break";
            r.value = s == 120 ? "2 min" : std::to_string(s) + " s";
            r.hint = "The pause between waves. Hold G to vote to skip it.";
            break;
        }
        case LobbyRow::ClearGround:
            r.label = "Clear ground";
            r.value = c.clearGroundMin ? "Every " + std::to_string(c.clearGroundMin) + " min" : "Off";
            r.hint = c.clearGroundMin ? "Dropped weapons and loose armour are cleared on this timer, to keep the lag down. Held and worn gear stays."
                                      : "Dropped weapons and loose armour stay until the next round.";
            break;
        case LobbyRow::MyTeam: {
            r.label = "My team";
            r.enabled = true;                                        // everyone picks their own
            const LobbyPlayer* me = nullptr;
            for (const LobbyPlayer& p : l.players) if (p.me) me = &p;
            r.value = TeamLabel(me && ValidTeam(me->team, f, c.teams) ? me->team : kNoTeam);
            r.hint = me && ValidTeam(me->hostOverride, f, c.teams) ? "The host moved you to " + TeamLabel(me->hostOverride) + "."
                                                          : std::string("Auto puts you on the smallest team.");
            break;
        }
        case LobbyRow::TeamKit:
            r.label = "Team colours";
            r.value = c.teamKit == TeamKit::Off ? "Off" : c.teamKit == TeamKit::Tabard ? "Tabard" : "Full kit";
            r.hint = c.teamKit == TeamKit::Off ? "Everyone keeps their own colours."
                   : c.teamKit == TeamKit::Tabard ? "The tabard takes the team colour. Each player sees it on their own copy of every fighter."
                   : "Clothes, tabard, shield and grips take the team colour; blades stay steel. Each player sees it on their own copy of every fighter.";
            break;
        case LobbyRow::FreeCam:
            r.label = "Free camera";
            r.value = c.freeCam ? "On" : "Off";
            r.hint = c.freeCam ? "Dead players can press F to fly a camera round the map. It stays inside the level."
                               : "Dead players can only follow the living.";
            break;
        case LobbyRow::Crossbows:
            r.label = "Crossbows";
            r.value = CrossbowsName(c.crossbows);
            r.hint = "Experimental: needs the crossbow test. Random players spawn with a crossbow (a new draw each round).";
            break;
        case LobbyRow::Fuse:
            r.label = "Fuse time";
            r.value = std::to_string(c.fuseS) + " s";
            r.hint = "How long \"it\" has before the fuse runs out and it is out of the round. Hitting someone passes the fuse on; it keeps burning.";
            break;
        case LobbyRow::Scenario: {
            const std::string map = ScenarioMap(l);
            const Scenario sc = EffectiveScenario(map, c.scenario);   // a pick the map does not offer reads Standard
            r.label = "Scenario";
            r.value = ScenarioName(sc);
            r.hint = ScenarioHint(sc);
            r.enabled = l.host && ScenariosFor(map).size() > 1;
            break;
        }
        case LobbyRow::Sync:
            r.label = "Sync";
            r.value = c.hostPhysics ? "Host physics" : "Per player";
            r.hint = c.hostPhysics ? "The host runs every fighter's physics, so every screen shows the same fight. Your own moves show about one ping late. From the next map."
                                   : "Each player runs their own fighter: no delay on your own moves, but hits and grabs can differ between screens. From the next map.";
            break;
    }
    return r;
}

}  // namespace lobby_detail

// The rows the format uses, in page order. Every format has Damage (Progression co-op shows it alone).
inline std::vector<LobbyRow> SettingsFor(const MatchConfig& c) {
    Format f = c.format;
    const bool rolled = c.gear == Gear::RandomTier || c.gear == Gear::Mirror;   // the Weapons row filters generated gear only
    if (IsBotFormat(f)) {                            // endless, back at a wave clear in the run's loadout
        std::vector<LobbyRow> b = {LobbyRow::Time, LobbyRow::FreeCam, LobbyRow::Gear, LobbyRow::Tier};
        if (rolled) b.push_back(LobbyRow::Weapons);
        for (LobbyRow r : {LobbyRow::WaveBreak, LobbyRow::FriendlyFire, LobbyRow::Damage, LobbyRow::Crossbows, LobbyRow::Sync}) b.push_back(r);
        return b;
    }
    if (IsBrawl(c)) return {LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::FreeCam, LobbyRow::Damage, LobbyRow::ClearGround, LobbyRow::Sync};   // everyone bare at kBrawlTier's strength
    if (IsTag(f)) {                                  // rounds with a fuse: no clock, no teams
        return {LobbyRow::Limit, LobbyRow::Respawn, LobbyRow::Fuse, LobbyRow::FreeCam, LobbyRow::Damage, LobbyRow::Sync};   // bare fists: no gear rows
    }
    if (IsInfection(f)) {                            // two fixed sides: the infection decides the teams, the colours and the crossbows
        std::vector<LobbyRow> t = {LobbyRow::Time, LobbyRow::Respawn, LobbyRow::FreeCam, LobbyRow::Gear, LobbyRow::Tier};
        if (rolled) t.push_back(LobbyRow::Weapons);
        t.push_back(LobbyRow::Damage); t.push_back(LobbyRow::Sync);
        return t;
    }
    if (HasRoles(f))                                 // the roles set the gear, tier, build and team: only the match's own rules are left
        return {LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::FreeCam, LobbyRow::FriendlyFire, LobbyRow::Damage, LobbyRow::TeamKit, LobbyRow::Crossbows, LobbyRow::Sync};
    std::vector<LobbyRow> v;
    if (f != Format::Tournament) { v.push_back(LobbyRow::Limit); v.push_back(LobbyRow::Time); }   // a bracket: one round a fight, no clock
    for (LobbyRow r : {LobbyRow::Respawn, LobbyRow::FreeCam, LobbyRow::Gear}) v.push_back(r);
    if (IsCtf(f)) v = {LobbyRow::CtfCaptures, LobbyRow::FlagReturn, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::FreeCam, LobbyRow::Gear};   // captures, not kills
    v.push_back(LobbyRow::Tier);                     // the tier is every player's strength, with own gear too
    if (rolled) v.push_back(LobbyRow::Weapons);
    if (HasTeams(f) && !IsCtf(f)) v.push_back(LobbyRow::Teams);                // Capture the flag is two fixed teams
    if (HasTeams(f)) v.push_back(LobbyRow::FriendlyFire);
    v.push_back(LobbyRow::Damage);
    if (!IsRoundFormat(f)) v.push_back(LobbyRow::ClearGround);
    if (HasTeams(f)) { v.push_back(LobbyRow::MyTeam); v.push_back(LobbyRow::TeamKit); }
    if (!IsCrossbowTdm(f)) v.push_back(LobbyRow::Crossbows);    // Crossbow TDM hands everyone one: no row
    v.push_back(LobbyRow::Sync);
    return v;
}

inline constexpr Format kModeOrder[kModeOrderCount] = {Format::FreeForAll, Format::Duel, Format::LastManStanding, Format::TwoTeams,
                                                       Format::TeamDuel, Format::Riot, Format::Juggernaut, Format::CaptureTheFlag, Format::Tag, Format::Infection,
                                                       Format::CrossbowTDM, Format::Tournament, Format::PitBridgeDuel, Format::PlayersVsBots, Format::Zombies};

// Which formats a lobby with `players` players can run (the host's count, a dedicated server's own seat not counted).
// Duel is one on one; Last Man Standing and Team Duel need somebody to be the last of; Riot and Juggernaut need a side of
// two against the one; Tag, Infection and Crossbow TDM need a second player. `hint` says why not, for the card.
struct ModeAvail { bool ok = true; const char* hint = ""; };
inline ModeAvail ModeAvailable(Format f, int players) {
    if (f == Format::Duel && players != 2) return {false, "Duel is for 2 players"};
    if ((f == Format::LastManStanding || f == Format::TeamDuel || IsCtf(f) || IsTag(f) || IsInfection(f) || IsCrossbowTdm(f)) && players < 2) return {false, "Needs 2 or more players"};
    if ((HasRoles(f) || f == Format::Tournament) && players < 3) return {false, "Needs 3 or more players"};
    if (f == Format::PitBridgeDuel && players < 2) return {false, "Needs 2 or more players"};
    return {};
}
// A third player joins a lobby set to Duel: it becomes Last Man Standing, keeping the round count and the gear rows.
inline bool DuelOutgrown(const MatchConfig& c, int players) { return c.format == Format::Duel && players > 2; }
inline MatchConfig AdjustForPlayers(MatchConfig c, int players) {
    if (DuelOutgrown(c, players)) c.format = Format::LastManStanding;
    return c;
}
inline constexpr const char* kDuelOutgrownLine = "Duel is for 2 players: switched to Last man standing";

// The group of the i-th mode card (kModeOrder, then Brawl, then Progression): known before any scene, so the page can
// build each card in its group's grid.
inline ModeGroup ModeGroupAt(size_t i) {
    if (i < kModeOrderCount) return IsBotFormat(kModeOrder[i]) ? ModeGroup::Coop : ModeGroup::Versus;
    return i == kModeOrderCount ? ModeGroup::Versus : ModeGroup::Coop;   // Brawl; Progression
}

inline LobbyScene BuildLobbyScene(const LobbyLocal& l) {
    using namespace lobby_detail;
    LobbyScene s;
    s.me = l.myName;
    s.badge = "v" + l.version + " \xC2\xB7 protocol " + std::to_string(int(kProtocolRevision));
    switch (l.phase) {
        case LobbyPhase::Offline: s.pill = {"Network not ready", style::kWarn}; break;
        case LobbyPhase::Idle: s.pill = {"Not in a lobby", style::kIron}; break;
        case LobbyPhase::Creating: s.pill = {"Creating a lobby", style::kWarn}; break;
        case LobbyPhase::Joining: s.pill = {"Joining a lobby", style::kWarn}; break;
        case LobbyPhase::InLobby: {
            if (l.host) { s.pill = {"Hosting via direct UDP", style::kOk}; break; }
            std::string owner;
            for (const LobbyPlayer& p : l.players) if (p.host) owner = p.name;
            s.pill = {owner.empty() ? std::string("In a lobby") : "In " + owner + "'s lobby", style::kOk};
            break;
        }
    }
    s.idle = l.phase == LobbyPhase::Idle;
    s.in = l.phase == LobbyPhase::InLobby;
    if (!s.in) s.status = l.status;
    if (s.idle) {
        s.filters = FilterChips(l.filter);
        std::vector<size_t> shown;
        for (size_t i = 0; i < l.found.size(); ++i) if (PassesFilter(l.found[i], l.filter)) shown.push_back(i);
        if (l.filter.mostPlayers)
            std::stable_sort(shown.begin(), shown.end(), [&](size_t a, size_t b) { return l.found[a].players > l.found[b].players; });
        if (!l.found.empty())
            s.browserCount = shown.size() == l.found.size() ? std::to_string(l.found.size()) + (l.found.size() == 1 ? " lobby" : " lobbies")
                           : std::to_string(shown.size()) + " of " + std::to_string(l.found.size()) + " lobbies shown";
        if (l.found.empty()) s.browserEmpty = "No public lobbies yet. REFRESH looks again.";
        else if (shown.empty()) s.browserEmpty = "No lobby matches these filters.";
        for (size_t at : shown) {
            if (s.browser.size() >= kBrowserRows) break;
            const LobbyFound& f = l.found[at];
            BrowserRow r;
            r.source = at;
            r.name = f.name.empty() ? "Unnamed lobby" : f.name;
            r.locked = f.locked;
            if (f.locked) r.name = kLockedMarker + r.name;
            r.players = std::to_string(f.players) + "/" + std::to_string(f.maxPlayers);
            r.detail = f.map;
            if (!f.mode.empty()) r.detail += (r.detail.empty() ? "" : " \xC2\xB7 ") + f.mode;
            if (!f.pack.empty()) r.detail += (r.detail.empty() ? "" : " \xC2\xB7 ") + f.pack;
            if (!f.compatible) r.note = {f.reason.empty() ? std::string("Incompatible") : f.reason, style::kWarn};
            else if (LobbyIsFull(f.players, f.maxPlayers)) r.note = {"Full", style::kWarn};
            else { r.note = {"Join", style::kOk}; r.joinable = true; }
            s.browser.push_back(r);
        }
    }
    if (!s.in) return s;

    const MatchConfig& c = l.config;
    Format f = c.format;
    s.playersTitle = "PLAYERS " + std::to_string(l.playerCount >= 0 ? l.playerCount : int(l.players.size())) + "/" + std::to_string(l.maxPlayers);
    for (const LobbyPlayer& p : l.players) {
        RosterRow r;
        r.name = p.name.empty() ? "?" : p.name;
        r.host = p.host; r.ready = p.ready; r.me = p.me;
        if (PicksTeams(f)) {
            int t = EffectiveTeam(p, f, c.teams);
            r.team = TeamLabel(t);
            r.swatch = TeamColor(t);                                // none for Auto
            r.moved = ValidTeam(p.hostOverride, f, c.teams);
            r.movable = l.host;
        }
        s.roster.push_back(r);
    }
    if (!l.host) {
        s.ready = true;
        for (const LobbyPlayer& p : l.players) if (p.me) s.readyOn = p.ready;
        s.readyLabel = s.readyOn ? "Ready \xC2\xB7 click to undo" : "Mark me ready";
    }
    s.hostPassword = l.host;
    if (l.host) s.passwordNote = l.locked ? "Locked: players need the password. Changing it kicks nobody."
                                          : "Open: type a password and press SET to lock the lobby.";
    s.balance = l.host && PicksTeams(f);
    const int players = l.playerCount >= 0 ? l.playerCount : int(l.players.size());
    for (Format m : kModeOrder) {
        const ModeAvail av = ModeAvailable(m, players);
        ModeCard mc;
        mc.format = m; mc.name = Upper(FormatName(m)); mc.description = FormatBlurb(m);
        mc.selected = m == f && !IsBrawl(c) && !l.progression; mc.enabled = l.host && av.ok;
        if (!av.ok) mc.hint = av.hint;
        s.modes.push_back(mc);
    }
    {
        ModeCard b;
        b.format = Format::FreeForAll;
        b.name = Upper(ModeName(Format::FreeForAll, Gear::Fists));
        b.description = kBrawlBlurb;
        b.selected = IsBrawl(c) && !l.progression; b.enabled = l.host; b.brawl = true;
        s.modes.push_back(b);
    }
    {
        ModeCard p;
        p.name = "PROGRESSION (CO-OP)";
        p.description = "Stock Progression in the Tavern, together. Free Mode off; your own save is untouched.";
        p.selected = l.progression; p.enabled = l.host; p.progression = true;
        s.modes.push_back(p);
    }
    for (size_t i = 0; i < s.modes.size(); ++i) s.modes[i].group = ModeGroupAt(i);
    s.playerLimit = Row(l, LobbyRow::Players);
    s.hasPlayerLimit = true;
    s.scenarioMap = lobby_detail::ScenarioMap(l);
    if (!l.progression) for (LobbyRow id : SettingsFor(c)) s.settings.push_back(Row(l, id));
    else s.settings.push_back(Row(l, LobbyRow::Damage));   // co-op fights take player damage too
    if (!l.progression && ScenariosFor(s.scenarioMap).size() > 1) { s.scenario = Row(l, LobbyRow::Scenario); s.hasScenario = true; }   // WORLD section, before Sync
    if (!l.host) s.hostOnly = "Only the host can change these.";
    size_t customChosen = l.modMaps.size();   // the picked custom map (none: l.modMaps.size())
    if (f != Format::Zombies)
        for (size_t i = 0; i < l.modMaps.size(); ++i) if (lobby_detail::SamePath(l.modMaps[i].path, l.mapPick)) customChosen = i;
    const size_t chosen = customChosen < l.modMaps.size() ? MapCount() : EffectiveMap(f, l.mapPick);
    for (size_t i = 0; i < MapCount() && !l.progression; ++i) {
        if (!kMaps[i].pick) continue;
        MapCard m;
        m.index = i;
        m.name = kMaps[i].display;
        m.tag = MapTag(kMaps[i]);
        m.allowed = MapAllowsFormat(kMaps[i], f);
        if (!m.allowed) m.reason = CtfOnly(kMaps[i]) ? "Capture the flag only" : MapRefusal(f);
        m.selected = i == chosen;
        m.enabled = l.host && m.allowed;
        s.maps.push_back(m);
    }
    for (size_t i = 0; i < l.modMaps.size() && !l.progression; ++i) {   // modded maps follow the stock ones
        MapCard m;
        m.index = MapCount() + i;
        m.modded = true;
        m.path = l.modMaps[i].path;
        m.name = l.modMaps[i].display;
        m.tag = l.modMaps[i].pak.empty() ? "Modded map" : "Modded map \xC2\xB7 " + l.modMaps[i].pak;
        m.allowed = f != Format::Zombies && f != Format::PitBridgeDuel;   // the Abyss is the only Zombies map, the Pit the only bridge
        if (!m.allowed) m.reason = MapRefusal(f);
        m.selected = i == customChosen;
        m.enabled = l.host && m.allowed;
        s.maps.push_back(m);
    }
    std::vector<PeerBuild> others;
    for (const LobbyPlayer& p : l.players) if (!p.me) others.push_back({p.name, p.build});
    const std::string warn = BuildWarning(l.myBuild, others);
    s.buildMismatch = !warn.empty();
    s.start = l.host && MayStartMatch(l.myBuild, others, l.allowBuildMismatch);
    const ModeAvail playable = l.progression ? ModeAvail{} : ModeAvailable(f, players);   // Duel needs exactly 2, Last Man Standing 2 or more
    const bool modWait = l.host && customChosen < l.modMaps.size() && l.modPickPending;
    if (modWait || !playable.ok) s.start = false;
    // The line beside START: what will be played.
    if (l.progression) s.summary = "PROGRESSION \xC2\xB7 the Tavern, together";
    else {
        s.summary = Upper(ModeName(f, c.gear, c.teams));
        for (const MapCard& m : s.maps) if (m.selected) s.summary += " \xC2\xB7 " + m.name;
        const bool gearRows = !IsBare(c) && !HasRoles(f);               // Riot and Juggernaut deal the gear by role; Brawl and Tag are bare
        if (gearRows) s.summary += " \xC2\xB7 " + std::string(GearName(c.gear));
        if (gearRows && c.gear != Gear::Own) s.summary += ", tier " + std::to_string(int(c.tier));
        if (gearRows && c.gear != Gear::Own && c.weapons != WeaponFilter::Any) s.summary += ", " + std::string(WeaponFilterName(c.weapons));
    }
    if (!l.host) s.waiting = "Waiting for the host";
    if (modWait) s.waiting = "Checking the map pack";
    if (l.host && !playable.ok) s.waiting = playable.hint;
    if (s.buildMismatch) s.waiting = l.host && l.allowBuildMismatch ? warn + " (allowed: allow_build_mismatch=1)" : warn;
    return s;
}

}  // namespace hsm
