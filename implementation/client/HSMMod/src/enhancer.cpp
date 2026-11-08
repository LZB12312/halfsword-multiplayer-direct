#include "enhancer.h"
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <string>
#include "log.h"
#include "session/hsm_enhancer.h"

namespace hsm::enhancer {
namespace {
constexpr uint64_t kScanEveryMs = 5000;
bool g_detected = false;
uint64_t g_nextScan = 0;

std::string Utf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// The module's own version resource, read in place: it is loaded already, so nothing is mapped or run.
bool VersionNamesIt(HMODULE m) {
    HRSRC r = FindResourceW(m, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));   // RT_VERSION, id 1
    if (!r) return false;
    const DWORD size = SizeofResource(m, r);
    HGLOBAL g = LoadResource(m, r);
    const auto* p = g ? static_cast<const uint8_t*>(LockResource(g)) : nullptr;
    return p && size <= (64u << 10) && VersionNamesEnhancer(p, size);
}

EnhancerSignals Check(const MODULEENTRY32W& me) {
    EnhancerSignals s;
    const std::string path = Utf8(me.szExePath);
    s.fileName = IsEnhancerFileName(FileNameOf(path));
    s.exportName = GetProcAddress(me.hModule, kEnhancerExport) != nullptr;
    s.version = VersionNamesIt(me.hModule);
    return s;
}

void Scan() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;   // ERROR_BAD_LENGTH while modules load: the next poll tries again
    MODULEENTRY32W me{}; me.dwSize = sizeof me;
    for (BOOL ok = Module32FirstW(snap, &me); ok && !g_detected; ok = Module32NextW(snap, &me)) {
        const EnhancerSignals s = Check(me);   // every module each time: a few hundred resource lookups, every 5 s
        if (!s.Matches()) continue;
        g_detected = true;
        HSM_LOG("enhancer: Half Sword Enhancer detected: %s (name=%d export=%d version=%d); lobbies that block it refuse this player",
                Utf8(me.szExePath).c_str(), s.fileName ? 1 : 0, s.exportName ? 1 : 0, s.version ? 1 : 0);
    }
    CloseHandle(snap);
}
}  // namespace

void Poll() {
    const uint64_t nowMs = GetTickCount64();
    if (g_detected || nowMs < g_nextScan) return;
    g_nextScan = nowMs + kScanEveryMs;
    Scan();
}

bool Detected() { return g_detected; }
}  // namespace hsm::enhancer
