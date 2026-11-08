#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace hsm::net::ice {

class Channel {
public:
    explicit Channel(std::string context);
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    const std::string& PublicKey() const { return publicKey_; }
    bool Accept(const std::string& publicKey);
    std::vector<uint8_t> Seal(const uint8_t* data, size_t size);
    std::vector<uint8_t> Open(const uint8_t* data, size_t size);

private:
    bool Crypt(bool encrypt, BCRYPT_KEY_HANDLE key, const uint8_t* nonce,
               uint8_t* tag, const uint8_t* in, size_t size, uint8_t* out);
    std::string context_, publicKey_;
    BCRYPT_ALG_HANDLE ecdh_ = nullptr, aes_ = nullptr;
    BCRYPT_KEY_HANDLE privateKey_ = nullptr, sendKey_ = nullptr, receiveKey_ = nullptr;
    uint64_t sequence_ = 0, highest_ = 0;
    std::set<uint64_t> seen_;
};

}
