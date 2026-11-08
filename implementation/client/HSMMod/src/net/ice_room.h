#pragma once
#include <cstdint>
#include <string>

namespace hsm::net::ice {

enum class Phase { Idle, Connecting, Ready, Failed };
struct State {
    Phase phase = Phase::Idle;
    uint16_t port = 0;
    std::string code, error;
};

// Called on the game thread. The worker owns signalling and all bridge sockets.
std::string Start(const std::string& service, bool host, const std::string& code, uint16_t gamePort);
State Status();
void SetGamePort(uint16_t port);
void Stop();

}
