// Transports. ITransport is what the session uses; DirectTransport (ENet UDP through the host) implements it. Incoming payloads are validated
// before they are queued for the game thread.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hsm {

struct NetEvent {
    enum Kind { Connected, Disconnected, Data } kind = Data;
    uint32_t peer = 0;
    uint64_t steamId = 0;   // set by DirectTransport
    uint8_t channel = 0;
    std::vector<uint8_t> data;
    double rxMs = 0;        // Data: when it arrived, on the steady clock (NetSteadyMs; 0 = unknown, use the poll time)
};

// Milliseconds on std::chrono::steady_clock, the clock of game::SteadyNowMs (owner_sync.cpp), from any thread.
inline double NetSteadyMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

class ITransport {
public:
    using Validator = std::function<bool(const uint8_t* data, size_t size, uint8_t channel)>;
    virtual ~ITransport() = default;
    virtual void Stop() = 0;
    virtual void Send(uint32_t peer, uint8_t channel, std::vector<uint8_t> data, bool reliable) = 0;
    virtual void Broadcast(uint8_t channel, std::vector<uint8_t> data, bool reliable) = 0;
    virtual void Disconnect(uint32_t peer) = 0;
    virtual bool Poll(NetEvent& out) = 0;
    virtual uint32_t RoundTripMs(uint32_t peer) = 0;
};

}  // namespace hsm
