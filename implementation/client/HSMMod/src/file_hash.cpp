#include "file_hash.h"
#include "session/hsm_build.h"
#include <windows.h>
#include <vector>

namespace hsm {

bool HashFile(const std::wstring& path, Sha256Digest& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    Sha256 s;
    std::vector<uint8_t> buf(1 << 20);
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), DWORD(buf.size()), &got, nullptr)) { ok = false; break; }
        if (got == 0) break;
        s.Update(buf.data(), got);
    }
    CloseHandle(h);
    if (ok) out = s.Final();
    return ok;
}

std::wstring OwnModulePath() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&OwnModulePath), &self))
        return {};
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(self, path.data(), DWORD(path.size()));
        if (n == 0) return {};
        if (n < path.size()) { path.resize(n); return path; }
        if (path.size() >= 32768) return {};
        path.resize(path.size() * 2);
    }
}

const Sha256Digest& OwnBinaryDigest() {
    static const Sha256Digest digest = [] {
        const std::wstring path = OwnModulePath();
        Sha256Digest d{};
        if (path.empty() || !HashFile(path, d)) d = Sha256Digest{};
        return d;
    }();
    return digest;
}

const std::string& OwnBinaryHash() {
    static const std::string hash = Sha256IsZero(OwnBinaryDigest()) ? std::string{} : BuildHash(OwnBinaryDigest());
    return hash;
}

}  // namespace hsm
