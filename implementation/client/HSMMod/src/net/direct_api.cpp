// Steam-free lobby service over ENet. The host owns membership and authenticates
// packet senders by their ENet connection. All gameplay goes through the host.
#include "net/direct_api.h"
#include "net/upnp.h"
#include "hsm_protocol.h"
#include "log.h"
#include "paths.h"
#include <enet/enet.h>
#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <cstring>

namespace hsm::direct {
namespace {
constexpr uint32_t kMagic = 0x48534431; // HSD1, separate from upstream Steam protocol
constexpr size_t kFrameMax = 65536, kQueueMax = 4096;
enum Op : uint8_t { Hello = 1, Snapshot, MemberUpdate, Chat, Game, Rejected };
using Values = std::map<std::string, std::string>;
struct Member { std::string name; Values data; };
struct Writer {
    std::vector<uint8_t> b;
    explicit Writer(Op op) { U32(kMagic); U8(op); }
    void U8(uint8_t v) { b.push_back(v); }
    void U32(uint32_t v) { for (int i=0;i<4;++i) U8(uint8_t(v >> (i*8))); }
    void U64(uint64_t v) { for (int i=0;i<8;++i) U8(uint8_t(v >> (i*8))); }
    void Str(const std::string& s) { U32(uint32_t(s.size())); b.insert(b.end(), s.begin(), s.end()); }
    void Map(const Values& v) { U32(uint32_t(v.size())); for (auto& [k,s] : v) { Str(k); Str(s); } }
};
struct Reader {
    const uint8_t* p; size_t n; bool ok = true;
    uint8_t U8() { if (!n) { ok=false; return 0; } --n; return *p++; }
    uint32_t U32() { uint32_t v=0; for(int i=0;i<4;++i) v |= uint32_t(U8()) << (i*8); return v; }
    uint64_t U64() { uint64_t v=0; for(int i=0;i<8;++i) v |= uint64_t(U8()) << (i*8); return v; }
    std::string Str(size_t max=4096) {
        auto len=U32(); if (!ok || len>max || len>n) { ok=false; return {}; }
        std::string s(reinterpret_cast<const char*>(p),len); p+=len; n-=len; return s;
    }
    Values Map() {
        Values v; auto count=U32(); if(count>128) { ok=false; return {}; }
        for(uint32_t i=0;i<count && ok;++i) { auto k=Str(64); auto s=Str(); v[k]=s; }
        return v;
    }
    bool Done() const { return ok && !n; }
};
struct Backend {
    bool ready=false, hosting=false, entered=false, joinable=true, dirty=false;
    ENetHost* host=nullptr; ENetPeer* server=nullptr;
    UserId me=1, next=2;
    int limit=8; uint16_t port=27015; bool upnp=true;
    uint32_t joinAt=0;
    std::string address="127.0.0.1:27015", name="Player";
    Values data; std::map<UserId,Member> members;
    std::map<ENetPeer*,UserId> peers;
    std::map<ENetPeer*,uint32_t> pending;
    std::deque<Event> events; std::deque<Message> messages;
    net::UpnpMapping mapping;
    ~Backend() { Clear(); if(ready) enet_deinitialize(); }
    void Clear() {
        mapping.Stop();
        if(host) { for(size_t i=0;i<host->peerCount;++i) enet_peer_disconnect_now(&host->peers[i],0); enet_host_destroy(host); }
        host=nullptr; server=nullptr; hosting=entered=dirty=false; joinAt=0;
        peers.clear(); pending.clear(); members.clear(); data.clear(); messages.clear(); events.clear();
    }
    void EventOf(Event::Kind kind, bool ok=true, const std::string& text={}) {
        if(events.size()>=kQueueMax) return;
        Event e{}; e.kind=kind; e.lobby=1; e.ok=ok; e.text=text; events.push_back(std::move(e));
    }
    bool Packet(ENetPeer* peer, const Writer& w, uint8_t channel=0, bool reliable=true) {
        if(!peer || w.b.size()>kFrameMax || peer->outgoingDataTotal > 4*1024*1024) return false;
        auto* p=enet_packet_create(w.b.data(), w.b.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
        if(!p) return false;
        if(enet_peer_send(peer,channel,p)<0) { enet_packet_destroy(p); return false; }
        return true;
    }
    void All(const Writer& w) { for(auto& [peer,id] : peers) { (void)id; Packet(peer,w); } }
    void StateTo(ENetPeer* peer, UserId recipient) {
        Writer w(Snapshot); w.U64(recipient); w.U32(uint32_t(limit)); w.Map(data); w.U32(uint32_t(members.size()));
        for(auto& [id,m] : members) { w.U64(id); w.Str(m.name); w.Map(m.data); }
        Packet(peer,w);
    }
    void Changed() { dirty=true; EventOf(Event::LobbyDataChanged); }
    UserId Sender(ENetPeer* p) const { auto it=peers.find(p); return it==peers.end()?0:it->second; }
    ENetPeer* Peer(UserId id) const { for(auto& [p,u]:peers) if(u==id) return p; return nullptr; }
    void Incoming(ENetPeer* peer, uint8_t channel, ENetPacket* packet) {
        if(packet->dataLength>kFrameMax) return;
        Reader r{packet->data,packet->dataLength}; if(r.U32()!=kMagic) return;
        auto op=Op(r.U8()); UserId sender=hosting?Sender(peer):1;
        if(hosting && !sender && op!=Hello) return;
        if(op==Hello && hosting && !sender && channel==0 && pending.count(peer)) {
            auto who=r.Str(32); if(!r.Done()) return;
            if(!joinable || members.size()>=size_t(limit)) {
                Writer w(Rejected); w.Str("Host is full or closed"); Packet(peer,w); enet_peer_disconnect_later(peer,0); pending.erase(peer); return;
            }
            pending.erase(peer); sender=next++; peers[peer]=sender;
            members[sender]={who.empty()?"Player":who,{}};
            Changed(); EventOf(Event::LobbyMembersChanged); return;
        }
        if(op==Snapshot && !hosting && peer==server && channel==0) {
            const auto assigned=r.U64(); const auto capacity=r.U32(); auto values=r.Map(); auto count=r.U32();
            if(count==0 || count>9 || assigned<2 || capacity<2 || capacity>9) return;
            std::map<UserId,Member> roster;
            for(uint32_t i=0;i<count && r.ok;++i) { auto id=r.U64(); Member m; m.name=r.Str(32); m.data=r.Map(); if(!id || id>>63 || roster.count(id)) return; roster[id]=std::move(m); }
            if(!r.Done() || !roster.count(1) || !roster.count(assigned)) return;
            me=assigned; limit=int(capacity); data=std::move(values); members=std::move(roster);
            if(!entered) { entered=true; joinAt=0; EventOf(Event::LobbyEntered); }
            else EventOf(Event::LobbyMembersChanged);
            return;
        }
        if(op==Rejected && !hosting && !entered && channel==0) {
            auto why=r.Str(); if(r.Done()) { joinAt=0; EventOf(Event::LobbyEntered,false,why); } return;
        }
        if(op==MemberUpdate && hosting && sender && channel==0) {
            auto key=r.Str(64), value=r.Str();
            if(!r.Done() || (key!="ready" && key!="hsm_team" && key!="hsm_ver" && key!="hsm_mod")) return;
            if(members[sender].data[key]!=value) { members[sender].data[key]=value; Changed(); }
            return;
        }
        if(op==Chat && channel==0 && entered) {
            UserId from=hosting?sender:r.U64(); auto text=r.Str(2048);
            if(!r.Done() || !members.count(from)) return;
            if(hosting) { Writer w(Chat); w.U64(from); w.Str(text); All(w); }
            if(events.size()<kQueueMax) { Event e{}; e.kind=Event::LobbyChat; e.lobby=1; e.user=from; e.text=std::move(text); events.push_back(std::move(e)); }
            return;
        }
        if(op==Game && entered && channel>0 && channel<=kChanCount) {
            UserId from=hosting?sender:r.U64(); auto to=r.U64();
            if(!r.ok || !members.count(from) || to!=me || !r.n || r.n>kMaxPacket) return;
            if(messages.size()<kQueueMax) { Message m; m.from=from; m.channel=channel-1; m.data.assign(r.p,r.p+r.n); messages.push_back(std::move(m)); }
        }
    }
} b;
std::string Read(const wchar_t* key, const wchar_t* fallback) {
    wchar_t value[256]; GetPrivateProfileStringW(L"net",key,fallback,value,256,ModFilePath(L"HSMMod.ini").c_str());
    char out[1024]{}; WideCharToMultiByte(CP_UTF8,0,value,-1,out,sizeof out,nullptr,nullptr); return out;
}
bool Endpoint(const std::string& s, ENetAddress& a) {
    auto colon=s.find(':'); auto name=s.substr(0,colon); std::string ps=colon==std::string::npos?std::to_string(b.port):s.substr(colon+1);
    if(name.empty() || ps.empty() || ps.find_first_not_of("0123456789")!=std::string::npos || ps.size()>5) return false;
    auto port=std::strtoul(ps.c_str(),nullptr,10); if(!port || port>65535) return false;
    a.port=uint16_t(port); return enet_address_set_host(&a,name.c_str())==0;
}
void Limits() { b.host->maximumPacketSize=kFrameMax; b.host->maximumWaitingData=1024*1024; }
} // namespace

bool Init() {
    if(b.ready) return true;
    if(enet_initialize()!=0) return false;
    b.ready=true; b.name=Read(L"name",L"Player").substr(0,32); b.address=Read(L"connect",L"127.0.0.1:27015");
    auto ps=Read(L"port",L"27015"); auto port=std::strtoul(ps.c_str(),nullptr,10);
    b.port=uint16_t(port && port<=65535 ? port : 27015); b.upnp=Read(L"upnp",L"1")!="0";
    HSM_LOG("direct: ready; host UDP port %u, connect=%s, UPnP=%s",b.port,b.address.c_str(),b.upnp?"on":"off"); return true;
}
bool Ready() { return b.ready; }
std::string ConnectAddress() { return b.address; }
bool SetConnectAddress(const std::string& address) {
    if(address.empty() || address.size()>253 || address.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_:")!=std::string::npos) return false;
    auto colon=address.find(':');
    if(colon==0 || (colon!=std::string::npos && address.find(':',colon+1)!=std::string::npos)) return false;
    if(colon!=std::string::npos) {
        auto ps=address.substr(colon+1);
        if(ps.empty() || ps.size()>5 || ps.find_first_not_of("0123456789")!=std::string::npos) return false;
        auto port=std::strtoul(ps.c_str(),nullptr,10); if(!port || port>65535) return false;
    }
    b.address=address; return true;
}
std::string ConnectionInfo() { return "Hosting UDP " + std::to_string(b.port) + "; friends connect to your public IPv4:" + std::to_string(b.port) + ". UPnP result is in HSMMod.log."; }
std::string RemoteAddress(UserId user) {
    auto* p=b.Peer(user); char addr[64]{};
    return p && enet_address_get_host_ip(&p->address,addr,sizeof addr)==0 ? std::string(addr) : std::string();
}
void DisconnectUser(UserId user) {
    if(!b.hosting) return;
    auto* peer=b.Peer(user); if(!peer) return;
    enet_peer_disconnect_now(peer,0); b.peers.erase(peer); b.members.erase(user);
    b.Changed(); b.EventOf(Event::LobbyMembersChanged);
}
void Tick() {
    if(!b.host) return;
    ENetEvent ev{};
    // Bound work per game tick, including hostile datagrams.
    for(int i=0;i<1024 && enet_host_service(b.host,&ev,0)>0;++i) {
        if(ev.type==ENET_EVENT_TYPE_CONNECT) {
            enet_peer_timeout(ev.peer,16,3000,10000);
            if(b.hosting) b.pending[ev.peer]=enet_time_get();
            else { Writer w(Hello); w.Str(b.name); b.Packet(ev.peer,w); }
        } else if(ev.type==ENET_EVENT_TYPE_RECEIVE) {
            b.Incoming(ev.peer,ev.channelID,ev.packet); enet_packet_destroy(ev.packet);
        } else if(ev.type==ENET_EVENT_TYPE_DISCONNECT) {
            if(b.hosting) { auto id=b.Sender(ev.peer); b.peers.erase(ev.peer); b.pending.erase(ev.peer); if(id) { b.members.erase(id); b.Changed(); b.EventOf(Event::LobbyMembersChanged); } }
            else { bool joining=!b.entered && b.joinAt; b.entered=false; b.server=nullptr; b.members.clear(); b.EventOf(joining?Event::LobbyEntered:Event::LobbyMembersChanged,false,"Host disconnected or refused connection"); b.joinAt=0; }
        }
    }
    for(auto it=b.pending.begin();it!=b.pending.end();) {
        if(enet_time_get()-it->second>5000) { enet_peer_disconnect_now(it->first,0); it=b.pending.erase(it); } else ++it;
    }
    if(b.joinAt && enet_time_get()-b.joinAt>10000) { enet_peer_reset(b.server); b.server=nullptr; b.joinAt=0; b.EventOf(Event::LobbyEntered,false,"Connection timed out: check connect= address, UDP port forwarding and firewall"); }
    if(b.dirty) { b.dirty=false; for(auto& [p,id]:b.peers) b.StateTo(p,id); }
    enet_host_flush(b.host);
}
bool Poll(Event& out) { if(b.events.empty()) return false; out=std::move(b.events.front()); b.events.pop_front(); return true; }
UserId MyUserId() { return b.me; }
std::string PersonaName(UserId id) { auto it=b.members.find(id); return it==b.members.end()?(id==b.me?b.name:std::string()):it->second.name; }
uint32_t AppId() { return 0; }
std::string LaunchConnectLobby() { return {}; }
void CreateLobby(LobbyType, int max) {
    b.Clear(); b.me=1; b.next=2; b.limit=std::clamp(max,2,9); b.joinable=true;
    ENetAddress address{ENET_HOST_ANY,b.port}; b.host=enet_host_create(&address,16,kChanCount+1,0,0);
    if(!b.host) { HSM_LOG("direct: cannot bind UDP port %u",b.port); b.EventOf(Event::LobbyCreated,false); return; }
    Limits(); b.hosting=b.entered=true; b.members[1]={b.name,{}};
    b.EventOf(Event::LobbyCreated); if(b.upnp) b.mapping.Start(b.port);
    HSM_LOG("direct: hosting UDP port %u (forward this UDP port manually if UPnP is unavailable)",b.port);
}
void JoinLobby(UserId) {
    b.Clear(); b.me=0;
    ENetAddress address{}; if(!Endpoint(b.address,address)) { b.EventOf(Event::LobbyEntered,false,"Invalid connect= address (use IPv4 or hostname:port)"); return; }
    b.host=enet_host_create(nullptr,1,kChanCount+1,0,0);
    if(!b.host) { b.EventOf(Event::LobbyEntered,false,"Could not create UDP socket"); return; }
    Limits(); b.server=enet_host_connect(b.host,&address,kChanCount+1,kMagic);
    if(!b.server) { b.EventOf(Event::LobbyEntered,false); return; }
    b.joinAt=enet_time_get(); if(!b.joinAt) b.joinAt=1;
}
void LeaveLobby(UserId) { b.Clear(); }
bool SetLobbyType(UserId,LobbyType) { return b.hosting; }
bool SetLobbyJoinable(UserId,bool on) { if(!b.hosting) return false; b.joinable=on; return true; }
bool SetLobbyMemberLimit(UserId,int n) { if(!b.hosting) return false; b.limit=std::clamp(n,int(b.members.size()),9); b.Changed(); return true; }
int LobbyMemberLimit(UserId) { return b.limit; }
bool SetLobbyData(UserId,const std::string& k,const std::string& v) {
    if(!b.hosting || k.size()>64 || v.size()>4096 || (!b.data.count(k) && b.data.size()>=128)) return false;
    if(b.data[k]!=v) { b.data[k]=v; b.Changed(); } return true;
}
std::string LobbyData(UserId,const std::string& k) { auto it=b.data.find(k); return it==b.data.end()?std::string():it->second; }
void SetMemberData(UserId,const std::string& k,const std::string& v) {
    if(!b.entered || k.size()>64 || v.size()>4096) return;
    if(b.hosting) { if(b.members[b.me].data[k]!=v) { b.members[b.me].data[k]=v; b.Changed(); } }
    else { Writer w(MemberUpdate); w.Str(k); w.Str(v); b.Packet(b.server,w); }
}
std::string MemberData(UserId,UserId u,const std::string& k) { auto m=b.members.find(u); if(m==b.members.end()) return {}; auto v=m->second.data.find(k); return v==m->second.data.end()?std::string():v->second; }
std::vector<UserId> LobbyMembers(UserId) { std::vector<UserId> ids; for(auto& [id,m]:b.members) { (void)m; ids.push_back(id); } return ids; }
UserId LobbyOwner(UserId) { return b.entered?1:0; }
bool SendLobbyChat(UserId,const std::string& text) {
    if(!b.entered || text.empty() || text.size()>2048) return false;
    Writer w(Chat); if(b.hosting) w.U64(b.me); w.Str(text);
    if(!b.hosting) return b.Packet(b.server,w);
    b.All(w); Event e{}; e.kind=Event::LobbyChat; e.lobby=1; e.user=b.me; e.text=text; b.events.push_back(std::move(e)); return true;
}
void RequestLobbyList(const std::string&,const std::string&) {
    // No master service. Joining this configured endpoint fetches and validates its state.
    Event e{}; e.kind=Event::LobbyList; e.lobbies={1}; b.events.push_back(std::move(e));
}
void InviteOverlay(UserId) { HSM_LOG("direct: share your public IPv4 address and UDP port %u (or LAN IPv4 for local players)",b.port); }
void SetRichPresence(const std::string&,const std::string&) {}
int MediumAvatar(UserId) { return 0; }
bool AvatarRGBA(int,uint32_t&,uint32_t&,std::vector<uint8_t>&) { return false; }
bool VoiceAvailable() { return false; } // gameplay/voice.cpp uses Windows WaveIn + ADPCM instead
void StartVoice() {}
void StopVoice() {}
bool TakeVoice(std::vector<uint8_t>& out) { out.clear(); return false; }
bool DecompressVoice(const uint8_t*,size_t,uint32_t,std::vector<int16_t>&) { return false; }
void InitRelay() {}
bool Send(UserId to,int channel,const void* data,uint32_t size,int flags) {
    if(!b.entered || channel<0 || channel>=kChanCount || !size || size>kMaxPacket || !b.members.count(to)) return false;
    // No peer-to-peer mesh: each client talks only to the host.
    if(!b.hosting && to!=1) return false;
    Writer w(Game); if(b.hosting) w.U64(b.me); w.U64(to);
    const auto* p=static_cast<const uint8_t*>(data); w.b.insert(w.b.end(),p,p+size);
    return b.Packet(b.hosting?b.Peer(to):b.server,w,uint8_t(channel+1),(flags&kReliable)!=0);
}
size_t Receive(int channel,std::vector<Message>& out,int max) {
    size_t count=0;
    for(auto it=b.messages.begin();it!=b.messages.end() && count<size_t(std::max(0,max));) {
        if(it->channel==channel) { out.push_back(std::move(*it)); it=b.messages.erase(it); ++count; } else ++it;
    } return count;
}
void AcceptSession(UserId) {}
void CloseSession(UserId user) {
    // Transport resets must not close the lobby's UDP connection. Membership is
    // managed by LeaveLobby and host-owned state, as with Steam's separate lobby.
    (void)user;
}
int PingMs(UserId user) { auto* p=b.hosting?b.Peer(user):b.server; return p?int(p->roundTripTime):-1; }
} // namespace hsm::direct
