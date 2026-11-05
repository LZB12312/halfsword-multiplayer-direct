#include "net/update_check.h"
#include "net/http_client.h"
#include "gameplay/lobby.h"
#include "log.h"
#include "session/hsm_pe.h"
#include "session/hsm_update.h"
#include "session/hsm_version.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace hsm::update {
namespace {
constexpr const char kRepo[] = "LZB12312/halfsword-multiplayer-direct";

std::atomic<bool> g_started{false};
std::mutex g_mu;
std::string g_notice;
bool g_chatShown = false;
// UPDATE NOW: the release the check offered, and how the install is going (all under g_mu).
enum class Install : uint8_t { None, Running, Done, Failed };
Install g_install = Install::None;
std::string g_tag, g_installText;
UpdateAssets g_assets;
bool g_haveAssets = false;

// "check failed (...)" reason: what the player or a bug report can act on.
std::string Reason(const net::HttpResponse& r, const char* host = "api.github.com") {
    using Fail = net::HttpResponse::Fail;
    switch (r.fail) {
        case Fail::None: break;
        case Fail::Unavailable: return "WinHTTP is unavailable";
        case Fail::NoConnection: return "no connection";
        case Fail::Timeout: return "GitHub did not answer";
        case Fail::Certificate: return "GitHub's certificate is not valid";
        case Fail::Unreachable: return std::string("cannot reach ") + host;
        case Fail::Other: return "request failed (" + std::to_string(r.winError) + ")";
    }
    if (r.status == 403 || r.status == 429) return "GitHub is limiting requests (" + std::to_string(r.status) + ")";
    return "GitHub answered " + std::to_string(r.status);
}

void Check() {
    const std::optional<Version> running = RunningVersion(game::kModVersion);
    if (!running) { HSM_LOG("update: check failed (this build's version %s is not known)", game::kModVersion); return; }
    const std::string cur = FormatVersion(*running);

    net::HttpRequest req;
    req.host = L"api.github.com";
    req.path = L"/repos/" + net::Wide(kRepo) + L"/releases?per_page=10";
    req.headers = L"Accept: application/vnd.github+json";
    req.agent = L"HSMMod/" + net::Wide(cur);   // GitHub refuses a request without a User-Agent
    req.maxBody = kMaxReleasesJson;
    req.timeoutMs = 15000;
    const net::HttpResponse r = net::Http(req);
    if (r.fail != net::HttpResponse::Fail::None || r.status != 200) { HSM_LOG("update: check failed (%s)", Reason(r).c_str()); return; }
    const ReleaseList list = ParseReleases(r.body);
    if (list.releases.empty() && !list.complete) { HSM_LOG("update: check failed (the answer was not a release list)"); return; }

    const std::optional<UpdateOffer> offer = NewestOffer(list.releases, *running);
    if (!offer) { HSM_LOG("update: up to date (%s)", cur.c_str()); return; }
    const std::string latest = FormatVersion(offer->version);
    const std::string url = offer->url.empty() ? std::string("https://github.com/") + kRepo + "/releases" : offer->url;
    HSM_LOG("update: HSMMod %s is available (this is %s): %s", latest.c_str(), cur.c_str(), url.c_str());
    ReleaseInfo release;
    release.tag = offer->tag;
    release.assets = offer->assets;
    UpdateAssets assets;
    const AssetsVerdict av = SelectUpdateAssets(release, kRepo, assets);
    if (av != AssetsVerdict::Ok) HSM_LOG("update: no one-click update (%s)", AssetsVerdictText(av));
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_notice = "HSMMod " + latest + " is out (you have " + cur + "). Source: " + kRepo;
        if (av == AssetsVerdict::Ok) { g_tag = offer->tag; g_assets = assets; g_haveAssets = true; }
    }
    // Download and verify in the background. The loaded code stays unchanged;
    // the installed version takes effect only after restarting the game.
    if (av == AssetsVerdict::Ok) StartInstall();
}

// ------------------------------------------------------------------ UPDATE NOW

struct Fetched { bool ok = false; std::string body, error; };

// One GET with the redirects followed by hand: every hop must stay on GitHub's release hosts over https (the update
// rules in session/hsm_update.h), at most kMaxUpdateRedirects of them. More than maxBody bytes is an error, not a cut.
Fetched Fetch(const std::string& startUrl, size_t maxBody, const std::string& cur) {
    Fetched f;
    std::string url = startUrl;
    for (int hop = 0; hop <= kMaxUpdateRedirects; ++hop) {
        std::string host, path;
        if (!ParseUpdateUrl(url, host, path)) { f.error = "the download link is not from GitHub"; return f; }
        net::HttpRequest req;
        req.host = net::Wide(host);
        req.path = net::Wide(path);
        req.headers = L"Accept: application/octet-stream";
        req.agent = L"HSMMod/" + net::Wide(cur);
        req.maxBody = maxBody + 1;
        req.timeoutMs = 60000;
        req.followRedirects = false;
        const net::HttpResponse r = net::Http(req);
        if (r.fail != net::HttpResponse::Fail::None) { f.error = Reason(r, host.c_str()); return f; }
        if (RedirectStatus(r.status)) { url = r.location; continue; }
        if (r.status != 200) { f.error = "GitHub answered " + std::to_string(r.status); return f; }
        if (r.body.size() > maxBody) { f.error = "the download is too big"; return f; }
        f.ok = true;
        f.body = r.body;
        return f;
    }
    f.error = "too many redirects";
    return f;
}

std::wstring RunningDllPath() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&RunningDllPath), &self) || !self) return {};
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
    return n == 0 || n >= MAX_PATH ? std::wstring() : std::wstring(buf, n);
}

// The mod must be the file "...\dwmapi.dll" for the swap to mean anything.
bool IsDwmapiPath(const std::wstring& s) {
    static const wchar_t kTail[] = L"\\dwmapi.dll";
    const size_t n = sizeof kTail / sizeof kTail[0] - 1;
    return s.size() > n && _wcsicmp(s.c_str() + s.size() - n, kTail) == 0;
}

bool WriteWhole(const std::wstring& path, const std::string& bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    for (size_t off = 0; ok && off < bytes.size();) {
        const DWORD chunk = DWORD(std::min<size_t>(bytes.size() - off, size_t(1) << 20));
        DWORD wrote = 0;
        ok = WriteFile(h, bytes.data() + off, chunk, &wrote, nullptr) && wrote == chunk;
        off += wrote;
    }
    ok = ok && FlushFileBuffers(h);
    return CloseHandle(h) && ok;
}

// The file as written reads back as the verified bytes (a full disk or a failing write is caught here).
bool ReadsBackAs(const std::wstring& path, const Sha256Digest& expected, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    Sha256 sha;
    std::vector<uint8_t> buf(size_t(1) << 16);
    size_t total = 0;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), DWORD(buf.size()), &got, nullptr)) { CloseHandle(h); return false; }
        if (got == 0) break;
        sha.Update(buf.data(), got);
        total += got;
    }
    CloseHandle(h);
    return total == size && sha.Final() == expected;
}

void SetInstall(Install state, const std::string& text) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_install = state;
    g_installText = text;
}

void Fail(const std::string& why) {
    HSM_LOG("update: failed (%s)", why.c_str());
    SetInstall(Install::Failed, "Update failed: " + why);
}

void InstallWorker(std::string tag, UpdateAssets assets, std::string cur) {
    const std::wstring dll = RunningDllPath();
    if (!IsDwmapiPath(dll)) return Fail("the mod is not running as dwmapi.dll");
    const std::wstring fresh = dll + L".new", old = dll + L".old";

    char size[32];
    std::snprintf(size, sizeof size, "%.1f", double(assets.dllSize) / (1024.0 * 1024.0));
    HSM_LOG("update: downloading %s (%llu bytes)", tag.c_str(), static_cast<unsigned long long>(assets.dllSize));
    SetInstall(Install::Running, std::string("Downloading ") + size + " MB...");

    const Fetched shaFile = Fetch(assets.shaUrl, kMaxUpdateShaBytes, cur);
    if (!shaFile.ok) return Fail(shaFile.error);
    Sha256Digest fromFile{}, expected{};
    if (!ParseUpdateShaFile(shaFile.body, fromFile)) return Fail("the release's SHA-256 file is not valid");
    if (!ExpectedDllDigest(fromFile, assets, expected)) return Fail("the release's SHA-256 and GitHub's digest disagree");

    const Fetched file = Fetch(assets.dllUrl, size_t(kMaxUpdateDllBytes), cur);
    if (!file.ok) return Fail(file.error);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(file.body.data());
    const DownloadVerdict dv = CheckDownload(bytes, file.body.size(), assets.dllSize, expected);
    if (dv != DownloadVerdict::Ok) return Fail(DownloadVerdictText(dv));
    HSM_LOG("update: sha256 ok");
    const PeVerdict pv = CheckProxyDll(bytes, file.body.size(), DwmapiRequiredExports());
    if (pv != PeVerdict::Ok) return Fail(PeVerdictText(pv));

    SwapPlan plan;
    for (int guard = 0; guard < 8 && plan.Next() != SwapAction::Done; ++guard) {
        bool ok = false;
        switch (plan.Next()) {
            case SwapAction::WriteNew: ok = WriteWhole(fresh, file.body) && ReadsBackAs(fresh, expected, file.body.size()); break;
            case SwapAction::MoveCurrentAside: ok = MoveFileExW(dll.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) != 0; break;
            case SwapAction::MoveNewIn: ok = MoveFileExW(fresh.c_str(), dll.c_str(), 0) != 0; break;
            case SwapAction::RestoreOld: ok = MoveFileExW(old.c_str(), dll.c_str(), 0) != 0; break;
            case SwapAction::DeleteNew: ok = DeleteFileW(fresh.c_str()) != 0; break;
            case SwapAction::Done: break;
        }
        plan.Report(ok);
    }
    if (plan.outcome() != SwapOutcome::Installed) return Fail(SwapOutcomeText(plan.outcome()));
    HSM_LOG("update: installed %s, restart the game", tag.c_str());
    SetInstall(Install::Done, "Update ready: restart the game");
}

// Start of the game after an update: the previous DLL (renamed aside while it ran) is not loaded any more.
void RemoveLeftovers() {
    const std::wstring dll = RunningDllPath();
    if (!IsDwmapiPath(dll)) return;
    DeleteFileW((dll + L".new").c_str());
    if (DeleteFileW((dll + L".old").c_str())) HSM_LOG("update: removed the old dll");
}
}  // namespace

void Start(bool enabled) {
    RemoveLeftovers();
    if (!enabled) { HSM_LOG("update: check off (update_check=0)"); return; }
    if (g_started.exchange(true)) return;
    std::thread(Check).detach();
}

std::string Notice() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_notice;
}

bool CanInstall() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_haveAssets && (g_install == Install::None || g_install == Install::Failed);
}

void StartInstall() {
    UpdateAssets assets;
    std::string tag;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (!g_haveAssets || (g_install != Install::None && g_install != Install::Failed)) return;
        g_install = Install::Running;
        g_installText = "Downloading...";
        assets = g_assets;
        tag = g_tag;
    }
    const std::optional<Version> running = RunningVersion(game::kModVersion);
    std::thread(InstallWorker, tag, assets, running ? FormatVersion(*running) : std::string("0")).detach();
}

std::string InstallStatus() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_installText;
}

std::string TakeChatNotice() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_notice.empty() || g_chatShown) return {};
    g_chatShown = true;
    return g_notice;
}
}  // namespace hsm::update
