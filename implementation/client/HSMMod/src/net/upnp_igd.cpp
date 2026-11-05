#include "net/upnp.h"
#include "log.h"
#include <miniupnpc.h>
#include <upnpcommands.h>
#include <chrono>
#include <cstring>
#include <string>

namespace hsm::net {
void UpnpMapping::Start(uint16_t port) {
    Stop(); stop_=false;
    worker_=std::thread([this,port] {
        int error=0;
        auto* devices=upnpDiscover(2000,nullptr,nullptr,UPNP_LOCAL_PORT_ANY,0,2,&error);
        UPNPUrls urls{}; IGDdatas igd{}; char lan[64]{},wan[64]{};
        const int found=devices?UPNP_GetValidIGD(devices,&urls,&igd,lan,sizeof lan,wan,sizeof wan):0;
        freeUPNPDevlist(devices);
        if((found!=UPNP_CONNECTED_IGD && found!=UPNP_PRIVATEIP_IGD) || stop_) {
            HSM_LOG("upnp: no available gateway; manually forward UDP %u to this PC (discovery %d)",port,error);
            FreeUPNPUrls(&urls); return;
        }
        const auto ps=std::to_string(port);
        // Never overwrite an existing rule, even one that happens to point here.
        char client[64]{},internal[16]{},desc[256]{},enabled[16]{},lease[16]{};
        auto query=[&] { return UPNP_GetSpecificPortMappingEntry(urls.controlURL,igd.first.servicetype,ps.c_str(),"UDP",nullptr,client,internal,desc,enabled,lease); };
        const int existing=query();
        if(existing==0) {
            HSM_LOG("upnp: UDP %u already maps to %s:%s; leaving the existing rule unchanged",port,client,internal);
            FreeUPNPUrls(&urls); return;
        }
        if(existing!=714) {
            HSM_LOG("upnp: cannot inspect UDP %u mapping (error %d); use manual forwarding",port,existing);
            FreeUPNPUrls(&urls); return;
        }
        const char* description="Half Sword Direct Multiplayer";
        const char* duration="3600";
        auto add=[&] { return UPNP_AddPortMapping(urls.controlURL,igd.first.servicetype,ps.c_str(),ps.c_str(),lan,description,"UDP",nullptr,duration); };
        int result=stop_?-1:add();
        if(result==725 && !stop_) { duration="0"; result=add(); }
        const bool created=result==0;
        HSM_LOG("upnp: UDP %u mapping %s (error %d); external IPv4 %s, internal %s",port,created?"created":"failed; use manual forwarding",result,wan,lan);
        auto renewed=std::chrono::steady_clock::now();
        while(created && !stop_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if(std::chrono::steady_clock::now()-renewed>std::chrono::minutes(20)) {
                renewed=std::chrono::steady_clock::now();
                const int q=query();
                if(q==714 || (q==0 && std::strcmp(client,lan)==0 && std::strcmp(internal,ps.c_str())==0 && std::strcmp(desc,description)==0)) {
                    HSM_LOG("upnp: UDP %u lease renewal returned %d",port,add());
                } else { HSM_LOG("upnp: UDP %u rule changed; leaving it unchanged",port); break; }
            }
        }
        if(created && query()==0 && std::strcmp(client,lan)==0 && std::strcmp(internal,ps.c_str())==0 && std::strcmp(desc,description)==0) {
            HSM_LOG("upnp: removing owned UDP %u mapping (result %d)",port,UPNP_DeletePortMapping(urls.controlURL,igd.first.servicetype,ps.c_str(),"UDP",nullptr));
        }
        FreeUPNPUrls(&urls);
    });
}
void UpnpMapping::Stop() { stop_=true; if(worker_.joinable()) worker_.join(); }
}
