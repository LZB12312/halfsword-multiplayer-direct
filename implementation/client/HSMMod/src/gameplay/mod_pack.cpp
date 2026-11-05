#include "gameplay/mod_pack.h"
#include "gameplay/lobby.h"
#include "gameplay/mod_maps.h"
#include "log.h"

namespace hsm::game::modpack {
namespace {
std::string g_configured;
direct::UserId g_decidedFor = 0;     // the lobby the host has already decided about
std::string g_lastJoinLog;

const char* Reason(const PackCheck& c) {
    switch (c.verdict) {
        case PackVerdict::NotInstalled: return "not installed";
        case PackVerdict::BadManifest: return "its HSMPack.ini is not valid";
        case PackVerdict::DifferentPack: return "another version";
        case PackVerdict::Pending: return "still checking";
        default: return "";
    }
}

// The log line for a verdict that is about one pack; `what` names whose copy (the host's own, or the lobby's).
void LogVerdict(const std::string& pack, const PackCheck& c) {
    switch (c.verdict) {
        case PackVerdict::Ok: HSM_LOG("pack: ok"); break;
        case PackVerdict::MissingPak: HSM_LOG("pack: missing %s", c.file.c_str()); break;
        case PackVerdict::DifferentPak: HSM_LOG("pack: %s sha256 differs", c.file.c_str()); break;
        case PackVerdict::NoPack: break;
        default: HSM_LOG("pack: %s: %s", pack.c_str(), Reason(c)); break;
    }
}
}  // namespace

void SetConfigured(const std::string& name) { g_configured = name; }

PackCheck Check(const LobbyPack& lobby) {
    return CheckPack(lobby, modmaps::Packs(), modmaps::Paks(), modmaps::Listed());
}

void LogJoinCheck(const LobbyPack& lobby, const PackCheck& check) {
    if (!lobby.any() || check.verdict == PackVerdict::Pending) return;
    const std::string key = lobby.name + "/" + lobby.version + "/" + lobby.hash + "/" + std::to_string(int(check.verdict)) + "/" + check.file;
    if (key == g_lastJoinLog) return;
    g_lastJoinLog = key;
    HSM_LOG("pack: lobby pack %s %s", lobby.name.c_str(), lobby.version.c_str());
    LogVerdict(lobby.name, check);
}

void Tick(Lobby& lobby) {
    if (!lobby.IsHost()) { g_decidedFor = 0; return; }
    if (g_decidedFor == lobby.id() || !modmaps::Listed()) return;
    std::string why;
    const LocalPack* pick = ChooseHostPack(modmaps::Packs(), g_configured, why);
    if (!pick) {
        g_decidedFor = lobby.id();
        if (!why.empty()) HSM_LOG("pack: no pack announced (%s)", why.c_str());
        lobby.SetPack(LobbyPack{});
        return;
    }
    // The host's own files must be what its manifest says, or the lobby would promise a pack nobody could match.
    const LobbyPack mine = LobbyPackOf(pick->manifest);
    const PackCheck c = Check(mine);
    if (c.verdict == PackVerdict::Pending) return;
    g_decidedFor = lobby.id();
    if (c.verdict != PackVerdict::Ok) {
        LogVerdict(mine.name, c);
        HSM_LOG("pack: no pack announced (the files of %s do not match its HSMPack.ini)", mine.name.c_str());
        lobby.SetPack(LobbyPack{});
        return;
    }
    lobby.SetPack(mine);
    HSM_LOG("pack: host pack %s %s (%zu paks)", mine.name.c_str(), mine.version.c_str(), pick->manifest.paks.size());
    LogVerdict(mine.name, c);
}

}  // namespace hsm::game::modpack
