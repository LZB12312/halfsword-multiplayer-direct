// Validated gameplay messages over the direct ENet backend. Host relays traffic; game thread only.
#pragma once
#include "net/transport.h"
#include <unordered_map>

namespace hsm {

class DirectTransport final : public ITransport {
public:
    explicit DirectTransport(Validator v) : validate_(std::move(v)) {}
    ~DirectTransport() override { Stop(); }

    uint32_t AllowPeer(uint64_t steamId);      // returns peer id; queues a Connected event the first time
    void DropPeer(uint64_t steamId);           // queues Disconnected and closes the Steam session
    bool IsAllowed(uint64_t steamId) const { return byId_.count(steamId) != 0; }
    uint32_t PeerOf(uint64_t steamId) const;
    uint64_t UserOf(uint32_t peer) const;

    void Stop() override;
    void Send(uint32_t peer, uint8_t channel, const std::vector<uint8_t>& data, bool reliable) override;
    void Broadcast(uint8_t channel, const std::vector<uint8_t>& data, bool reliable) override;
    void Disconnect(uint32_t peer) override;
    bool Poll(NetEvent& out) override;
    uint32_t RoundTripMs(uint32_t peer) override;

private:
    void Pump();
    Validator validate_;
    std::unordered_map<uint64_t, uint32_t> byId_;
    std::unordered_map<uint32_t, uint64_t> bySlot_;
    uint32_t nextPeer_ = 1;
    bool sendFailLogged_ = false;
    std::vector<NetEvent> queue_;
    size_t head_ = 0;
};

}  // namespace hsm
