// UPDATE NOW (HSMMod 0.8.0): the rules of the self-update, pure and unit-tested. The mod downloads the release's
// dwmapi.dll, checks its SHA-256 against the release's dwmapi.dll.sha256 (and GitHub's own digest of the asset, which
// must agree when it is given), checks that it is a 64-bit proxy DLL (session/hsm_pe.h), then swaps it in for the
// running one; the player restarts the game. Nothing here touches the network or the disk: net/update_check.cpp does
// the I/O and asks these rules at every step.
//
//   assets      which two release assets, from which URLs (exactly this repo's release download path, never another)
//   hosts       which hosts a download or a redirect may reach (GitHub's release storage), https only, no port
//   sha file    "<64 hex>  dwmapi.dll" as `sha256sum` writes it
//   download    size cap, size and hash checks of the bytes
//   swap        the file moves as a state machine: every failing step has one outcome and leaves a working DLL
#pragma once
#include "session/hsm_version.h"
#include "util/hsm_sha256.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace hsm {

inline constexpr uint64_t kMaxUpdateDllBytes = 16ull << 20;   // 16 MiB; the mod is about 4 MB
inline constexpr size_t kMaxUpdateShaBytes = 1024;
inline constexpr size_t kMaxUpdateLocation = 2048;            // a redirect target (signed storage URLs are long)
inline constexpr int kMaxUpdateRedirects = 4;
inline constexpr const char kUpdateDllAsset[] = "dwmapi.dll";
inline constexpr const char kUpdateShaAsset[] = "dwmapi.dll.sha256";

// ------------------------------------------------------------------ assets

struct UpdateAssets {
    std::string dllUrl, shaUrl;
    uint64_t dllSize = 0;
    bool haveApiDigest = false;
    Sha256Digest apiDigest{};
};

enum class AssetsVerdict : uint8_t { Ok, BadTag, MissingDll, MissingSha, BadUrl, BadSize, BadDigest };

inline const char* AssetsVerdictText(AssetsVerdict v) {
    switch (v) {
        case AssetsVerdict::Ok: return "ok";
        case AssetsVerdict::BadTag: return "the release tag is not usable";
        case AssetsVerdict::MissingDll: return "the release has no dwmapi.dll";
        case AssetsVerdict::MissingSha: return "the release has no dwmapi.dll.sha256";
        case AssetsVerdict::BadUrl: return "the release's download link is not from GitHub";
        case AssetsVerdict::BadSize: return "the release's file size is not usable";
        case AssetsVerdict::BadDigest: return "the release's digest is not usable";
    }
    return "the release is not usable";
}

inline bool UpdateTagOk(std::string_view t) {
    if (t.empty() || t.size() > kMaxReleaseTag) return false;
    for (char c : t) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

// GitHub's "digest" of an asset: "sha256:<64 hex>".
inline bool ParseApiDigest(const std::string& s, Sha256Digest& out) {
    static const char kPrefix[] = "sha256:";
    return s.size() == sizeof kPrefix - 1 + 64 && s.compare(0, sizeof kPrefix - 1, kPrefix) == 0 && Sha256FromHex(s.substr(sizeof kPrefix - 1), out);
}

// The two files of release `r` of `repo` ("owner/name"). Each asset's download link must be exactly
// https://github.com/<repo>/releases/download/<tag>/<asset name>: another host, another repo, another tag or a query
// is refused. The DLL asset needs a size of 1..kMaxUpdateDllBytes; a digest, when present, must be well formed.
inline AssetsVerdict SelectUpdateAssets(const ReleaseInfo& r, std::string_view repo, UpdateAssets& out) {
    out = UpdateAssets{};
    if (!UpdateTagOk(r.tag)) return AssetsVerdict::BadTag;
    const ReleaseAsset *dll = nullptr, *sha = nullptr;
    for (const ReleaseAsset& a : r.assets) {
        if (!dll && a.name == kUpdateDllAsset) dll = &a;
        if (!sha && a.name == kUpdateShaAsset) sha = &a;
    }
    if (!dll) return AssetsVerdict::MissingDll;
    if (!sha) return AssetsVerdict::MissingSha;
    const std::string prefix = "https://github.com/" + std::string(repo) + "/releases/download/" + r.tag + "/";
    if (dll->url != prefix + kUpdateDllAsset || sha->url != prefix + kUpdateShaAsset) return AssetsVerdict::BadUrl;
    if (dll->size < 1 || dll->size > kMaxUpdateDllBytes) return AssetsVerdict::BadSize;
    if (sha->size > kMaxUpdateShaBytes) return AssetsVerdict::BadSize;
    if (!dll->digest.empty()) {
        if (!ParseApiDigest(dll->digest, out.apiDigest)) { out = UpdateAssets{}; return AssetsVerdict::BadDigest; }
        out.haveApiDigest = true;
    }
    out.dllUrl = dll->url; out.shaUrl = sha->url; out.dllSize = dll->size;
    return AssetsVerdict::Ok;
}

// ------------------------------------------------------------------ hosts

// A URL the update may fetch: https, one of GitHub's release hosts, no user, no port, a path of safe characters.
// Redirects (GitHub answers a release download with one to its storage) go through the same test.
inline bool UpdateHostOk(std::string_view host) {
    return host == "github.com" || host == "objects.githubusercontent.com" || host == "release-assets.githubusercontent.com";
}

inline bool ParseUpdateUrl(std::string_view url, std::string& host, std::string& path) {
    static const std::string_view kScheme = "https://";
    host.clear(); path.clear();
    if (url.size() <= kScheme.size() || url.size() > kMaxUpdateLocation || url.compare(0, kScheme.size(), kScheme) != 0) return false;
    const size_t slash = url.find('/', kScheme.size());
    if (slash == std::string_view::npos) return false;
    const std::string_view h = url.substr(kScheme.size(), slash - kScheme.size());
    if (!UpdateHostOk(h)) return false;   // also rules out "user@", ":port" and upper case
    for (char c : url.substr(slash)) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '-' || c == '.' || c == '_' || c == '~' || c == '/' || c == '?' || c == '&' || c == '=' || c == '%' ||
                        c == '+' || c == ':' || c == ',' || c == ';' || c == '@' || c == '!' || c == '$' || c == '*' || c == '(' || c == ')';
        if (!ok) return false;
    }
    host = std::string(h);
    path = std::string(url.substr(slash));
    return true;
}

inline bool RedirectStatus(unsigned status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

// ------------------------------------------------------------------ the sha file

// "<64 hex digits><one or two separator characters>dwmapi.dll", as `sha256sum` writes it ("  " text mode, " *" binary),
// then an optional line end. Nothing else: not another file name, not a second line.
inline bool ParseUpdateShaFile(const std::string& text, Sha256Digest& out) {
    if (text.empty() || text.size() > kMaxUpdateShaBytes) return false;
    size_t end = text.size();
    while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\r')) --end;
    const std::string line = text.substr(0, end);
    if (line.size() < 64 + 2 + sizeof kUpdateDllAsset - 1) return false;
    if (!Sha256FromHex(line.substr(0, 64), out)) return false;
    const std::string rest = line.substr(64);
    return rest == std::string("  ") + kUpdateDllAsset || rest == std::string(" *") + kUpdateDllAsset;
}

// The hash the downloaded DLL must have: the .sha256 file's, and the API's digest when GitHub gave one - a disagreement
// means one of the two answers was tampered with or stale, so nothing is installed.
inline bool ExpectedDllDigest(const Sha256Digest& fromFile, const UpdateAssets& assets, Sha256Digest& out) {
    if (assets.haveApiDigest && assets.apiDigest != fromFile) return false;
    out = fromFile;
    return true;
}

// Official builds (session/hsm_mod_rules.h OfficialOf): GitHub's digest of the dwmapi.dll of the release whose tag is
// exactly the running version (not a draft). False when the list does not hold that release (older than the page, or
// a build with no release) or its asset carries no well formed digest.
inline bool RunningReleaseDigest(const std::vector<ReleaseInfo>& releases, const Version& running, Sha256Digest& out) {
    for (const ReleaseInfo& r : releases) {
        if (r.draft) continue;
        const std::optional<Version> v = ParseVersion(r.tag);
        if (!v || CompareVersion(*v, running) != 0) continue;
        for (const ReleaseAsset& a : r.assets)
            if (a.name == kUpdateDllAsset) return ParseApiDigest(a.digest, out);
        return false;
    }
    return false;
}

// ------------------------------------------------------------------ the bytes

enum class DownloadVerdict : uint8_t { Ok, Empty, TooBig, SizeMismatch, HashMismatch };

inline const char* DownloadVerdictText(DownloadVerdict v) {
    switch (v) {
        case DownloadVerdict::Ok: return "ok";
        case DownloadVerdict::Empty: return "the download was empty";
        case DownloadVerdict::TooBig: return "the download is too big";
        case DownloadVerdict::SizeMismatch: return "the download has the wrong size";
        case DownloadVerdict::HashMismatch: return "the download does not match its SHA-256";
    }
    return "the download is not valid";
}

// apiSize: the size the release states (0 = unknown, then only the cap and the hash apply).
inline DownloadVerdict CheckDownload(const uint8_t* data, size_t n, uint64_t apiSize, const Sha256Digest& expected) {
    if (n == 0 || !data) return DownloadVerdict::Empty;
    if (n > kMaxUpdateDllBytes) return DownloadVerdict::TooBig;
    if (apiSize != 0 && apiSize != n) return DownloadVerdict::SizeMismatch;
    return Sha256Of(data, n) == expected ? DownloadVerdict::Ok : DownloadVerdict::HashMismatch;
}

// ------------------------------------------------------------------ the swap

// dwmapi.dll.new is written and read back; the running dwmapi.dll is renamed to dwmapi.dll.old (Windows lets a loaded DLL
// be renamed, not overwritten or deleted); the .new file takes its place. The caller performs each action `Next()` names
// and reports whether it worked; the plan decides what comes next:
//   WriteNew fails           -> remove .new                       -> FailedUnchanged (dwmapi.dll was never touched)
//   MoveCurrentAside fails   -> remove .new                       -> FailedUnchanged
//   MoveNewIn fails          -> move .old back (RestoreOld)       -> RolledBack, or Broken when that fails too
//   everything works         -> Installed (the .old stays until the next start removes it)
enum class SwapAction : uint8_t { WriteNew, MoveCurrentAside, MoveNewIn, RestoreOld, DeleteNew, Done };
enum class SwapOutcome : uint8_t { Running, Installed, FailedUnchanged, RolledBack, Broken };

class SwapPlan {
public:
    SwapAction Next() const {
        switch (state_) {
            case State::Write: return SwapAction::WriteNew;
            case State::Aside: return SwapAction::MoveCurrentAside;
            case State::In: return SwapAction::MoveNewIn;
            case State::Restore: return SwapAction::RestoreOld;
            case State::Cleanup: return SwapAction::DeleteNew;
            case State::Finished: break;
        }
        return SwapAction::Done;
    }
    void Report(bool ok) {
        switch (state_) {
            case State::Write: if (ok) state_ = State::Aside; else Cleanup(SwapOutcome::FailedUnchanged); break;
            case State::Aside: if (ok) state_ = State::In; else Cleanup(SwapOutcome::FailedUnchanged); break;
            case State::In: if (ok) Finish(SwapOutcome::Installed); else state_ = State::Restore; break;
            case State::Restore: if (ok) Cleanup(SwapOutcome::RolledBack); else Finish(SwapOutcome::Broken); break;
            case State::Cleanup: Finish(pending_); break;   // removing the temporary file is best effort
            case State::Finished: break;
        }
    }
    SwapOutcome outcome() const { return outcome_; }
private:
    enum class State : uint8_t { Write, Aside, In, Restore, Cleanup, Finished };
    void Cleanup(SwapOutcome then) { pending_ = then; state_ = State::Cleanup; }
    void Finish(SwapOutcome o) { outcome_ = o; state_ = State::Finished; }
    State state_ = State::Write;
    SwapOutcome pending_ = SwapOutcome::Running, outcome_ = SwapOutcome::Running;
};

inline const char* SwapOutcomeText(SwapOutcome o) {
    switch (o) {
        case SwapOutcome::Installed: return "installed";
        case SwapOutcome::FailedUnchanged: return "could not write the new file next to the game";
        case SwapOutcome::RolledBack: return "could not put the new file in place; the old one is back";
        case SwapOutcome::Broken: return "could not put the new file in place and could not restore dwmapi.dll.old: rename it to dwmapi.dll";
        case SwapOutcome::Running: break;
    }
    return "not finished";
}

}  // namespace hsm
