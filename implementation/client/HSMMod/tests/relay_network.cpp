#include "../src/net/ice_room.cpp"
#include <cstdio>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL relay line %d: %s\n", __LINE__, #x); return 1; } } while (0)

namespace {
struct GameSocket {
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in address{};
    ~GameSocket() { if (socket != INVALID_SOCKET) closesocket(socket); }
    bool Bind() {
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == INVALID_SOCKET) return false;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int length = sizeof address;
        u_long nonblocking = 1;
        return bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0 &&
               getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == 0 &&
               ioctlsocket(socket, FIONBIO, &nonblocking) == 0;
    }
    bool Send(uint16_t port, const char* data, int size) {
        auto peer = address;
        peer.sin_port = htons(port);
        return sendto(socket, data, size, 0, reinterpret_cast<sockaddr*>(&peer), sizeof peer) == size;
    }
    bool Receive(const std::string& want) {
        char buffer[1200];
        const int bytes = recv(socket, buffer, sizeof buffer, 0);
        return bytes == int(want.size()) && std::string_view(buffer, size_t(bytes)) == want;
    }
};

template <class Predicate> bool WaitFor(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        Sleep(2);
    }
    return true;
}
}

int main() {
    using namespace hsm::net::ice;
    WSADATA wsa{};
    CHECK(WSAStartup(MAKEWORD(2,2), &wsa) == 0);
    juice_set_log_level(JUICE_LOG_LEVEL_ERROR);
    juice_server_credentials_t credentials[]{{"relay-test", "relay-test-password", 2}};
    juice_server_config_t serverConfig{};
    serverConfig.credentials = credentials;
    serverConfig.credentials_count = 1;
    serverConfig.max_allocations = 2;
    serverConfig.max_peers = 2;
    serverConfig.bind_address = serverConfig.external_address = "127.0.0.1";
    std::unique_ptr<juice_server_t, decltype(&juice_server_destroy)> server(juice_server_create(&serverConfig), &juice_server_destroy);
    CHECK(server);
    const Turn turn{"127.0.0.1", credentials[0].username, credentials[0].password, juice_server_get_port(server.get())};
    CHECK(turn.port);
    Signal message;
    CHECK(!ReadSignal(R"({"kind":"welcome","id":"host"})",message));
    CHECK(ReadSignal(R"({"kind":"welcome","id":"host","mode":"relay-v1","relay_ip":"127.0.0.1","turn_port":"3478","turn_user":"1234:relay-test","turn_password":"AAAAAAAAAAAAAAAAAAAAAAAAAAA="})",message));
    CHECK(message.turn.ip == turn.ip && message.turn.port == 3478);

    juice_config_t noRelay{};
    noRelay.relay_only = 1;
    std::unique_ptr<juice_agent_t, decltype(&juice_destroy)> unavailable(juice_create(&noRelay), &juice_destroy);
    CHECK(unavailable && juice_gather_candidates(unavailable.get()) == 0);
    char unavailableSdp[JUICE_MAX_SDP_STRING_LEN]{};
    CHECK(juice_get_local_description(unavailable.get(),unavailableSdp,sizeof unavailableSdp) == 0);
    CHECK(!RelayDescription(unavailableSdp,turn.ip));
    CHECK(std::string_view(unavailableSdp).find("a=candidate:") == std::string_view::npos);

    Peer left("test-room"), right("test-room");
    CHECK(left.Start(turn) && right.Start(turn));
    CHECK(WaitFor([&] { return left.gathered && right.gathered; }));
    char leftSdp[JUICE_MAX_SDP_STRING_LEN]{}, rightSdp[JUICE_MAX_SDP_STRING_LEN]{};
    CHECK(juice_get_local_description(left.agent,leftSdp,sizeof leftSdp) == 0);
    CHECK(juice_get_local_description(right.agent,rightSdp,sizeof rightSdp) == 0);
    CHECK(RelayDescription(leftSdp,turn.ip) && RelayDescription(rightSdp,turn.ip));
    CHECK(!RelayDescription(leftSdp,"127.0.0.2"));
    const auto mixed = std::string(leftSdp) + "a=candidate:1 1 UDP 99 127.0.0.1 9999 typ host\r\n";
    CHECK(!RelayDescription(mixed,turn.ip));
    CHECK(juice_set_remote_description(right.agent,mixed.c_str()) == JUICE_ERR_INVALID);
    auto leaking = std::string(leftSdp);
    leaking.replace(leaking.find("raddr 0.0.0.0"), 13, "raddr 192.168.1.5");
    CHECK(!RelayDescription(leaking,turn.ip));

    CHECK(left.channel.Accept(right.channel.PublicKey()) && right.channel.Accept(left.channel.PublicKey()));
    CHECK(juice_set_remote_description(left.agent,rightSdp) == 0);
    CHECK(juice_set_remote_description(right.agent,leftSdp) == 0);
    CHECK(WaitFor([&] { return left.Connected() && right.Connected(); }));
    char local[256]{}, remote[256]{};
    CHECK(juice_get_selected_candidates(left.agent,local,sizeof local,remote,sizeof remote) == 0);
    CHECK(std::string_view(local).find(" typ relay ") != std::string_view::npos);
    CHECK(std::string_view(remote).find(" typ relay ") != std::string_view::npos);

    GameSocket a, b;
    CHECK(a.Bind() && b.Bind());
    const std::string fromA(1200,'A'), fromB(1200,'B');
    CHECK(a.Send(left.port,fromA.data(),int(fromA.size())) && b.Send(right.port,fromB.data(),int(fromB.size())));
    bool receivedA = false, receivedB = false;
    CHECK(WaitFor([&] {
        left.Pump(ntohs(a.address.sin_port));
        right.Pump(ntohs(b.address.sin_port));
        receivedA = receivedA || a.Receive(fromB);
        receivedB = receivedB || b.Receive(fromA);
        return receivedA && receivedB;
    }));
    std::puts("PASS only relay candidates selected, direct candidates rejected, encrypted game datagrams forwarded both ways");
}
