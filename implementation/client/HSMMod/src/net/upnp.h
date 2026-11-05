#pragma once
#include <cstdint>
#include <thread>
#include <atomic>
namespace hsm::net {
// MiniUPnPc discovery and lease renewal away from the game thread. Stop removes
// only the mapping this instance created, never a pre-existing router rule.
class UpnpMapping {
public:
    ~UpnpMapping() { Stop(); }
    void Start(uint16_t port);
    void Stop();
private:
    std::thread worker_;
    std::atomic<bool> stop_{false};
};
}
