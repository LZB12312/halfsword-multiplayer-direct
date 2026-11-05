// Exercise ownership/failure paths without discovering or changing a real router.
#include "net/upnp.h"
#include <miniupnpc.h>
#include <upnpcommands.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <cstdarg>
namespace hsm { void Log(const char*,...) {} }
namespace {
std::atomic<int> adds{0},deletes{0},queries{0},frees{0};
int scenario=0;
bool present=false;
UPNPDev device{};
const char* description="Half Sword Direct Multiplayer";
}
extern "C" {
UPNPDev* upnpDiscover(int,const char*,const char*,int,int,unsigned char,int*) { return scenario==1?nullptr:&device; }
void freeUPNPDevlist(UPNPDev*) {}
void FreeUPNPUrls(UPNPUrls*) { ++frees; }
int UPNP_GetValidIGD(UPNPDev*,UPNPUrls* u,IGDdatas* d,char* lan,int,char* wan,int) {
    static char url[]="http://test/"; u->controlURL=url; std::strcpy(d->first.servicetype,"test");
    std::strcpy(lan,"192.168.1.2"); std::strcpy(wan,"203.0.113.1"); return UPNP_CONNECTED_IGD;
}
int UPNP_GetSpecificPortMappingEntry(const char*,const char*,const char*,const char*,const char*,char* client,char* port,char* desc,char*,char*) {
    ++queries;
    if(scenario==2 || (present && scenario==6)) { std::strcpy(client,"192.168.1.3"); std::strcpy(port,"27015"); std::strcpy(desc,"Other application"); return 0; }
    if(scenario==3) return 501;
    if(!present) return 714;
    std::strcpy(client,"192.168.1.2"); std::strcpy(port,"27015"); std::strcpy(desc,description); return 0;
}
int UPNP_AddPortMapping(const char*,const char*,const char*,const char*,const char*,const char*,const char*,const char*,const char* lease) {
    ++adds;
    if(scenario==4) return 718;
    if(scenario==5 && std::strcmp(lease,"0")) return 725;
    present=true; return 0;
}
int UPNP_DeletePortMapping(const char*,const char*,const char*,const char*,const char*) { ++deletes; present=false; return 0; }
}
#define CHECK(x) do { if(!(x)) { printf("FAIL scenario %d line %d: %s\n",scenario,__LINE__,#x); return 1; } } while(0)
int main() {
    for(scenario=0;scenario<=6;++scenario) {
        adds=0; deletes=0; queries=0; frees=0; present=false;
        hsm::net::UpnpMapping m; m.Start(27015);
        // Give the worker a deterministic chance to finish its discovery/add path.
        auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(!frees && !adds && std::chrono::steady_clock::now()<until) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::this_thread::sleep_for(std::chrono::milliseconds(30)); m.Stop();
        CHECK(frees==1);
        if(scenario==0 || scenario==5) CHECK(deletes==1);
        else CHECK(deletes==0);
        if(scenario==1 || scenario==2 || scenario==3) CHECK(adds==0);
        if(scenario==4 || scenario==6) CHECK(adds==1);
        if(scenario==5) CHECK(adds==2);
    }
    puts("PASS UPnP create/remove, unavailable gateway, existing mapping, inspection failure, add failure, permanent lease fallback, changed-rule preservation");
}
