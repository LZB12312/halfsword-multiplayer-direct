// Steam lobby lifecycle for the in-game Multiplayer screen. The lobby is the source of truth for who is in the
// session; the session layer mirrors membership into the transport. Game thread only.
#pragma once
#include "match/hsm_online_rules.h"
#include "match/hsm_match.h"
#include "session/hsm_mod_rules.h"
#include "session/hsm_modpack.h"
#include "net/direct_api.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hsm::game {

constexpr int kMaxLobbyMembers = 8;   // players: == kMaxPlayers (session/hsm_roster.h; static_assert in session.cpp).
                                      // A role=server lobby has one member more: the server, which is not a player.
#ifdef HSM_TESTBENCH
constexpr const char* kModVersion = "0.4.1-pre1-test";
#else
constexpr const char* kModVersion = "0.4.1-pre1";   // bump with every release tag
#endif
// This mod's build: kModVersion, the git describe build.sh passed and the DLL file's hash (session/hsm_build.h). Players
// compare it.
std::string ModBuild();
// allow_build_mismatch in HSMMod.ini (config.h): the host may start a match with other builds in. Set once at start.
void SetAllowBuildMismatch(bool on);
bool AllowBuildMismatch();

struct LobbyMember { direct::UserId id = 0; std::string card; std::string name; bool ready = false; int team = -1; int hostOverride = -1; bool host = false; std::string build; };

struct LobbyListing {
    direct::UserId id = 0;
    std::string name, map, mode;
    int players = 0;      // players: a role=server lobby's server is not counted
    int maxPlayers = kMaxLobbyMembers;   // the host's player limit (hsm_max); 8 when the lobby has none
    bool locked = false;  // asks for a password (hsm_lock); the password is never listed
    LobbyPack pack;       // the modpack the host announced (hsm_pack/hsm_packv/hsm_packh); empty: none. Checked live, not here
    bool banned = false;  // the owner is on the global ban list (session/hsm_global_bans.h): left out of the list
    bool compatible = false;
    std::string reason;   // why it is not joinable, if !compatible
    std::string mods;     // the lobby's mod rule tag ("mods: Vanilla"), empty for the default (session/hsm_mod_rules.h LobbyRuleTag)
    std::string modsText; // the mod rules' full sentence for this machine (what to remove), when they turn it away
};

class Lobby {
public:
    enum class State { Offline, Idle, Creating, Joining, InLobby };

    // Membership callbacks for the session (fired on the game thread).
    std::function<void(direct::UserId)> onMemberJoined, onMemberLeft;
    std::function<void()> onEntered, onLeft;
    // The host left and Steam made another member the owner. True: the session migrates to the new owner and the lobby
    // stays; false (or unset): leave the lobby, as before 0.7.0-pre5.
    std::function<bool(direct::UserId oldOwner, direct::UserId newOwner)> onOwnerChanged;
    std::function<void(direct::UserId from, const std::string& text)> onChat;   // a lobby chat line (ours too); the text is unchecked
    LobbyBlock hostBlock;                        // how strict this machine's lobbies are (block_hse=, mod_block=)
    LobbyBlock CurrentBlock() const;             // this lobby's, from its host; hostBlock outside one
    ModPreset hostPreset = ModPreset::MapsOnly;  // the rule of this machine's lobbies (mod_rule=)
    bool InClassesLobby() const;                 // the lobby this machine is in uses classes (mod_rule=classes or unrestricted)
    bool InUnrestrictedLobby() const;            // ...with no class power budget (mod_rule=unrestricted)
    std::function<std::string()> myRuleSheet;   // this machine's class settings (hsm_online_rules.h)
    std::function<std::string(ModPreset, LobbyBlock)> onlineBlock;   // why this machine may not play online in this kind of lobby; empty: it may
    std::function<void()> onEnterCard;   // a new lobby: the old lobby's cards go
    void RepublishMyCard();   // the card again, if it changed since it was sent
    std::function<std::string()> myCard;                                        // this machine's build card (hsm_build_card.h), asked for on entering a lobby
    std::function<void(const std::string& name, const std::string& card)> onBuildCard;   // a member's card arrived or changed; peer-written
    std::function<void(const std::string& name, bool joined)> onPresence;       // someone else came or went, after we entered

    void Tick();                                   // drives Steam init + callback processing

    void Host(direct::LobbyType type, bool server = false);   // server: role=server, one member more (hsm_srv=1)
    void Join(direct::UserId lobby, const std::string& password = std::string());   // password: for a locked lobby (kept for this lobby only)
    void JoinAddress(const std::string& address);
    void HostIce();
    void JoinIce(const std::string& code);
    void Leave(bool keepIce = false);
    void Invite();
    bool SendChat(const std::string& text);        // to every member (Steam lobby chat); false: not in a lobby or Steam refused
    void SetType(direct::LobbyType type);
    void SetReady(bool ready);
    bool SetDisplayName(const std::string& name);
    int SeverEase() const;
    void SetSeverEase(int pct);
    // The host's player limit, 2..8 (Steam's member limit and lobby data hsm_max). Never below the players already in:
    // nobody is kicked by it. Before the lobby exists it is what Host creates it with.
    int MaxPlayers() const;
    void SetMaxPlayers(int n);
    // Lobby password. Locked is the public flag (hsm_lock); the password itself is never in lobby data. The host sets it
    // (SetPassword, "" clears; nobody is kicked), a joiner has the one it joined with, and every member keeps the host's
    // latest (RememberPassword, from LobbySecret) so a new host after a migration keeps the lobby locked.
    bool Locked() const;
    const std::string& Password() const;            // this lobby's; empty: none known
    void SetPassword(const std::string& password);  // host
    void RememberPassword(const std::string& password);   // member: the host's latest
    // A locked lobby was joined without (or with a wrong) password: the page asks for it, then Join(lobby, password).
    void AskPassword(direct::UserId lobby, const std::string& note = std::string());
    direct::UserId PasswordAsk() const { return askLobby_; }
    const std::string& PasswordNote() const { return askNote_; }
    void CancelPasswordAsk() { askLobby_ = 0; askNote_.clear(); }
    void SetPublicState(const std::string& state, const std::string& map, const std::string& mode);
    void RefreshList();
    // Match settings (lobby data, host writes) and teams (own pick in member data, host moves in lobby data).
    MatchConfig config() const;
    void SetConfig(const MatchConfig& c);
    std::string MapPick() const;                    // host's chosen level (package path), empty until picked
    void SetMapPick(const std::string& path);
    LobbyPack Pack() const;                         // the host's announced modpack (session/hsm_modpack.h); empty: none
    void SetPack(const LobbyPack& pack);            // host: announce it (empty: none); gameplay/mod_pack.cpp
    // Host: announce the lobby's mod rule and the host's own cheat mods (gameplay/mod_rules.cpp LobbyTick); only changed
    // keys are written.
    void SetModRule(const ModRule& rule, const std::string& hostCheats);
    // This lobby's rule as its lobby data names it (what a member is held to; the MODS tab). `named`: the host wrote one
    // (a lobby without hsm_mrule reads as NoRule).
    ModRule RoomRule(bool* named = nullptr) const;
    // Any member: this DLL's official-build verdict in member data (kMemberOfficialKey); written when it changes, since
    // the update check usually answers after the lobby is entered.
    void SetOfficial(Official o);
    bool Progression() const;                       // the host chose the Progression card (hsm_prog)
    void SetProgression(bool on);
    void SetTeamPick(int team);
    void SetOverride(direct::UserId member, int team);
    void ClearOverrides();

    State state() const { return state_; }
    bool IsHost() const { return state_ == State::InLobby && owner_ == direct::MyUserId(); }
    bool InLobby() const { return state_ == State::InLobby; }
    bool ServerLobby() const;                       // a role=server lobby: the owner is the server, not a player
    int PlayerCount() const;                        // members, minus the server in a role=server lobby
    direct::UserId id() const { return lobby_; }
    direct::UserId owner() const { return owner_; }
    direct::LobbyType type() const { return type_; }
    const std::vector<LobbyMember>& members() const { return members_; }
    const std::vector<LobbyListing>& listings() const { return listings_; }
    const std::string& status() const { return status_; }

private:
    void Enter(direct::UserId lobby);
    void RefreshMembers();
    void SetStatus(const std::string& s);
    LobbyListing Describe(direct::UserId lobby) const;
    std::string password_;                // the lobby's password as this machine knows it, for passwordLobby_
    direct::UserId passwordLobby_ = 0;
    direct::UserId askLobby_ = 0;         // the lobby the page should ask a password for
    std::string askNote_;                 // why it asks again ("Wrong password")
    int maxPlayers_ = kMaxLobbyMembers;   // the host's choice; hsm_max is what everyone reads
    std::string officialSent_;   // the kMemberOfficialKey value last written (SetOfficial)
    bool server_ = false;   // Host(type, true): this machine runs role=server
    State state_ = State::Offline;
    direct::UserId lobby_ = 0, owner_ = 0, joining_ = 0;   // joining_: the lobby Join asked for, until its enter
    direct::LobbyType type_ = direct::LobbyType::FriendsOnly;
    std::vector<LobbyMember> members_;
    std::vector<LobbyListing> listings_;
    std::string status_ = "Network not ready";
    bool launchChecked_ = false;
};

}  // namespace hsm::game
