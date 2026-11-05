// Test the control parser in the same translation unit, including malformed and
// forged frames. No router or socket is used by these tests.
#include "../src/net/direct_api.cpp"
#include <cstdio>
#include <random>
namespace hsm { void Log(const char*,...) {} }
namespace hsm::net { void UpnpMapping::Start(uint16_t) {} void UpnpMapping::Stop() {} }
#define CHECK(x) do { if(!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main() {
    using namespace hsm::direct;
    CHECK(!SetConnectAddress("")); CHECK(!SetConnectAddress("127.0.0.1:0"));
    CHECK(!SetConnectAddress("127.0.0.1:65536")); CHECK(!SetConnectAddress("host:1:2"));
    CHECK(!SetConnectAddress("host\nrole=server")); CHECK(SetConnectAddress("test.example:27015"));
    CHECK(SetConnectAddress("192.168.1.2"));
    Writer w(Snapshot); w.Str("value"); w.Map({{"key","test"}});
    for(size_t n=0;n<w.b.size();++n) {
        Reader r{w.b.data(),n}; r.U32(); r.U8(); r.Str(); r.Map(); CHECK(!r.Done());
    }
    Reader r{w.b.data(),w.b.size()}; CHECK(r.U32()==kMagic); CHECK(r.U8()==Snapshot);
    CHECK(r.Str()=="value"); CHECK(r.Map()==Values({{"key","test"}})); CHECK(r.Done());
    ENetPeer peer{};
    b.hosting=b.entered=true; b.me=1; b.members[1]={"Host",{}}; b.members[2]={"Client",{}}; b.peers[&peer]=2;
    auto incoming=[&](Writer& frame,uint8_t channel) {
        ENetPacket p{}; p.data=frame.b.data(); p.dataLength=frame.b.size(); b.Incoming(&peer,channel,&p);
    };
    Writer fakeSnapshot(Snapshot); fakeSnapshot.U64(2); fakeSnapshot.U32(9); fakeSnapshot.Map({{"test","forged"}}); fakeSnapshot.U32(0);
    incoming(fakeSnapshot,0); CHECK(b.data.empty()); CHECK(b.me==1);
    Writer fakeUpdate(MemberUpdate); fakeUpdate.Str("hsm_build"); fakeUpdate.Str("forged");
    incoming(fakeUpdate,0); CHECK(b.members[2].data.empty()); CHECK(b.data.empty());
    Writer update(MemberUpdate); update.Str("ready"); update.Str("1"); incoming(update,0);
    CHECK(b.members[2].data.at("ready")=="1"); CHECK(b.members[1].data.empty());
    Writer game(Game); game.U64(1); game.U8(42); incoming(game,1);
    CHECK(b.messages.size()==1); CHECK(b.messages.front().from==2); CHECK(b.messages.front().data==std::vector<uint8_t>{42}); b.messages.clear();
    Writer wrongTarget(Game); wrongTarget.U64(3); wrongTarget.U8(42); incoming(wrongTarget,1); CHECK(b.messages.empty());
    // Truncations and random input must never create state or unbounded queues.
    std::mt19937 rng(7);
    for(int i=0;i<20000;++i) {
        std::vector<uint8_t> bytes(rng()%512); for(auto& x:bytes) x=uint8_t(rng());
        if(bytes.size()>=4 && i%2==0) for(int j=0;j<4;++j) bytes[j]=uint8_t(kMagic>>(8*j));
        ENetPacket p{}; p.data=bytes.data(); p.dataLength=bytes.size(); b.Incoming(&peer,uint8_t(rng()%4),&p);
    }
    CHECK(b.data.empty()); CHECK(b.members.size()==2); CHECK(b.events.size()<=kQueueMax); CHECK(b.messages.size()<=kQueueMax);
    b.Clear();
    puts("PASS address validation, truncated frames, sender binding, host-state protection, peer-target filtering and 20000 malformed frames");
}
