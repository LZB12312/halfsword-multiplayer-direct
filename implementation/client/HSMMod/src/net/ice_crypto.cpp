#include "net/ice_crypto.h"
#include <wincrypt.h>
#include <array>
#include <limits>
#include <utility>

namespace hsm::net::ice {
namespace {
constexpr size_t kPublicBytes = sizeof(BCRYPT_ECCKEY_BLOB) + 64;
constexpr size_t kNonce = 12, kTag = 16, kMaxPacket = 1400;
constexpr uint64_t kReplayWindow = 256;
}

Channel::Channel(std::string context) : context_(std::move(context)) {
    if (BCryptOpenAlgorithmProvider(&ecdh_, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0) ||
        BCryptGenerateKeyPair(ecdh_, &privateKey_, 256, 0) ||
        BCryptFinalizeKeyPair(privateKey_, 0) ||
        BCryptOpenAlgorithmProvider(&aes_, BCRYPT_AES_ALGORITHM, nullptr, 0)) return;
    wchar_t mode[] = BCRYPT_CHAIN_MODE_GCM;
    if (BCryptSetProperty(aes_, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(mode), sizeof mode, 0)) return;

    std::array<uint8_t, kPublicBytes> bytes{};
    ULONG written = 0;
    if (BCryptExportKey(privateKey_, nullptr, BCRYPT_ECCPUBLIC_BLOB, bytes.data(),
                        ULONG(bytes.size()), &written, 0) || written != bytes.size()) return;
    char encoded[129]{};
    DWORD length = sizeof encoded;
    if (CryptBinaryToStringA(bytes.data(), written, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, encoded, &length))
        publicKey_ = encoded;
}

Channel::~Channel() {
    if (sendKey_) BCryptDestroyKey(sendKey_);
    if (receiveKey_) BCryptDestroyKey(receiveKey_);
    if (privateKey_) BCryptDestroyKey(privateKey_);
    if (aes_) BCryptCloseAlgorithmProvider(aes_, 0);
    if (ecdh_) BCryptCloseAlgorithmProvider(ecdh_, 0);
}

bool Channel::Accept(const std::string& peer) {
    if (publicKey_.empty() || sendKey_ || receiveKey_ || peer.size() != 96 || peer == publicKey_) return false;
    std::array<uint8_t, kPublicBytes> bytes{};
    DWORD length = DWORD(bytes.size());
    if (!CryptStringToBinaryA(peer.data(), DWORD(peer.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT,
                              bytes.data(), &length, nullptr, nullptr) || length != bytes.size()) return false;
    BCRYPT_KEY_HANDLE publicKey = nullptr;
    if (BCryptImportKeyPair(ecdh_, nullptr, BCRYPT_ECCPUBLIC_BLOB, &publicKey, bytes.data(), length, 0)) return false;
    BCRYPT_SECRET_HANDLE agreement = nullptr;
    const auto agreed = BCryptSecretAgreement(privateKey_, publicKey, &agreement, 0);
    BCryptDestroyKey(publicKey);
    if (agreed) return false;

    // CNG keeps the ECDH secret in its handle and derives direction-specific keys.
    const auto derive = [&](std::string label, BCRYPT_KEY_HANDLE& key) {
        wchar_t hash[] = BCRYPT_SHA256_ALGORITHM;
        BCryptBuffer inputs[]{
            {sizeof hash, KDF_HASH_ALGORITHM, hash},
            {ULONG(label.size()), KDF_SECRET_PREPEND, label.data()}
        };
        BCryptBufferDesc params{BCRYPTBUFFER_VERSION, ULONG(std::size(inputs)), inputs};
        std::array<uint8_t, 32> material{};
        ULONG written = 0;
        const bool ok = !BCryptDeriveKey(agreement, BCRYPT_KDF_HMAC, &params, material.data(),
                                        ULONG(material.size()), &written, KDF_USE_SECRET_AS_HMAC_KEY_FLAG)
            && written == material.size()
            && !BCryptGenerateSymmetricKey(aes_, &key, nullptr, 0, material.data(), ULONG(material.size()), 0);
        SecureZeroMemory(material.data(), material.size());
        return ok;
    };
    const bool ok = derive(context_ + ":" + publicKey_ + ">" + peer, sendKey_)
        && derive(context_ + ":" + peer + ">" + publicKey_, receiveKey_);
    BCryptDestroySecret(agreement);
    return ok;
}

bool Channel::Crypt(bool encrypt, BCRYPT_KEY_HANDLE key, const uint8_t* nonce,
                    uint8_t* tag, const uint8_t* in, size_t size, uint8_t* out) {
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<uint8_t*>(nonce);
    info.cbNonce = kNonce;
    info.pbAuthData = reinterpret_cast<PUCHAR>(context_.data());
    info.cbAuthData = ULONG(context_.size());
    info.pbTag = tag;
    info.cbTag = kTag;
    ULONG written = 0;
    const auto crypt = encrypt ? BCryptEncrypt : BCryptDecrypt;
    return !crypt(key, const_cast<uint8_t*>(in), ULONG(size), &info, nullptr, 0, out, ULONG(size), &written, 0)
        && written == size;
}

std::vector<uint8_t> Channel::Seal(const uint8_t* data, size_t size) {
    if (!sendKey_ || !receiveKey_ || !size || size > kMaxPacket || sequence_ == std::numeric_limits<uint64_t>::max()) return {};
    std::vector<uint8_t> packet(kNonce + size + kTag);
    auto sequence = sequence_++;
    for (size_t i = kNonce; i > 4;) {
        packet[--i] = uint8_t(sequence);
        sequence >>= 8;
    }
    if (!Crypt(true, sendKey_, packet.data(), packet.data() + kNonce + size, data, size, packet.data() + kNonce)) return {};
    return packet;
}

std::vector<uint8_t> Channel::Open(const uint8_t* data, size_t size) {
    if (!receiveKey_ || size <= kNonce + kTag || size > kNonce + kMaxPacket + kTag) return {};
    if (data[0] || data[1] || data[2] || data[3]) return {};
    uint64_t sequence = 0;
    for (size_t i = 4; i < kNonce; ++i) sequence = (sequence << 8) | data[i];
    if ((sequence <= highest_ && highest_ - sequence >= kReplayWindow) || seen_.contains(sequence)) return {};
    std::array<uint8_t, kTag> tag{};
    for (size_t i = 0; i < kTag; ++i) tag[i] = data[size - kTag + i];
    std::vector<uint8_t> clear(size - kNonce - kTag);
    if (!Crypt(false, receiveKey_, data, tag.data(), data + kNonce, clear.size(), clear.data())) return {};
    if (sequence > highest_) highest_ = sequence;
    seen_.insert(sequence);
    while (!seen_.empty() && highest_ - *seen_.begin() >= kReplayWindow) seen_.erase(seen_.begin());
    return clear;
}
}
