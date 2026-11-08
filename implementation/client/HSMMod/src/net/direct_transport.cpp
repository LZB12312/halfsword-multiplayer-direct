#include "net/direct_transport.h"
#include "hsm_protocol.h"
#include "log.h"
#include "net/direct_api.h"

namespace hsm {

namespace {
constexpr double kMaxAgeMs = 5000;   // an older Steam receive stamp is not believed (the poll time is used)
}  // namespace

uint32_t DirectTransport::AllowPeer(uint64_t steamId) {
    if (!steamId) return 0;
    auto it = byId_.find(steamId);
    if (it != byId_.end()) return it->second;
    uint32_t peer = nextPeer_++;
    byId_[steamId] = peer; bySlot_[peer] = steamId;
    direct::AcceptSession(steamId);
    NetEvent e; e.kind = NetEvent::Connected; e.peer = peer; e.steamId = steamId;
    queue_.push_back(std::move(e));
    HSM_LOG("direct-net: peer %u = %llu (%s)", peer, (unsigned long long)steamId, direct::PersonaName(steamId).c_str());
    return peer;
}

void DirectTransport::DropPeer(uint64_t steamId) {
    auto it = byId_.find(steamId);
    if (it == byId_.end()) return;
    NetEvent e; e.kind = NetEvent::Disconnected; e.peer = it->second; e.steamId = steamId;
    bySlot_.erase(it->second); byId_.erase(it);
    direct::CloseSession(steamId);
    queue_.push_back(std::move(e));
}

uint32_t DirectTransport::PeerOf(uint64_t steamId) const { auto it = byId_.find(steamId); return it == byId_.end() ? 0 : it->second; }
uint64_t DirectTransport::UserOf(uint32_t peer) const { auto it = bySlot_.find(peer); return it == bySlot_.end() ? 0 : it->second; }

void DirectTransport::Stop() {
    for (auto& [id, peer] : byId_) direct::CloseSession(id);
    byId_.clear(); bySlot_.clear(); queue_.clear(); head_ = 0;
}

void DirectTransport::Send(uint32_t peer, uint8_t channel, const std::vector<uint8_t>& data, bool reliable) {
    uint64_t id = UserOf(peer);
    if (!id) return;
    int flags = direct::kAutoRestart | (reliable ? direct::kReliable : direct::kNoNagle);
    if (!direct::Send(id, channel, data.data(), uint32_t(data.size()), flags) && !sendFailLogged_) {
        sendFailLogged_ = true;
        HSM_LOG("direct-net: UDP send to %llu failed (further failures not logged)", (unsigned long long)id);
    }
}

void DirectTransport::Broadcast(uint8_t channel, const std::vector<uint8_t>& data, bool reliable) {
    for (auto& [peer, id] : bySlot_) Send(peer, channel, data, reliable);
}

void DirectTransport::Disconnect(uint32_t peer) { if (uint64_t id = UserOf(peer)) { direct::DisconnectUser(id); DropPeer(id); } }

void DirectTransport::Pump() {
    std::vector<direct::Message> msgs;
    for (int ch = 0; ch < kChanCount; ++ch) direct::Receive(ch, msgs);
    for (direct::Message& m : msgs) {
        uint32_t peer = PeerOf(m.from);
        if (!peer) continue;                                                       // not a lobby member we accepted
        if (m.data.size() > kMaxPacket || !validate_ || !validate_(m.data.data(), m.data.size(), uint8_t(m.channel))) continue;
        NetEvent e; e.kind = NetEvent::Data; e.peer = peer; e.steamId = m.from; e.channel = uint8_t(m.channel); e.data = std::move(m.data);
        if (m.ageMs >= 0 && m.ageMs <= kMaxAgeMs) e.rxMs = NetSteadyMs() - m.ageMs;   // Steam's own receive time
        queue_.push_back(std::move(e));
    }
}

bool DirectTransport::Poll(NetEvent& out) {
    if (head_ >= queue_.size()) { queue_.clear(); head_ = 0; Pump(); }
    if (head_ >= queue_.size()) return false;
    out = std::move(queue_[head_++]);
    return true;
}

uint32_t DirectTransport::RoundTripMs(uint32_t peer) {
    uint64_t id = UserOf(peer);
    int ping = id ? direct::PingMs(id) : -1;
    return ping > 0 ? uint32_t(ping) : 0;
}


}  // namespace hsm
