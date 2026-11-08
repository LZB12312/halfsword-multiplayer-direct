// Real UDP integration harness; no game, Steam process, or Steam DLL is loaded.
#include "net/direct_api.h"
#include "hsm_protocol.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <algorithm>
#include <set>
namespace hsm { void Log(const char* fmt,...) { va_list ap; va_start(ap,fmt); vprintf(fmt,ap); puts(""); va_end(ap); } }
using namespace hsm;
namespace d=hsm::direct;
#define CHECK(x) do { if(!(x)) { printf("FAIL %d: %s\n",__LINE__,#x); return 1; } } while(0)
int main(int argc,char** argv) {
    CHECK(argc==2 || argc==3); CHECK(d::Init()); CHECK(!d::VoiceAvailable());
    CHECK(GetModuleHandleA("steam_api64.dll")==nullptr);
    const std::string mode=argv[1];
    const bool ice=mode=="ice-host" || mode=="ice-client";
    const bool host=mode=="host" || mode=="ice-host";
    const char* token = ice ? std::getenv("HSM_INVITE_TOKEN") : nullptr;
    if(mode=="lifecycle-host" || mode=="lifecycle-client") {
        const bool server=mode=="lifecycle-host"; int entered=0, chats=0; bool joining=!server;
        if(server) d::CreateLobby(d::LobbyType::Private,2); else d::JoinLobby(1);
        auto start=GetTickCount();
        for(;;) {
            CHECK(GetTickCount()-start<15000); d::Tick(); d::Event e;
            while(d::Poll(e)) {
                if(e.kind==d::Event::LobbyCreated) CHECK(e.ok);
                if(e.kind==d::Event::LobbyEntered) { CHECK(e.ok); joining=false; ++entered; CHECK(d::SendLobbyChat(1,"lifecycle")); }
                if(server && e.kind==d::Event::LobbyChat) {
                    CHECK(d::RemoteAddress(e.user)=="127.0.0.1");
                    if(++chats==1) d::DisconnectUser(e.user);
                    else { d::LeaveLobby(1); puts("PASS host kick, new connection and graceful shutdown"); return 0; }
                }
            }
            if(!server && entered && !joining && !d::LobbyOwner(1)) {
                if(entered==1) { joining=true; d::JoinLobby(1); }
                else { d::LeaveLobby(1); puts("PASS kicked client reconnects and observes host disconnect"); return 0; }
            }
            Sleep(2);
        }
    }
    if(mode=="timeout" || mode=="bindfail") {
        if(mode=="bindfail") d::CreateLobby(d::LobbyType::Private,2); else d::JoinLobby(1);
        auto start=GetTickCount();
        for(;;) {
            CHECK(GetTickCount()-start<13000); d::Tick(); d::Event e;
            while(d::Poll(e)) if(e.kind==d::Event::LobbyEntered || e.kind==d::Event::LobbyCreated) {
                CHECK(!e.ok); d::LeaveLobby(1); puts("PASS connection/bind failure reported"); return 0;
            }
            Sleep(2);
        }
    }
    if(host) {
        if (ice) d::CreateIceLobby(3,token ? token : "");
        else d::CreateLobby(d::LobbyType::Private,3);
    } else if (ice) {
        CHECK(argc==3);
        d::JoinIceLobby(token ? token : "",argv[2]);
    } else d::JoinLobby(1);
    bool joined=false, chat=false, large=false, unreliable=false, ready=false;
    std::set<d::UserId> seen, complete;
    uint32_t start=GetTickCount(), lastSend=0, finished=0;
    for(;;) {
        const auto now=GetTickCount(); CHECK(now-start<(ice?65000u:15000u));
        d::Tick(); d::Event e;
        while(d::Poll(e)) {
            if(e.kind==d::Event::LobbyCreated) {
                if (!e.ok) puts(e.text.c_str());
                CHECK(e.ok);
                CHECK(d::LobbyOwner(1)==1);
                joined=true;
                d::SetLobbyData(1,"test","snapshot-value");
                CHECK(d::SetLobbyData(1,"hsm_sever","250"));
                if (ice) {
                    printf("ROOM %s\n",d::IceRoomCode().c_str());
                    fflush(stdout);
                }
            }
            if(e.kind==d::Event::LobbyEntered) {
                if (ice && !e.ok) puts(e.text.c_str());
                if(mode=="reject") { CHECK(!e.ok); d::LeaveLobby(1); puts("PASS full-host rejection"); return 0; }
                CHECK(e.ok); joined=true; CHECK(d::MyUserId()>1); CHECK(d::LobbyData(1,"test")=="snapshot-value");
                CHECK(d::LobbyData(1,"hsm_sever")=="250");
                CHECK(!d::SetLobbyData(1,"hsm_sever","400"));
                CHECK(d::LobbyData(1,"hsm_sever")=="250");
                d::SetMemberData(1,"ready","1");
                d::SetMemberData(1,"hsm_card","Militia\nMaps");
                d::SetMemberData(1,"hsm_mrule","forged");
                CHECK(d::SetDisplayName("Renamed-"+std::to_string(d::MyUserId())));
                CHECK(d::SendLobbyChat(1,"hello from client"));
            }
            if(e.kind==d::Event::LobbyChat) { CHECK(e.user>1); CHECK(e.text=="hello from client"); chat=true; }
        }
        if(!joined) { Sleep(2); continue; }
        if(!host) ready=d::PersonaName(d::MyUserId())=="Renamed-"+std::to_string(d::MyUserId());
        if(host) for(auto id:d::LobbyMembers(1)) if(id!=1 && !seen.count(id)) {
            CHECK(d::RemoteAddress(id).empty()==ice); seen.insert(id);
        }
        // Exercise fragmented reliable frames at the gameplay packet limit and a
        // separate unreliable stream. Echo both directions and check every byte.
        if(now-lastSend>50 && (!host || !seen.empty())) {
            lastSend=now; std::vector<uint8_t> blob(kMaxPacket,0x5a), pose(128,0xa5);
            if(!host) { CHECK(d::Send(1,0,blob.data(),uint32_t(blob.size()),d::kReliable)); CHECK(d::Send(1,1,pose.data(),uint32_t(pose.size()),d::kUnreliable)); }
        }
        std::vector<d::Message> msgs; d::Receive(0,msgs); d::Receive(1,msgs);
        for(auto& m:msgs) {
            CHECK(m.from==(host?m.from:1)); CHECK(m.from!=d::MyUserId());
            CHECK(m.data.size()==(m.channel==0?kMaxPacket:128));
            CHECK(std::all_of(m.data.begin(),m.data.end(),[&](uint8_t c){return c==(m.channel==0?0x5a:0xa5);}));
            if(host) { CHECK(d::Send(m.from,m.channel,m.data.data(),uint32_t(m.data.size()),m.channel==0?d::kReliable:0)); if(m.channel==0) complete.insert(m.from); }
            if(m.channel==0) large=true; else unreliable=true;
        }
        if(!host) ready=ready && d::MemberData(1,d::MyUserId(),"ready")=="1" && d::MemberData(1,d::MyUserId(),"hsm_card")=="Militia\nMaps";
        else ready=std::all_of(seen.begin(),seen.end(),[](auto id){ return d::MemberData(1,id,"ready")=="1" && d::MemberData(1,id,"hsm_card")=="Militia\nMaps" && d::MemberData(1,id,"hsm_mrule").empty() && d::PersonaName(id)=="Renamed-"+std::to_string(id); });
        if(chat && large && unreliable && ready && (!host || complete.size()==2)) {
            if(!finished) finished=now;
        }
        if(finished && now-finished>(host?3000:1500)) { d::LeaveLobby(1); puts("PASS snapshot, membership, chat, ready, reliable 16 KiB and unreliable UDP"); return 0; }
        Sleep(2);
    }
}
