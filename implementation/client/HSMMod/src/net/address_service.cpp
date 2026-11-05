#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "net/address_service.h"
#include "net/http_client.h"
#include "session/hsm_moderation.h"
#include <cstring>
namespace hsm::net {
void AddressService::Start(bool publicLookup) {
    Stop();
    { std::lock_guard<std::mutex> lock(mu_); public_.clear(); local_.clear(); }
    worker_=std::thread([this,publicLookup] {
        SOCKET s=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
        sockaddr_in dest{},local{}; dest.sin_family=AF_INET; dest.sin_port=htons(9);
        inet_pton(AF_INET,"1.1.1.1",&dest.sin_addr); int size=sizeof local; char ip[64]{};
        if(s!=INVALID_SOCKET && connect(s,reinterpret_cast<sockaddr*>(&dest),sizeof dest)==0 && getsockname(s,reinterpret_cast<sockaddr*>(&local),&size)==0) {
            if(inet_ntop(AF_INET,&local.sin_addr,ip,sizeof ip)) { std::lock_guard<std::mutex> lock(mu_); local_=ip; }
        }
        if(s!=INVALID_SOCKET) closesocket(s);
        if(!publicLookup) return;
        HttpRequest req; req.host=L"api.ipify.org"; req.path=L"/"; req.agent=L"HalfSword-Direct"; req.maxBody=64; req.timeoutMs=5000;
        const auto response=Http(req);
        if(response.fail==HttpResponse::Fail::None && response.status==200 && ValidIpv4(response.body)) {
            std::lock_guard<std::mutex> lock(mu_); public_=response.body;
        }
    });
}
bool CopyText(const std::string& text) {
    if(text.empty() || text.size()>4096) return false;
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.c_str(),-1,nullptr,0);
    if(!count) return false;
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,size_t(count)*sizeof(wchar_t));
    if(!memory) return false;
    auto* p=static_cast<wchar_t*>(GlobalLock(memory));
    if(!p) { GlobalFree(memory); return false; }
    MultiByteToWideChar(CP_UTF8,0,text.c_str(),-1,p,count); GlobalUnlock(memory);
    HWND owner=GetActiveWindow(); if(!owner) owner=GetForegroundWindow();
    if(!owner || !OpenClipboard(owner)) { GlobalFree(memory); return false; }
    const bool ok=EmptyClipboard() && SetClipboardData(CF_UNICODETEXT,memory);
    CloseClipboard(); if(!ok) GlobalFree(memory); return ok;
}
std::string ClipboardText() {
    std::string result;
    if(!OpenClipboard(nullptr)) return result;
    HANDLE h=GetClipboardData(CF_UNICODETEXT);
    if(h) {
        const auto bytes=GlobalSize(h); const auto* p=static_cast<const wchar_t*>(GlobalLock(h));
        if(p) {
            size_t count=0,limit=std::min<size_t>(bytes/sizeof(wchar_t),512);
            while(count<limit && p[count]) ++count;
            if(count && count<limit) {
                const int n=WideCharToMultiByte(CP_UTF8,0,p,int(count),nullptr,0,nullptr,nullptr);
                if(n>0) { result.resize(size_t(n)); WideCharToMultiByte(CP_UTF8,0,p,int(count),result.data(),n,nullptr,nullptr); }
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard(); return result;
}
}
