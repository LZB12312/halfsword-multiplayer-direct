// Which way a Steam session's packets go: straight between the two machines, or through a Valve relay (SDR) and
// which relay. Read from SteamNetConnectionInfo_t (m_nFlags, m_idPOPRelay, m_idPOPRemote) for the health log, so a
// tester's log shows whether a high ping is the route or the distance. Pure: the flat API call is in steam_api.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

namespace hsm::steam {

constexpr int32_t kConnFlagFast = 8;       // k_nSteamNetworkConnectionInfoFlags_Fast: same LAN or localhost (probably)
constexpr int32_t kConnFlagRelayed = 16;   // k_nSteamNetworkConnectionInfoFlags_Relayed: SDR or TURN

// A data-centre code such as "fra" or "iad" (GetSteamNetworkingLocationPOPStringFromID). Empty for 0 or a code with
// characters that are not printable.
inline std::string PopName(uint32_t id) {
    if (!id) return {};
    const char c[4] = {char(id >> 16), char(id >> 8), char(id), char(id >> 24)};
    std::string s;
    for (char ch : c) {
        if (!ch) break;
        if (ch < 0x21 || ch > 0x7e) return {};
        s += ch;
    }
    return s;
}

struct Route {
    bool relayed = false, lan = false;
    uint32_t relayPop = 0, remotePop = 0;
    bool operator==(const Route& o) const {
        return relayed == o.relayed && lan == o.lan && relayPop == o.relayPop && remotePop == o.remotePop;
    }
};

inline Route RouteFrom(int32_t flags, uint32_t relayPop, uint32_t remotePop) {
    Route r;
    r.relayed = (flags & kConnFlagRelayed) != 0 || relayPop != 0;
    r.lan = (flags & kConnFlagFast) != 0;
    r.relayPop = relayPop;
    r.remotePop = remotePop;
    return r;
}

// "direct", "direct (LAN)", "relayed via fra", "relayed via fra, far end near iad".
inline std::string Describe(const Route& r) {
    if (!r.relayed) return r.lan ? "direct (LAN)" : "direct";
    std::string s = "relayed";
    const std::string via = PopName(r.relayPop), far = PopName(r.remotePop);
    if (!via.empty()) s += " via " + via;
    if (!far.empty() && far != via) s += ", far end near " + far;
    return s;
}

// The last route seen per peer: Note() is true the first time and whenever it changes (a line to log).
class RouteWatch {
public:
    bool Note(uint64_t peer, const Route& r) {
        auto [it, fresh] = last_.try_emplace(peer, r);
        if (fresh) return true;
        if (it->second == r) return false;
        it->second = r;
        return true;
    }
    void Forget(uint64_t peer) { last_.erase(peer); }
    void Clear() { last_.clear(); }

private:
    std::unordered_map<uint64_t, Route> last_;
};

}  // namespace hsm::steam
