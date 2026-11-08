#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <juice/juice.h>
#include "net/ice_room.h"
#include "net/ice_crypto.h"
#include "agent/hsm_agent.h"
#include "session/hsm_version.h"
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <string_view>

namespace hsm::net::ice {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr size_t kMaxSignal = 8192, kMaxDatagram = 1400;
bool InAlphabet(const std::string& s, const char* alphabet) { return s.find_first_not_of(alphabet) == std::string::npos; }
bool PeerId(const std::string& s) { return s.size() == 36 && InAlphabet(s, "0123456789abcdef-"); }

struct Signal {
    std::string kind, id, from, sdp, key;
};

// The service sends objects containing strings and, for a description, one nested object.
bool ReadObject(const std::string& s, size_t& i, std::map<std::string, std::string>& fields,
                std::map<std::string, std::string>* payload) {
    using namespace version_detail;
    SkipSpace(s, i);
    if (i == s.size() || s[i++] != '{') return false;
    for (size_t count = 0; count < 8; ++count) {
        SkipSpace(s, i);
        bool tooLong = false;
        std::string key, value;
        if (i == s.size() || s[i] != '"' || !ReadString(s, i, 32, key, tooLong) || tooLong) return false;
        SkipSpace(s, i);
        if (i == s.size() || s[i++] != ':') return false;
        SkipSpace(s, i);
        if (i == s.size()) return false;
        if (key == "payload" && payload) {
            if (!payload->empty() || !ReadObject(s, i, *payload, nullptr)) return false;
        } else if (s[i] != '"' || !ReadString(s, i, 4096, value, tooLong) || tooLong ||
                   !fields.emplace(key, std::move(value)).second) return false;
        SkipSpace(s, i);
        if (i == s.size()) return false;
        const char next = s[i++];
        if (next == '}') return true;
        if (next != ',') return false;
    }
    return false;
}

bool ReadSignal(const std::string& raw, Signal& out) {
    std::map<std::string, std::string> fields, payload;
    size_t offset = 0;
    if (!ReadObject(raw, offset, fields, &payload)) return false;
    version_detail::SkipSpace(raw, offset);
    if (offset != raw.size()) return false;
    out = {fields["kind"], fields["id"], fields["from"], payload["sdp"], payload["key"]};
    return out.kind != "signal" || (payload["kind"] == "description" && !out.sdp.empty() &&
                                    out.sdp.find('\0') == std::string::npos && out.key.size() == 96);
}

struct Job {
    std::mutex mutex;
    State state;
    std::atomic<bool> stop{false}, finished{false};
    std::atomic<uint16_t> gamePort{0};
    void Update(Phase phase, std::string error = {}, uint16_t port = 0) {
        std::lock_guard lock(mutex);
        state.phase = phase;
        state.error = std::move(error);
        state.port = port;
    }
};

// Lifetime is the game process. The worker never needs to join on the game thread.
std::shared_ptr<Job>& ActiveJob() {
    static auto* job = new std::shared_ptr<Job>();
    return *job;
}

struct HttpHandle {
    HINTERNET value = nullptr;
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
    HttpHandle() = default;
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
};

class Wire {
public:
    ~Wire() {
        if (reader_.joinable()) {
            WinHttpWebSocketShutdown(socket_.value, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            reader_.join();
        }
    }

    std::string Connect(const std::string& service, bool host, const std::string& code) {
        session_.value = WinHttpOpen(L"HalfSword direct ICE", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_.value) return "Could not start Windows networking";
        WinHttpSetTimeouts(session_.value, 5000, 5000, 5000, 10000);
        const std::wstring hostname(service.begin(), service.end());
        connection_.value = WinHttpConnect(session_.value, hostname.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!connection_.value) return "Could not reach the room service";
        HttpHandle request;
        const std::wstring path = L"/room/" + std::wstring(code.begin(), code.end());
        request.value = WinHttpOpenRequest(connection_.value, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!request.value) return "Could not open room request";
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect) ||
            !WinHttpSetOption(request.value, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0))
            return "Windows could not prepare the secure room connection";
        const std::wstring_view headers = host ? L"X-HSM-Role: host\r\n" : L"X-HSM-Role: join\r\n";
        const bool sent = WinHttpSendRequest(request.value, headers.data(), DWORD(headers.size()), nullptr, 0, 0, 0);
        if (!sent || !WinHttpReceiveResponse(request.value, nullptr)) return "Could not reach the secure room service";
        DWORD status = 0, size = sizeof status;
        if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX))
            return "Room service returned an invalid response";
        if (status == 409 || status == 410) return "Room unavailable, full, or expired";
        if (status == 429) return "Room service limit reached. Try again later";
        if (status != 101) return "Room service unavailable (" + std::to_string(status) + ")";
        socket_.value = WinHttpWebSocketCompleteUpgrade(request.value, 0);
        if (!socket_.value) return "Secure room connection failed";
        reader_ = std::thread([this] { Read(); });
        return {};
    }

    bool Send(const std::string& s) {
        return !WinHttpWebSocketSend(socket_.value, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                    const_cast<char*>(s.data()), DWORD(s.size()));
    }

    std::deque<std::string> Drain() {
        std::lock_guard lock(mutex_);
        std::deque<std::string> result;
        result.swap(inbox_);
        return result;
    }

    bool Closed() const { return closed_; }

private:
    void Read() {
        std::array<char, kMaxSignal> buffer{};
        size_t used = 0;
        try {
            while (used < buffer.size()) {
                DWORD count = 0;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
                if (WinHttpWebSocketReceive(socket_.value, buffer.data() + used, DWORD(buffer.size() - used), &count, &type)) break;
                used += count;
                if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) continue;
                if (type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) break;
                std::lock_guard lock(mutex_);
                if (inbox_.size() == 64) break;
                inbox_.emplace_back(buffer.data(), used);
                used = 0;
            }
        } catch (...) {}
        closed_ = true;
    }

    HttpHandle session_, connection_, socket_;
    std::thread reader_;
    std::mutex mutex_;
    std::deque<std::string> inbox_;
    std::atomic<bool> closed_{false};
};

class Peer {
public:
    explicit Peer(const std::string& context) : channel(context) {}
    ~Peer() {
        if (agent) juice_destroy(agent);
        if (socket != INVALID_SOCKET) closesocket(socket);
    }
    bool Start() {
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in loopback{};
        loopback.sin_family = AF_INET;
        loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        u_long nonblocking = 1;
        if (socket == INVALID_SOCKET || bind(socket, reinterpret_cast<sockaddr*>(&loopback), sizeof loopback)
            || ioctlsocket(socket, FIONBIO, &nonblocking)) return false;
        int length = sizeof loopback;
        if (getsockname(socket, reinterpret_cast<sockaddr*>(&loopback), &length)) return false;
        port = ntohs(loopback.sin_port);

        juice_config_t config{};
        config.stun_server_host = "stun.cloudflare.com";
        config.stun_server_port = 3478;
        config.user_ptr = this;
        config.cb_gathering_done = [](juice_agent_t*, void* user) {
            static_cast<Peer*>(user)->gathered = true;
        };
        config.cb_recv = [](juice_agent_t*, const char* data, size_t size, void* user) {
            auto& peer = *static_cast<Peer*>(user);
            if (size > kMaxDatagram + 28) return;
            std::lock_guard lock(peer.mutex);
            if (peer.inbox.size() < 256)
                peer.inbox.emplace_back(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + size);
        };
        agent = juice_create(&config);
        return agent && !channel.PublicKey().empty() && !juice_gather_candidates(agent);
    }
    bool Connected() const {
        const auto state = juice_get_state(agent);
        return state == JUICE_STATE_CONNECTED || state == JUICE_STATE_COMPLETED;
    }
    void Pump(uint16_t gamePort) {
        std::deque<std::vector<uint8_t>> packets;
        {
            std::lock_guard lock(mutex);
            packets.swap(inbox);
        }
        if (!gamePort) return;
        sockaddr_in game{};
        game.sin_family = AF_INET;
        game.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        game.sin_port = htons(gamePort);
        for (const auto& packet : packets) {
            auto plain = channel.Open(packet.data(), packet.size());
            if (!plain.empty()) sendto(socket, reinterpret_cast<const char*>(plain.data()), int(plain.size()), 0,
                                       reinterpret_cast<const sockaddr*>(&game), sizeof game);
        }
        if (!Connected()) return;
        for (int i = 0; i < 128; ++i) {
            std::array<uint8_t, kMaxDatagram + 1> buf{};
            sockaddr_in from{};
            int length = sizeof from;
            const int count = recvfrom(socket, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                                       reinterpret_cast<sockaddr*>(&from), &length);
            if (count < 0) break;
            if (from.sin_addr.s_addr != game.sin_addr.s_addr || from.sin_port != game.sin_port) continue;
            auto encrypted = channel.Seal(buf.data(), size_t(count));
            if (!encrypted.empty()) juice_send(agent, reinterpret_cast<const char*>(encrypted.data()), encrypted.size());
        }
    }

    Channel channel;
    juice_agent_t* agent = nullptr;
    uint16_t port = 0;
    std::atomic<bool> gathered{false};
    bool announced = false;
    Clock::time_point deadline = Clock::now() + 30s;
private:
    SOCKET socket = INVALID_SOCKET;
    std::mutex mutex;
    std::deque<std::vector<uint8_t>> inbox;
};

std::string Run(Job& job, const std::string& service, bool host, const std::string& code) {
    Wire wire;
    const auto error = wire.Connect(service, host, code);
    if (!error.empty()) return error;
    std::map<std::string, std::unique_ptr<Peer>> peers;
    const auto deadline = Clock::now() + 30s;
    bool welcomed = false, clientReady = false;
    auto add = [&](const std::string& id, const std::string& clientId) {
        if (peers.size() >= 7 || peers.contains(id)) return;
        auto peer = std::make_unique<Peer>("hsm-direct-rooms-v1:" + code + ":" + clientId);
        if (peer->Start()) peers.emplace(id, std::move(peer));
    };
    while (!job.stop && !wire.Closed()) {
        for (const auto& raw : wire.Drain()) {
            Signal msg;
            if (!ReadSignal(raw, msg)) continue;
            if (!welcomed && msg.kind == "welcome") {
                if (host && msg.id == "host") {
                    welcomed = true;
                    job.Update(Phase::Ready);
                } else if (!host && PeerId(msg.id)) {
                    welcomed = true;
                    add("host", msg.id);
                    if (peers.empty()) return "Could not create the local ICE connection";
                }
                continue;
            }
            if (!welcomed) continue;
            if (host && msg.kind == "joined" && PeerId(msg.id)) add(msg.id, msg.id);
            if (host && msg.kind == "left") peers.erase(msg.id);
            if (msg.kind != "signal") continue;
            const auto peer = peers.find(msg.from);
            if (peer != peers.end() && (!peer->second->channel.Accept(msg.key) ||
                juice_set_remote_description(peer->second->agent, msg.sdp.c_str()))) {
                peers.erase(peer);
                if (!host) return "Peer handshake failed. Use the same in-game ICE build";
            }
        }
        for (auto it = peers.begin(); it != peers.end();) {
            auto& peer = *it->second;
            if (peer.gathered && !peer.announced) {
                char description[JUICE_MAX_SDP_STRING_LEN]{};
                if (juice_get_local_description(peer.agent, description, sizeof description) ||
                    !wire.Send("{\"to\":" + JsonString(it->first) +
                               ",\"payload\":{\"kind\":\"description\",\"sdp\":" + JsonString(description) +
                               ",\"key\":" + JsonString(peer.channel.PublicKey()) + "}}"))
                    return "Could not exchange connection details";
                peer.announced = true;
            }
            if (peer.Connected()) {
                if (!host && !clientReady) {
                    job.Update(Phase::Ready, {}, peer.port);
                    clientReady = true;
                }
                peer.Pump(job.gamePort);
            } else if (juice_get_state(peer.agent) == JUICE_STATE_FAILED || Clock::now() >= peer.deadline) {
                if (!host) return "Direct ICE connection failed. Check UDP connectivity and firewall";
                it = peers.erase(it);
                continue;
            }
            ++it;
        }
        if ((!welcomed || (!host && !clientReady)) && Clock::now() >= deadline) return "Room connection timed out";
        std::this_thread::sleep_for(2ms);
    }
    return "Direct ICE room closed";
}
} // namespace

std::string Start(const std::string& service, bool host, const std::string& code, uint16_t gamePort) {
    auto& current = ActiveJob();
    if (current && !current->finished) return "The previous direct ICE room is still closing";
    if (service.empty() || service.size() > 253 ||
        !InAlphabet(service, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") ||
        service.front() == '.' || service.back() == '.' || service.find("..") != std::string::npos)
        return "Invalid direct ICE signalling hostname";
    std::string room = code;
    if (host) {
        std::array<uint8_t, 6> random{};
        if (BCryptGenRandom(nullptr, random.data(), ULONG(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))
            return "Could not generate a room code";
        room.clear();
        constexpr char hex[] = "0123456789abcdef";
        for (uint8_t b : random) { room += hex[b >> 4]; room += hex[b & 15]; }
    } else if (room.size() != 12 || !InAlphabet(room, "0123456789abcdef")) return "Enter the 12-character room code";

    auto job = std::make_shared<Job>();
    job->state = {Phase::Connecting, 0, room, {}};
    job->gamePort = gamePort;
    try {
        std::thread([job, service, host, room] {
            try {
                const auto error = Run(*job, service, host, room);
                if (!job->stop) job->Update(Phase::Failed, error);
            } catch (...) {
                if (!job->stop) job->Update(Phase::Failed, "Private connection stopped unexpectedly");
            }
            job->finished = true;
        }).detach();
    } catch (...) { return "Could not start the direct ICE connection"; }
    current = std::move(job);
    return {};
}

State Status() {
    const auto& job = ActiveJob();
    if (!job || job->stop) return {};
    std::lock_guard lock(job->mutex);
    return job->state;
}
void SetGamePort(uint16_t port) { if (const auto& job = ActiveJob()) job->gamePort = port; }
void Stop() { if (const auto& job = ActiveJob()) { job->gamePort = 0; job->stop = true; } }

}
