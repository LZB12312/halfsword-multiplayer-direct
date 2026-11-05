// Direct-IP backend. All functions run on the game thread, except the UPnP worker.
// User IDs are host-assigned session identities, not authenticated Steam accounts.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hsm::direct {

using UserId = uint64_t;
using ApiCall = uint64_t;

enum class LobbyType : int { Private = 0, FriendsOnly = 1, Public = 2, Invisible = 3 };

struct Event {
    enum Kind {
        LobbyCreated,         // ok, lobby
        LobbyEntered,         // ok, lobby
        LobbyMembersChanged,  // lobby, user, a = state change bits (1 entered, 2 left, 4 disconnected, 8 kicked, 16 banned)
        LobbyDataChanged,     // lobby, user (== lobby for lobby-wide data)
        LobbyChat,            // lobby, user, text
        JoinRequested,        // lobby (friend clicked Join / accepted invite)
        LobbyList,            // lobbies
        SessionRequest,       // user wants a networking session
        SessionFailed,        // user
    } kind;
    bool ok = true;
    UserId lobby = 0, user = 0;
    uint32_t a = 0;
    std::string text;
    std::vector<UserId> lobbies;
};

struct Message { UserId from = 0; int channel = 0; std::vector<uint8_t> data; double ageMs = -1; };   // ageMs: since Steam received it (-1 unknown)

enum SendFlags : int { kUnreliable = 0, kNoNagle = 1, kReliable = 8, kAutoRestart = 32 };

// Lifecycle (game thread). Init initializes ENet/Winsock and reads local settings.
bool Init();
bool Ready();
std::string ConnectAddress();
bool SetConnectAddress(const std::string& address);
std::string ConnectionInfo();
std::string ShareAddress();
std::string LanAddress();
bool SetDisplayName(const std::string& name);
std::string RemoteAddress(UserId user);
void DisconnectUser(UserId user); // host moderation: remove a connection and lobby member
void Tick();                      // polls call results; pumps callbacks if the game does not
bool Poll(Event& out);            // drain queued callbacks/results

UserId MyUserId();
std::string PersonaName(UserId user);   // empty for unknown
uint32_t AppId();
std::string LaunchConnectLobby();        // "+connect_lobby <id>" from the command line, if any

// Lobbies (results arrive as Events)
void CreateLobby(LobbyType type, int maxMembers);
void JoinLobby(UserId lobby);
void LeaveLobby(UserId lobby);
bool SetLobbyType(UserId lobby, LobbyType type);
bool SetLobbyJoinable(UserId lobby, bool joinable);
bool SetLobbyMemberLimit(UserId lobby, int maxMembers);   // optional export; false without it (the host's own check stays)
int LobbyMemberLimit(UserId lobby);                       // 0 unknown
bool SetLobbyData(UserId lobby, const std::string& key, const std::string& value);
std::string LobbyData(UserId lobby, const std::string& key);
void SetMemberData(UserId lobby, const std::string& key, const std::string& value);
std::string MemberData(UserId lobby, UserId user, const std::string& key);
std::vector<UserId> LobbyMembers(UserId lobby);
UserId LobbyOwner(UserId lobby);
bool SendLobbyChat(UserId lobby, const std::string& text);
void RequestLobbyList(const std::string& filterKey, const std::string& filterValue);
void InviteOverlay(UserId lobby);
void SetRichPresence(const std::string& key, const std::string& value);

// Compatibility hooks: direct sessions have no platform avatars.
int MediumAvatar(UserId user);
bool AvatarRGBA(int image, uint32_t& width, uint32_t& height, std::vector<uint8_t>& rgba);

// Compatibility hooks: return false. Voice uses Windows WaveIn and the mod's ADPCM codec.
bool VoiceAvailable();
void StartVoice();
void StopVoice();
// The compressed bytes recorded since the last call; empty when there are none. false: Steam refused (logged once).
bool TakeVoice(std::vector<uint8_t>& out);
// One compressed chunk (anyone's) to 16-bit mono PCM at sampleRate. false: corrupt or unsupported.
bool DecompressVoice(const uint8_t* data, size_t size, uint32_t sampleRate, std::vector<int16_t>& pcm);

// ENet UDP, reliable and unreliable channels. Clients connect only to the host.
void InitRelay();
bool Send(UserId to, int channel, const void* data, uint32_t size, int flags);
size_t Receive(int channel, std::vector<Message>& out, int max = 64);
void AcceptSession(UserId user);
void CloseSession(UserId user);
int PingMs(UserId user);                // -1 if unknown

}  // namespace hsm::direct
