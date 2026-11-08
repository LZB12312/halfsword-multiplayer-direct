#include "gameplay/lobby.h"
#include "config.h"
#include "session/hsm_display_name.h"
#include "file_hash.h"
#include "gameplay/mod_pack.h"
#include "gameplay/mod_rules.h"
#include "hsm_protocol.h"
#include "log.h"
#include "match/hsm_build_card.h"
#include "session/hsm_build.h"
#include "session/hsm_password.h"
#include "session/hsm_player_limit.h"
#include "ue/engine.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace hsm::game {
namespace {
#ifdef HSM_TESTBENCH
constexpr const char* kLobbyNamePrefix = "[TEST] ";
#else
constexpr const char* kLobbyNamePrefix = "";
#endif
// Parses a decimal int from Steam key/value data; anything else (empty, garbage, overflow) gives the fallback.
int IntOr(const std::string& s, int fallback) {
    if (s.empty() || s.size() > 6) return fallback;
    char* end = nullptr; long v = std::strtol(s.c_str(), &end, 10);
    return end && *end == 0 ? int(v) : fallback;
}
}  // namespace
#ifndef HSM_GIT_DESCRIBE
#define HSM_GIT_DESCRIBE ""
#endif
// The version, the commit build.sh saw and the SHA-256 of this DLL's own file (session/hsm_build.h). Worked out once: the
// first call reads the DLL (a few MB; mod.cpp makes it on the worker thread at startup, before any session).
std::string ModBuild() {
    static const std::string build = ModBuildString(kModVersion, HSM_GIT_DESCRIBE, OwnBinaryHash());
    return build;
}
namespace {
bool g_allowBuildMismatch = false;
}  // namespace
void SetAllowBuildMismatch(bool on) { g_allowBuildMismatch = on; }
bool AllowBuildMismatch() { return g_allowBuildMismatch; }

namespace {
std::string BuildKeyHex() { char b[32]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)ue::BuildKey()); return b; }
std::string ProtocolStr() { return std::to_string(int(kProtocolRevision)); }
}  // namespace

void Lobby::SetStatus(const std::string& s) {
    if (s == status_) return;
    status_ = s; HSM_LOG("lobby: %s", s.c_str());
}

void Lobby::Tick() {
    if (state_ == State::Offline) {
        if (!direct::Init()) return;

        state_ = State::Idle;
        SetStatus("Not in a lobby");
    }
    direct::Tick();
    if (state_ == State::InLobby && !direct::LobbyOwner(lobby_)) { Leave(); SetStatus("Host disconnected; session ended"); return; }
    if (!launchChecked_) {          // launched through a friend's "Join Game"
        launchChecked_ = true;
        std::string id = direct::LaunchConnectLobby();
        if (!id.empty()) Join(std::strtoull(id.c_str(), nullptr, 10));
    }
    direct::Event e;
    while (direct::Poll(e)) {
        switch (e.kind) {
            case direct::Event::LobbyCreated:
                if (!e.ok) { state_ = State::Idle; SetStatus(e.text.empty() ? "Could not create lobby" : e.text); break; }
                direct::SetLobbyData(e.lobby, "hsm_proto", ProtocolStr());
                direct::SetLobbyData(e.lobby, "hsm_build", BuildKeyHex());
                direct::SetLobbyData(e.lobby, "hsm_ver", kModVersion);
                direct::SetLobbyData(e.lobby, "name", kLobbyNamePrefix + direct::PersonaName(direct::MyUserId()) + "'s fight");
                direct::SetLobbyData(e.lobby, "state", "menu");
                direct::SetLobbyData(e.lobby, "hsm_srv", server_ ? "1" : "0");
                direct::SetLobbyData(e.lobby, "hsm_max", std::to_string(maxPlayers_));
                direct::SetLobbyData(e.lobby, "hsm_lock", "0");
                Enter(e.lobby);
                break;
            case direct::Event::LobbyEntered:
                if (state_ == State::Creating) break;    // our own new lobby: entered before LobbyCreated sets its data
                if (state_ == State::InLobby && lobby_ == e.lobby) break;
                if (!e.ok && state_ == State::Idle && askLobby_ == e.lobby) {
                    CancelPasswordAsk();
                    SetStatus(e.text);
                    break;
                }
                // An enter for a lobby we are no longer joining (a late answer after Leave or a second invite) would
                // otherwise drop us into it, or reset a join in progress. Fix from CSLewin's PR #1.
                if (state_ != State::Joining || e.lobby != joining_) {
                    if (e.ok) direct::LeaveLobby(e.lobby);
                    HSM_LOG("lobby: ignored stale enter lobby=%llu", (unsigned long long)e.lobby);
                    break;
                }
                joining_ = 0;
                if (!e.ok) { direct::LeaveLobby(e.lobby); state_ = State::Idle; SetStatus(e.text.empty() ? "Could not connect: check address, UDP port and firewall" : e.text); break; }
                if (state_ != State::InLobby || lobby_ != e.lobby) {
                    LobbyListing l = Describe(e.lobby);
                    if (!l.compatible) { direct::LeaveLobby(e.lobby); state_ = State::Idle; SetStatus(l.modsText.empty() ? "Cannot join: " + l.reason : l.modsText); break; }
                    if (l.pack.any()) {   // the host's modpack: this machine must have exactly it (nothing is downloaded)
                        const PackCheck pc = modpack::Check(l.pack);
                        modpack::LogJoinCheck(l.pack, pc);
                        if (pc.verdict != PackVerdict::Ok) { direct::LeaveLobby(e.lobby); state_ = State::Idle; SetStatus(PackJoinLine(l.pack, pc)); break; }
                    }
                    if (l.locked && passwordLobby_ != e.lobby) {   // an invite or a friend's Join Game: no password was typed yet
                        direct::LeaveLobby(e.lobby,true); state_ = State::Idle;
                        SetStatus("This lobby needs a password");
                        AskPassword(e.lobby);
                        break;
                    }
                    Enter(e.lobby);
                }
                break;
            case direct::Event::LobbyMembersChanged:
            case direct::Event::LobbyDataChanged:
                if (e.lobby == lobby_ && state_ == State::InLobby) {
                    if (e.ok) RefreshMembers();
                    else { Leave(); SetStatus(e.text.empty() ? "Host disconnected" : e.text); }
                }
                break;
            case direct::Event::JoinRequested:
                if (e.lobby && e.lobby != lobby_) { Leave(); Join(e.lobby); }   // Leave is a no-op unless in or joining a lobby
                break;
            case direct::Event::LobbyList: {
                listings_.clear();
                for (direct::UserId id : e.lobbies) {
                    LobbyListing l; l.id = id; l.name = direct::ConnectAddress();
                    l.compatible = true; l.map = "Connect to fetch host settings";
                    listings_.push_back(std::move(l));
                }
                std::stable_sort(listings_.begin(), listings_.end(), [](const LobbyListing& a, const LobbyListing& b) { return a.compatible > b.compatible; });
                SetStatus(e.ok ? std::to_string(listings_.size()) + " configured hosts" : "Could not list lobbies");
                break;
            }
            case direct::Event::SessionRequest: {   // a (re)opened session from a lobby member must be accepted again
                bool member = false;
                if (state_ == State::InLobby && e.user) for (direct::UserId id : direct::LobbyMembers(lobby_)) member = member || id == e.user;
                if (member) direct::AcceptSession(e.user);
                HSM_LOG("lobby: networking session request from %s %s", direct::PersonaName(e.user).c_str(), member ? "accepted" : "ignored (not in our lobby)");
                break;
            }
            case direct::Event::SessionFailed:
                HSM_LOG("lobby: networking session with %s failed (reason %u: %s)", direct::PersonaName(e.user).c_str(), e.a, e.text.c_str());
                break;
            case direct::Event::LobbyChat:
                if (e.lobby == lobby_ && state_ == State::InLobby && onChat) onChat(e.user, e.text);
                break;
        }
    }
}

LobbyListing Lobby::Describe(direct::UserId lobby) const {
    LobbyListing l; l.id = lobby;
    l.name = direct::LobbyData(lobby, "name"); l.map = direct::LobbyData(lobby, "map"); l.mode = direct::LobbyData(lobby, "mode");
    const std::vector<direct::UserId> members = direct::LobbyMembers(lobby);
    const bool server = direct::LobbyData(lobby, "hsm_srv") == "1";
    l.players = int(members.size()) - (server && !members.empty() ? 1 : 0);
    l.maxPlayers = ParsePlayerLimit(direct::LobbyData(lobby, "hsm_max"));   // peer-written; an old lobby has none: 8
    std::string proto = direct::LobbyData(lobby, "hsm_proto"), build = direct::LobbyData(lobby, "hsm_build");
    if (proto.empty()) l.reason = "not a HalfSwordMP lobby";
    else if (proto != ProtocolStr()) l.reason = "different mod version (host " + direct::LobbyData(lobby, "hsm_ver") + ", you " + kModVersion + ")";
    else if (build != BuildKeyHex()) l.reason = "different game version";
    else if (LobbyIsFull(l.players, l.maxPlayers) && std::find(members.begin(), members.end(), direct::MyUserId()) == members.end())
        l.reason = "lobby full";   // a full lobby we are already in is not full to us
    l.locked = direct::LobbyData(lobby, "hsm_lock") == "1";
    l.pack = ReadLobbyPack(direct::LobbyData(lobby, "hsm_pack"), direct::LobbyData(lobby, "hsm_packv"), direct::LobbyData(lobby, "hsm_packh"));   // peer-written: validated
    if (!proto.empty()) {   // mod rules: the lobby's rule against this machine's own report (the host checks again after the join)
        const ModRule rule = DecodeLobbyRule({direct::LobbyData(lobby, kLobbyModRuleKey), direct::LobbyData(lobby, kLobbyModCheatsKey),
                                              direct::LobbyData(lobby, kLobbyModOfficialKey), direct::LobbyData(lobby, kLobbyModListKey)});   // peer-written: DecodeLobbyRule reads it strictly
        const LobbyModView v = ViewLobbyRule(rule, modrules::Own(), modrules::OwnOfficial(), direct::LobbyData(lobby, kLobbyHostCheatKey));   // the host's cheats: peer-written, HostCheatNames filters them
        l.mods = v.tag;
        if (!DecodeLobbyBlock(direct::LobbyData(lobby, kLobbyBlockKey)).hse) l.mods += l.mods.empty() ? "HS Enhancer allowed" : ", HS Enhancer allowed";   // so nobody joins one by accident
        if (modrules::Enabled() && !v.reason.empty()) {   // mod_rules=0: no pre-check here, the host still decides
            l.modsText = v.text;
            if (l.reason.empty()) l.reason = v.reason;
        }
    }
    l.compatible = l.reason.empty();
    return l;
}

void Lobby::Host(direct::LobbyType type, bool server) {
    if (!server && onlineBlock) if (const std::string why = onlineBlock(hostPreset, hostBlock); !why.empty()) { SetStatus(why); return; }
    if (state_ != State::Idle) return;
    password_.clear(); passwordLobby_ = 0; askLobby_ = 0; askNote_.clear();
    type_ = type; server_ = server; state_ = State::Creating; SetStatus("Creating lobby...");
    direct::CreateLobby(type, maxPlayers_ + (server ? 1 : 0));
}

ModRule Lobby::RoomRule(bool* named) const {
    const std::string rule = InLobby() ? direct::LobbyData(lobby_, kLobbyModRuleKey) : std::string();
    if (named) *named = !rule.empty();
    if (rule.empty()) return NoRule();
    return DecodeLobbyRule({rule, direct::LobbyData(lobby_, kLobbyModCheatsKey), direct::LobbyData(lobby_, kLobbyModOfficialKey),
                            direct::LobbyData(lobby_, kLobbyModListKey)});   // peer-written: DecodeLobbyRule reads it strictly
}

bool Lobby::ServerLobby() const { return InLobby() && direct::LobbyData(lobby_, "hsm_srv") == "1"; }

int Lobby::PlayerCount() const { return int(members_.size()) - (ServerLobby() && !members_.empty() ? 1 : 0); }

bool Lobby::Locked() const { return InLobby() && direct::LobbyData(lobby_, "hsm_lock") == "1"; }

const std::string& Lobby::Password() const {
    static const std::string none;
    return passwordLobby_ && passwordLobby_ == lobby_ && InLobby() ? password_ : none;
}

void Lobby::SetPassword(const std::string& password) {
    if (!IsHost()) return;
    password_ = CleanPassword(password); passwordLobby_ = password_.empty() ? 0 : lobby_;
    direct::SetLobbyData(lobby_, "hsm_lock", password_.empty() ? "0" : "1");   // only whether, never the password
}

void Lobby::RememberPassword(const std::string& password) {
    if (!InLobby() || IsHost()) return;
    password_ = CleanPassword(password); passwordLobby_ = password_.empty() ? 0 : lobby_;
}

void Lobby::AskPassword(direct::UserId lobby, const std::string& note) { askLobby_ = lobby; askNote_ = note; }

int Lobby::MaxPlayers() const { return InLobby() ? ParsePlayerLimit(direct::LobbyData(lobby_, "hsm_max")) : maxPlayers_; }

void Lobby::SetMaxPlayers(int n) {
    if (!IsHost()) return;
    maxPlayers_ = ClampPlayerLimit(n, PlayerCount());
    direct::SetLobbyMemberLimit(lobby_, maxPlayers_ + (ServerLobby() ? 1 : 0));   // an old steam_api64 lacks it: the host's Hello check still holds
    direct::SetLobbyData(lobby_, "hsm_max", std::to_string(maxPlayers_));
}

void Lobby::JoinAddress(const std::string& address) {
    if(!direct::SetConnectAddress(address)) { SetStatus("Enter an IPv4 address or hostname, optionally followed by :port (1-65535)"); return; }
    if (direct::IceActive()) direct::LeaveLobby(1);
    SaveIniValues({{"connect",address}});
    Join(1);
}
void Lobby::HostIce() {
    if (state_ != State::Idle) return;
    if (onlineBlock) if (const auto why = onlineBlock(hostPreset,hostBlock); !why.empty()) { SetStatus(why); return; }
    password_.clear(); passwordLobby_=askLobby_=0; askNote_.clear();
    type_=direct::LobbyType::Private;
    server_=false;
    state_=State::Creating;
    SetStatus("Opening direct ICE room...");
    direct::CreateIceLobby(maxPlayers_);
}

void Lobby::JoinIce(const std::string& code) {
    if (state_ != State::Idle) return;
    password_.clear(); passwordLobby_=askLobby_=0; askNote_.clear();
    joining_=1;
    state_=State::Joining;
    SetStatus("Connecting to direct ICE room...");
    direct::JoinIceLobby(code);
}

void Lobby::Join(direct::UserId lobby, const std::string& password) {
    if (state_ != State::Idle || !lobby) return;
    password_ = CleanPassword(password); passwordLobby_ = password_.empty() ? 0 : lobby;
    askLobby_ = 0; askNote_.clear();
    joining_ = lobby; state_ = State::Joining; SetStatus("Joining lobby...");
    direct::JoinLobby(lobby);
}

void Lobby::Enter(direct::UserId lobby) {
    lobby_ = lobby; state_ = State::InLobby; owner_ = direct::LobbyOwner(lobby);
    direct::SetMemberData(lobby_, "hsm_ver", kModVersion);
    direct::SetMemberData(lobby_, "hsm_mod", ModBuild());   // the lobby names every other player on another build
    if (onEnterCard) onEnterCard();
    if (myCard) direct::SetMemberData(lobby_, "hsm_card", myCard());
    if (myRuleSheet) direct::SetMemberData(lobby_, "hsm_sheet", myRuleSheet());
    officialSent_.clear();
    SetOfficial(modrules::OwnOfficial());   // mod rules: the update check's verdict on this DLL (LobbyTick updates it)
    direct::SetRichPresence("connect", "+connect_lobby " + std::to_string(lobby_));
    direct::SetRichPresence("status", IsHost() ? "Hosting a Half Sword fight" : "In a Half Sword lobby");
    members_.clear();
    RefreshMembers();
    SetStatus(IsHost() ? direct::ConnectionInfo() : "Waiting for host to start a fight...");
    if (onEntered) onEntered();
}

void Lobby::Leave(bool keepIce) {
    if (state_ != State::InLobby && state_ != State::Joining && state_ != State::Creating) return;
    direct::LeaveLobby(state_ == State::Joining ? joining_ : lobby_,keepIce);
    for (const LobbyMember& m : members_) if (m.id != direct::MyUserId() && onMemberLeft) onMemberLeft(m.id);
    members_.clear(); lobby_ = owner_ = joining_ = 0; state_ = State::Idle;
    password_.clear(); passwordLobby_ = 0;
    direct::SetRichPresence("connect", ""); direct::SetRichPresence("status", "");
    SetStatus("Left lobby");
    if (onLeft) onLeft();
}

void Lobby::Invite() { if (InLobby()) SetStatus(direct::ConnectionInfo()); }
bool Lobby::SendChat(const std::string& text) { return InLobby() && !text.empty() && direct::SendLobbyChat(lobby_, text); }
void Lobby::SetType(direct::LobbyType type) { if (IsHost() && direct::SetLobbyType(lobby_, type)) type_ = type; }
MatchConfig Lobby::config() const {
    MatchConfig c;
    if (!InLobby()) return c;
    c.format = Format(IntOr(direct::LobbyData(lobby_, "hsm_fmt"), 0));
    c.teams = uint8_t(std::clamp(IntOr(direct::LobbyData(lobby_, "hsm_teams"), 2), 0, 255));   // none: two; hsm_fmt=2, the old "3 teams", is Teams with 3 (Sanitize)
    // the limit's key is the format's own (LimitKeyFor): hsm_trnr rounds per fight (Tournament), hsm_pbwin wins to take the match (Pit Bridge Duels)
    c.scoreLimit = uint16_t(std::clamp(LimitFromKey(c.format, IntOr(direct::LobbyData(lobby_, LimitKeyFor(c.format)), LimitToKey(c.format, DefaultScoreLimit(c.format)))), 0, 1000));
    c.timeLimitS = uint16_t(IntOr(direct::LobbyData(lobby_, "hsm_time"), 0));
    c.respawnDelayS = uint16_t(IntOr(direct::LobbyData(lobby_, "hsm_resp"), 5));
    c.friendlyFire = direct::LobbyData(lobby_, "hsm_ff") == "1";
    c.gear = Gear(IntOr(direct::LobbyData(lobby_, "hsm_gear"), 0));
    c.tier = uint8_t(std::clamp(IntOr(direct::LobbyData(lobby_, "hsm_tier"), 2), 0, 7));
    c.weapons = WeaponFilter(std::clamp(IntOr(direct::LobbyData(lobby_, "hsm_wpn"), 0), 0, int(kWeaponFilterCount) - 1));   // none: Any
    c.teamKit = TeamKit(std::clamp(IntOr(direct::LobbyData(lobby_, "hsm_kit"), int(TeamKit::Full)), 0, int(kTeamKitCount) - 1));   // none: Full kit
    c.crossbows = uint8_t(std::clamp(IntOr(direct::LobbyData(lobby_, "hsm_xbow"), 0), 0, int(kCrossbowModeCount) - 1));   // none: Off
    c.fuseS = uint8_t(std::clamp(IntOr(direct::LobbyData(lobby_, "hsm_fuse"), kFuseDefaultS), int(kFuseMinS), int(kFuseMaxS)));   // none: 30 s (Tag)
    c.scenario = ScenarioFromKey(direct::LobbyData(lobby_, "hsm_scen"));                              // none: Standard
    c.damagePct = uint16_t(std::clamp(IntOr(direct::LobbyData(lobby_, "hsm_dmg"), kDamageStock), 0, int(kDamageStock)));   // none: stock
    c.waveBreakS = uint16_t(std::clamp(IntOr(direct::LobbyData(lobby_, "hsm_wbrk"), 0), 0, 600));      // 0: the format's own
    c.clearGroundMin = uint8_t(std::clamp(IntOr(direct::LobbyData(lobby_, "hsm_clr"), 0), 0, 255));    // 0: off
    c.freeCam = direct::LobbyData(lobby_, "hsm_fcam") != "0";                                       // none: on
    c.ctfCaptures = uint8_t(std::clamp(IntOr(direct::LobbyData(lobby_, "hsm_ctfcap"), kCtfDefaultCaptures), kCtfMinCaptures, kCtfMaxCaptures));   // none: 3
    c.flagReturnS = uint8_t(std::clamp(IntOr(direct::LobbyData(lobby_, "hsm_ctfret"), kFlagReturnDefaultS), kFlagReturnMinS, kFlagReturnMaxS));   // none: 20 s
    c.hostPhysics = direct::LobbyData(lobby_, "hsm_sync") == "host";                                  // none: per player
    return Sanitize(c);
}

void Lobby::SetConfig(const MatchConfig& in) {
    if (!IsHost()) return;
    MatchConfig c = Sanitize(in);
    direct::SetLobbyData(lobby_, "hsm_fmt", std::to_string(int(c.format)));
    direct::SetLobbyData(lobby_, "hsm_teams", std::to_string(c.teams));
    direct::SetLobbyData(lobby_, LimitKeyFor(c.format), std::to_string(LimitToKey(c.format, c.scoreLimit)));
    direct::SetLobbyData(lobby_, "hsm_time", std::to_string(c.timeLimitS));
    direct::SetLobbyData(lobby_, "hsm_resp", std::to_string(c.respawnDelayS));
    direct::SetLobbyData(lobby_, "hsm_ff", c.friendlyFire ? "1" : "0");
    direct::SetLobbyData(lobby_, "hsm_gear", std::to_string(int(c.gear)));
    direct::SetLobbyData(lobby_, "hsm_tier", std::to_string(int(c.tier)));
    direct::SetLobbyData(lobby_, "hsm_wpn", std::to_string(int(c.weapons)));
    direct::SetLobbyData(lobby_, "hsm_kit", std::to_string(int(c.teamKit)));
    direct::SetLobbyData(lobby_, "hsm_xbow", std::to_string(c.crossbows));
    direct::SetLobbyData(lobby_, "hsm_fuse", std::to_string(c.fuseS));
    direct::SetLobbyData(lobby_, "hsm_scen", std::to_string(c.scenario));
    direct::SetLobbyData(lobby_, "hsm_dmg", std::to_string(c.damagePct));
    direct::SetLobbyData(lobby_, "hsm_wbrk", std::to_string(c.waveBreakS));
    direct::SetLobbyData(lobby_, "hsm_clr", std::to_string(c.clearGroundMin));
    direct::SetLobbyData(lobby_, "hsm_fcam", c.freeCam ? "1" : "0");
    direct::SetLobbyData(lobby_, "hsm_ctfcap", std::to_string(int(c.ctfCaptures)));
    direct::SetLobbyData(lobby_, "hsm_ctfret", std::to_string(int(c.flagReturnS)));
    direct::SetLobbyData(lobby_, "hsm_sync", c.hostPhysics ? "host" : "owner");
}
std::string Lobby::MapPick() const { return InLobby() ? direct::LobbyData(lobby_, "hsm_map") : std::string(); }
void Lobby::SetMapPick(const std::string& path) { if (IsHost()) direct::SetLobbyData(lobby_, "hsm_map", path); }
LobbyPack Lobby::Pack() const {
    return InLobby() ? ReadLobbyPack(direct::LobbyData(lobby_, "hsm_pack"), direct::LobbyData(lobby_, "hsm_packv"), direct::LobbyData(lobby_, "hsm_packh")) : LobbyPack{};
}
void Lobby::SetPack(const LobbyPack& p) {
    if (!IsHost() || (!p.any() && !Pack().any())) return;   // nothing announced, nothing to clear
    direct::SetLobbyData(lobby_, "hsm_pack", p.name);
    direct::SetLobbyData(lobby_, "hsm_packv", p.version);
    direct::SetLobbyData(lobby_, "hsm_packh", p.hash);
}
void Lobby::SetModRule(const ModRule& rule, const std::string& hostCheats) {
    if (!IsHost()) return;
    const LobbyModData d = EncodeLobbyRule(rule);
    bool changed = false;
    const auto put = [&](const char* key, const std::string& v) {
        if (direct::LobbyData(lobby_, key) == v) return;
        direct::SetLobbyData(lobby_, key, v);
        changed = true;
    };
    put(kLobbyModRuleKey, d.rule); put(kLobbyModCheatsKey, d.cheats); put(kLobbyModOfficialKey, d.official); put(kLobbyModListKey, d.list);
    put(kLobbyHostCheatKey, hostCheats);
    put(kLobbyBlockKey, EncodeLobbyBlock(hostBlock));
    if (changed) HSM_LOG("lobby: mod rule announced: %s%s%s", RuleSummary(rule).c_str(), hostCheats.empty() ? "" : "; host runs ", hostCheats.c_str());
}

void Lobby::SetOfficial(Official o) {
    if (!InLobby()) return;
    const char* name = OfficialName(o);
    if (officialSent_ == name) return;
    direct::SetMemberData(lobby_, kMemberOfficialKey, name);
    officialSent_ = name;
}

bool Lobby::Progression() const { return InLobby() && direct::LobbyData(lobby_, "hsm_prog") == "1"; }
void Lobby::SetProgression(bool on) { if (IsHost()) direct::SetLobbyData(lobby_, "hsm_prog", on ? "1" : "0"); }

void Lobby::SetTeamPick(int team) { if (InLobby()) direct::SetMemberData(lobby_, "hsm_team", std::to_string(team)); }

void Lobby::SetOverride(direct::UserId member, int team) {
    if (IsHost()) direct::SetLobbyData(lobby_, "hsm_ovr_" + std::to_string(member), std::to_string(team));
}

void Lobby::ClearOverrides() { for (const LobbyMember& m : members_) SetOverride(m.id, -1); }

void Lobby::SetReady(bool ready) { if (InLobby()) direct::SetMemberData(lobby_, "ready", ready ? "1" : "0"); }
int Lobby::SeverEase() const { return InLobby()?std::clamp(IntOr(direct::LobbyData(lobby_,"hsm_sever"),100),25,400):100; }
void Lobby::SetSeverEase(int pct) { if(IsHost()) direct::SetLobbyData(lobby_,"hsm_sever",std::to_string(std::clamp(pct,25,400))); }
bool Lobby::SetDisplayName(const std::string& name) {
    const auto clean=CleanDisplayName(name);
    if(clean.empty() || !direct::SetDisplayName(clean)) { SetStatus("Enter a display name (up to 32 UTF-8 bytes)"); return false; }
    SaveIniValues({{"name",clean}});
    SetStatus("Display name updated"); return true;
}
LobbyBlock Lobby::CurrentBlock() const { return InLobby() ? DecodeLobbyBlock(direct::LobbyData(lobby_, kLobbyBlockKey)) : hostBlock; }
bool Lobby::InUnrestrictedLobby() const { return InLobby() && direct::LobbyData(lobby_, kLobbyModRuleKey) == std::to_string(int(ModPreset::Unrestricted)); }
bool Lobby::InClassesLobby() const { return InLobby() && (InUnrestrictedLobby() || direct::LobbyData(lobby_, kLobbyModRuleKey) == std::to_string(int(ModPreset::Classes))); }

void Lobby::RepublishMyCard() {
    if (!InLobby() || !myCard) return;
    const std::string card = myCard();
    if (direct::MemberData(lobby_, direct::MyUserId(), "hsm_card") != card) direct::SetMemberData(lobby_, "hsm_card", card);
    if (myRuleSheet) {
        const std::string sheet = myRuleSheet();
        if (direct::MemberData(lobby_, direct::MyUserId(), "hsm_sheet") != sheet) direct::SetMemberData(lobby_, "hsm_sheet", sheet);
    }
}


void Lobby::SetPublicState(const std::string& state, const std::string& map, const std::string& mode) {
    if (!IsHost()) return;
    direct::SetLobbyData(lobby_, "state", state); direct::SetLobbyData(lobby_, "map", map); direct::SetLobbyData(lobby_, "mode", mode);
}

void Lobby::RefreshList() {
    if (state_ == State::Offline) return;
    SetStatus("Checking configured host...");
    direct::RequestLobbyList("hsm_proto", ProtocolStr());
}

void Lobby::RefreshMembers() {
    direct::UserId owner = direct::LobbyOwner(lobby_);
    if (owner && owner != owner_) {           // host left and Steam promoted someone
        const direct::UserId old = owner_;
        owner_ = owner;
        if (!onOwnerChanged || !onOwnerChanged(old, owner)) {
            SetStatus("Host left the lobby");
            Leave();
            return;
        }
        SetStatus(owner == direct::MyUserId() ? "The host left. You are the new host." : "The host left. " + (direct::PersonaName(owner).empty() ? std::string("Another player") : direct::PersonaName(owner)) + " is the new host.");
    }
    std::vector<direct::UserId> ids = direct::LobbyMembers(lobby_);
    const bool synced = !members_.empty();   // the first refresh after Enter lists whoever was here: not "joined" lines
    for (const LobbyMember& m : members_) {
        if (std::find(ids.begin(), ids.end(), m.id) != ids.end() || m.id == direct::MyUserId()) continue;
        if (onMemberLeft) onMemberLeft(m.id);
        if (onPresence) onPresence(m.name, false);
    }
    std::vector<LobbyMember> next;
    for (direct::UserId id : ids) {
        if (!id) continue;
        LobbyMember m; m.id = id; m.name = direct::PersonaName(id); m.host = id == owner_;
        m.ready = direct::MemberData(lobby_, id, "ready") == "1";
        m.build = direct::MemberData(lobby_, id, "hsm_mod").substr(0, kMaxModBuild);   // peer-written
        m.card = direct::MemberData(lobby_, id, "hsm_card").substr(0, kMaxCardValue);   // peer-written; decoded by hsm_build_card.h
        auto was = std::find_if(members_.begin(), members_.end(), [&](const LobbyMember& o) { return o.id == id; });
        if (!m.card.empty() && onBuildCard && (was == members_.end() || was->card != m.card)) onBuildCard(m.name, m.card);
        auto team = [](int t) { return t >= -1 && t <= 3 ? t : -1; };   // peer-written; later narrowed to int8_t
        m.team = team(IntOr(direct::MemberData(lobby_, id, "hsm_team"), -1));
        m.hostOverride = team(IntOr(direct::LobbyData(lobby_, "hsm_ovr_" + std::to_string(id)), -1));
        bool known = std::any_of(members_.begin(), members_.end(), [&](const LobbyMember& o) { return o.id == id; });
        if (!known && id && id != direct::MyUserId()) {
            if (onMemberJoined) onMemberJoined(id);
            if (synced && onPresence) onPresence(m.name, true);
        }
        next.push_back(m);
    }
    members_ = std::move(next);
}

}  // namespace hsm::game
