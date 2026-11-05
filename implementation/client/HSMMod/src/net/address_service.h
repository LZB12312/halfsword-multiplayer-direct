#pragma once
#include <mutex>
#include <thread>
#include <string>
namespace hsm::net {
class AddressService {
public:
    ~AddressService() { Stop(); }
    void Start(bool publicLookup=true);
    void Stop() { if(worker_.joinable()) worker_.join(); }
    std::string Public() const { std::lock_guard<std::mutex> lock(mu_); return public_; }
    std::string Local() const { std::lock_guard<std::mutex> lock(mu_); return local_; }
private:
    mutable std::mutex mu_;
    std::thread worker_;
    std::string public_,local_;
};
bool CopyText(const std::string& text);
std::string ClipboardText();
}
