// Unit tests for hsm_update.h and the release-asset scan of hsm_version.h. Run via implementation/shared/run_tests.sh test_hsm_update.
#include "session/hsm_update.h"
#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static const char kRepo[] = "Dudiebug/halfsword-multiplayer-mod";
static const std::string kHex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

static std::string Base(const std::string& tag) { return "https://github.com/" + std::string(kRepo) + "/releases/download/" + tag + "/"; }

static std::string AssetJson(const std::string& name, const std::string& url, const std::string& size, const std::string& digest) {
    return "{\"url\":\"https://api.github.com/x\",\"id\":1,\"name\":\"" + name + "\",\"uploader\":{\"login\":\"a\",\"html_url\":\"https://github.com/a\"},"
           "\"size\":" + size + ",\"digest\":" + digest + ",\"browser_download_url\":\"" + url + "\"}";
}

static std::string ReleaseJson(const std::string& tag, const std::string& assets) {
    return "{\"url\":\"https://api.github.com/r\",\"tag_name\":\"" + tag + "\",\"html_url\":\"https://github.com/o/r/releases/tag/" + tag +
           "\",\"author\":{\"login\":\"x\"},\"draft\":false,\"prerelease\":true,\"assets\":[" + assets + "],\"body\":\"notes [1] {2}\"}";
}

static std::string Good(const std::string& tag, const std::string& digest = "\"sha256:" + kHex + "\"") {
    return ReleaseJson(tag, AssetJson("dwmapi.dll", Base(tag) + "dwmapi.dll", "4000000", digest) + "," +
                            AssetJson("dwmapi.dll.sha256", Base(tag) + "dwmapi.dll.sha256", "90", "null"));
}

static ReleaseInfo One(const std::string& json) {
    ReleaseList l = ParseReleases("[" + json + "]");
    CHECK(l.complete && l.releases.size() == 1);
    return l.releases.empty() ? ReleaseInfo{} : l.releases[0];
}

static void TestScan() {
    ReleaseList l = ParseReleases("[" + Good("v0.8.0-pre25") + "," + ReleaseJson("v0.8.0-pre24", "") + "]");
    CHECK(l.complete && l.releases.size() == 2);
    CHECK(l.releases[0].tag == "v0.8.0-pre25" && l.releases[0].prerelease && l.releases[0].assets.size() == 2);
    CHECK(l.releases[0].assets[0].name == "dwmapi.dll" && l.releases[0].assets[0].size == 4000000);
    CHECK(l.releases[0].assets[0].digest == "sha256:" + kHex);
    CHECK(l.releases[0].assets[0].url == Base("v0.8.0-pre25") + "dwmapi.dll");
    CHECK(l.releases[0].assets[1].name == "dwmapi.dll.sha256" && l.releases[0].assets[1].digest.empty());
    CHECK(l.releases[1].assets.empty());
    // an uploader's html_url or name never replaces the asset's own fields, and the release keeps its own html_url
    CHECK(l.releases[0].url == "https://github.com/o/r/releases/tag/v0.8.0-pre25");
    // assets: null, or assets that are not an array, leave the release without assets
    l = ParseReleases("[{\"tag_name\":\"v1.0.0\",\"assets\":null,\"x\":[{\"name\":\"dwmapi.dll\"}]}]");
    CHECK(l.complete && l.releases.size() == 1 && l.releases[0].assets.empty());
    // a nested array that is not "assets" is not read as assets
    l = ParseReleases("[{\"tag_name\":\"v1.0.0\",\"other\":[{\"name\":\"dwmapi.dll\"}]}]");
    CHECK(l.complete && l.releases[0].assets.empty());
    // an asset without a name is dropped; an over-long url is emptied
    l = ParseReleases("[{\"tag_name\":\"v1.0.0\",\"assets\":[{\"size\":5},{\"name\":\"a\",\"browser_download_url\":\"https://github.com/" + std::string(400, 'x') + "\"}]}]");
    CHECK(l.complete && l.releases[0].assets.size() == 1 && l.releases[0].assets[0].url.empty());
    // at most kMaxReleaseAssets are kept
    std::string many;
    for (int i = 0; i < 40; ++i) many += (i ? "," : "") + AssetJson("f" + std::to_string(i), "https://github.com/x", "1", "null");
    l = ParseReleases("[" + ReleaseJson("v1.0.0", many) + "]");
    CHECK(l.complete && l.releases[0].assets.size() == kMaxReleaseAssets);
    // a huge size is not a number we trust
    l = ParseReleases("[{\"tag_name\":\"v1.0.0\",\"assets\":[{\"name\":\"a\",\"size\":99999999999999999999}]}]");
    CHECK(l.complete && l.releases[0].assets[0].size == 0);
    // damaged inside the assets: incomplete
    l = ParseReleases("[{\"tag_name\":\"v1.0.0\",\"assets\":[{\"name\":\"dwm");
    CHECK(!l.complete);
}

static void TestSelect() {
    UpdateAssets a;
    ReleaseInfo r = One(Good("v0.8.0-pre25"));
    CHECK(SelectUpdateAssets(r, kRepo, a) == AssetsVerdict::Ok);
    CHECK(a.dllUrl == Base("v0.8.0-pre25") + "dwmapi.dll" && a.shaUrl == Base("v0.8.0-pre25") + "dwmapi.dll.sha256");
    CHECK(a.dllSize == 4000000 && a.haveApiDigest && Sha256Hex(a.apiDigest) == kHex);
    // no digest from the API is fine (the .sha256 file stays the source)
    r = One(Good("v0.8.0-pre25", "null"));
    CHECK(SelectUpdateAssets(r, kRepo, a) == AssetsVerdict::Ok && !a.haveApiDigest);
    // a malformed digest is refused, not ignored
    for (const char* d : {"\"md5:abcd\"", "\"sha256:xyz\"", "\"sha256:\"", "\"\\u0073ha256:00\""}) {
        r = One(Good("v0.8.0-pre25", d));
        CHECK(SelectUpdateAssets(r, kRepo, a) == AssetsVerdict::BadDigest && a.dllUrl.empty());
    }
    // missing assets
    r = One(ReleaseJson("v0.8.0-pre25", AssetJson("dwmapi.dll.sha256", Base("v0.8.0-pre25") + "dwmapi.dll.sha256", "90", "null")));
    CHECK(SelectUpdateAssets(r, kRepo, a) == AssetsVerdict::MissingDll);
    r = One(ReleaseJson("v0.8.0-pre25", AssetJson("dwmapi.dll", Base("v0.8.0-pre25") + "dwmapi.dll", "4000000", "null")));
    CHECK(SelectUpdateAssets(r, kRepo, a) == AssetsVerdict::MissingSha);
    r = One(ReleaseJson("v0.8.0-pre25", ""));
    CHECK(SelectUpdateAssets(r, kRepo, a) == AssetsVerdict::MissingDll);
    // wrong host, wrong repo, wrong tag, a query, http
    const std::string tag = "v0.8.0-pre25";
    for (const std::string& bad : {std::string("https://evil.example/") + std::string(kRepo) + "/releases/download/" + tag + "/dwmapi.dll",
                                   std::string("https://github.com.evil.example/") + kRepo + "/releases/download/" + tag + "/dwmapi.dll",
                                   std::string("https://github.com/Other/repo/releases/download/") + tag + "/dwmapi.dll",
                                   Base("v9.9.9") + "dwmapi.dll",
                                   Base(tag) + "dwmapi.dll?x=1",
                                   Base(tag) + "../dwmapi.dll",
                                   "http://github.com/" + std::string(kRepo) + "/releases/download/" + tag + "/dwmapi.dll",
                                   Base(tag) + "other.dll"}) {
        r = One(ReleaseJson(tag, AssetJson("dwmapi.dll", bad, "4000000", "null") + "," + AssetJson("dwmapi.dll.sha256", Base(tag) + "dwmapi.dll.sha256", "90", "null")));
        CHECK(SelectUpdateAssets(r, kRepo, a) == AssetsVerdict::BadUrl);
        r = One(ReleaseJson(tag, AssetJson("dwmapi.dll", Base(tag) + "dwmapi.dll", "4000000", "null") + "," + AssetJson("dwmapi.dll.sha256", bad, "90", "null")));
        CHECK(SelectUpdateAssets(r, kRepo, a) == AssetsVerdict::BadUrl);
    }
    // sizes: zero, over the cap, missing
    for (const char* sz : {"0", "16777217", "99999999999"}) {
        r = One(ReleaseJson(tag, AssetJson("dwmapi.dll", Base(tag) + "dwmapi.dll", sz, "null") + "," + AssetJson("dwmapi.dll.sha256", Base(tag) + "dwmapi.dll.sha256", "90", "null")));
        CHECK(SelectUpdateAssets(r, kRepo, a) == AssetsVerdict::BadSize);
    }
    r = One(ReleaseJson(tag, AssetJson("dwmapi.dll", Base(tag) + "dwmapi.dll", "16777216", "null") + "," + AssetJson("dwmapi.dll.sha256", Base(tag) + "dwmapi.dll.sha256", "90", "null")));
    CHECK(SelectUpdateAssets(r, kRepo, a) == AssetsVerdict::Ok);
    r = One(ReleaseJson(tag, AssetJson("dwmapi.dll", Base(tag) + "dwmapi.dll", "4000000", "null") + "," + AssetJson("dwmapi.dll.sha256", Base(tag) + "dwmapi.dll.sha256", "5000", "null")));
    CHECK(SelectUpdateAssets(r, kRepo, a) == AssetsVerdict::BadSize);
    // a tag that could change the path
    r = One(Good("v0.8.0-pre25"));
    r.tag = "v1/../x";
    CHECK(SelectUpdateAssets(r, kRepo, a) == AssetsVerdict::BadTag);
    r.tag = "";
    CHECK(SelectUpdateAssets(r, kRepo, a) == AssetsVerdict::BadTag);
    // the offer carries the assets
    r = One(Good("v0.8.0-pre25"));
    std::vector<ReleaseInfo> list{r};
    auto o = NewestOffer(list, Version{0, 8, 0, 24});
    CHECK(o && o->assets.size() == 2);
}

static void TestUrls() {
    std::string host, path;
    CHECK(ParseUpdateUrl("https://github.com/o/r/releases/download/v1/dwmapi.dll", host, path) && host == "github.com" && path == "/o/r/releases/download/v1/dwmapi.dll");
    CHECK(ParseUpdateUrl("https://release-assets.githubusercontent.com/github-production-release-asset/1/2?sp=r&sv=2018-11-09&sig=a%2Bb%3D&se=2026-10-31T10%3A00%3A00Z&rscd=attachment%3B+filename%3Ddwmapi.dll", host, path) &&
          host == "release-assets.githubusercontent.com");
    CHECK(ParseUpdateUrl("https://objects.githubusercontent.com/x", host, path) && host == "objects.githubusercontent.com");
    for (const char* bad : {"http://github.com/x", "https://evil.example/x", "https://github.com.evil.example/x", "https://github.com@evil.example/x",
                            "https://evil.example@github.com/x", "https://github.com:8443/x", "https://github.com", "https://GITHUB.com/x",
                            "https://raw.githubusercontent.com/x", "https://api.github.com/x", "//github.com/x", "/relative", "",
                            "https://github.com/a b", "https://github.com/a\"b", "https://github.com/a#b", "https://github.com/a\\b",
                            "ftp://github.com/x", "https:/github.com/x"})
        CHECK(!ParseUpdateUrl(bad, host, path) && host.empty() && path.empty());
    CHECK(!ParseUpdateUrl("https://github.com/" + std::string(kMaxUpdateLocation, 'a'), host, path));
    CHECK(RedirectStatus(302) && RedirectStatus(301) && RedirectStatus(307) && RedirectStatus(308) && RedirectStatus(303));
    CHECK(!RedirectStatus(200) && !RedirectStatus(304) && !RedirectStatus(404) && !RedirectStatus(300));
}

static void TestShaFile() {
    Sha256Digest d{};
    CHECK(ParseUpdateShaFile(kHex + "  dwmapi.dll\n", d) && Sha256Hex(d) == kHex);
    CHECK(ParseUpdateShaFile(kHex + "  dwmapi.dll", d));
    CHECK(ParseUpdateShaFile(kHex + " *dwmapi.dll\r\n", d));
    CHECK(ParseUpdateShaFile("0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF  dwmapi.dll\n", d) && Sha256Hex(d) == kHex);
    for (const std::string& bad : {std::string(""), kHex, kHex + "  other.dll\n", kHex + " dwmapi.dll\n", kHex + "   dwmapi.dll\n",
                                   kHex.substr(1) + "  dwmapi.dll\n", kHex + "0  dwmapi.dll\n", "zz" + kHex.substr(2) + "  dwmapi.dll\n",
                                   kHex + "  dwmapi.dll\n" + kHex + "  dwmapi.dll\n", kHex + "  dwmapi.dll.sha256\n", kHex + "  dwmapi.dll x\n",
                                   std::string("<html>404</html>"), kHex + "  dwmapi.dll\n\n x", std::string(2000, 'a')})
        CHECK(!ParseUpdateShaFile(bad, d));
}

static void TestDigests() {
    Sha256Digest file{}, api{}, out{};
    CHECK(Sha256FromHex(kHex, file) && Sha256FromHex(kHex, api));
    UpdateAssets a;
    CHECK(ExpectedDllDigest(file, a, out) && out == file);
    a.haveApiDigest = true; a.apiDigest = api;
    CHECK(ExpectedDllDigest(file, a, out) && out == file);
    api[5] ^= 1; a.apiDigest = api;
    CHECK(!ExpectedDllDigest(file, a, out));
}

static void TestDownload() {
    const std::vector<uint8_t> bytes(1000, 0x5A);
    const Sha256Digest good = Sha256Of(bytes.data(), bytes.size());
    CHECK(CheckDownload(bytes.data(), bytes.size(), 1000, good) == DownloadVerdict::Ok);
    CHECK(CheckDownload(bytes.data(), bytes.size(), 0, good) == DownloadVerdict::Ok);   // size unknown: hash decides
    CHECK(CheckDownload(bytes.data(), bytes.size(), 999, good) == DownloadVerdict::SizeMismatch);
    CHECK(CheckDownload(nullptr, 0, 0, good) == DownloadVerdict::Empty);
    Sha256Digest bad = good; bad[31] ^= 0x80;
    CHECK(CheckDownload(bytes.data(), bytes.size(), 1000, bad) == DownloadVerdict::HashMismatch);
    std::vector<uint8_t> one = bytes; one[500] ^= 1;
    CHECK(CheckDownload(one.data(), one.size(), 1000, good) == DownloadVerdict::HashMismatch);
    CHECK(CheckDownload(bytes.data(), size_t(kMaxUpdateDllBytes) + 1, 0, good) == DownloadVerdict::TooBig);
}

// Runs the plan against a fake disk: `failAt` names the actions that fail.
struct Run { SwapOutcome outcome; std::vector<SwapAction> steps; };
static Run Play(std::vector<SwapAction> failAt) {
    SwapPlan p;
    Run r{SwapOutcome::Running, {}};
    for (int guard = 0; guard < 20 && p.Next() != SwapAction::Done; ++guard) {
        const SwapAction a = p.Next();
        r.steps.push_back(a);
        bool ok = true;
        for (SwapAction f : failAt) if (f == a) ok = false;
        p.Report(ok);
    }
    r.outcome = p.outcome();
    return r;
}

static void TestSwap() {
    using A = SwapAction;
    Run r = Play({});
    CHECK(r.outcome == SwapOutcome::Installed && r.steps == std::vector<A>({A::WriteNew, A::MoveCurrentAside, A::MoveNewIn}));
    r = Play({A::WriteNew});   // nothing was touched: only the .new file is removed
    CHECK(r.outcome == SwapOutcome::FailedUnchanged && r.steps == std::vector<A>({A::WriteNew, A::DeleteNew}));
    r = Play({A::MoveCurrentAside});
    CHECK(r.outcome == SwapOutcome::FailedUnchanged && r.steps == std::vector<A>({A::WriteNew, A::MoveCurrentAside, A::DeleteNew}));
    r = Play({A::MoveNewIn});   // the running dll was moved aside: it must come back
    CHECK(r.outcome == SwapOutcome::RolledBack && r.steps == std::vector<A>({A::WriteNew, A::MoveCurrentAside, A::MoveNewIn, A::RestoreOld, A::DeleteNew}));
    r = Play({A::MoveNewIn, A::RestoreOld});
    CHECK(r.outcome == SwapOutcome::Broken && r.steps == std::vector<A>({A::WriteNew, A::MoveCurrentAside, A::MoveNewIn, A::RestoreOld}));
    // a failing cleanup changes nothing about the outcome
    r = Play({A::WriteNew, A::DeleteNew});
    CHECK(r.outcome == SwapOutcome::FailedUnchanged);
    r = Play({A::MoveNewIn, A::DeleteNew});
    CHECK(r.outcome == SwapOutcome::RolledBack);
    // once finished the plan stays finished
    SwapPlan p;
    while (p.Next() != A::Done) p.Report(true);
    p.Report(false);
    CHECK(p.Next() == A::Done && p.outcome() == SwapOutcome::Installed);
    CHECK(SwapPlan().outcome() == SwapOutcome::Running);
    for (SwapOutcome o : {SwapOutcome::Installed, SwapOutcome::FailedUnchanged, SwapOutcome::RolledBack, SwapOutcome::Broken, SwapOutcome::Running})
        CHECK(SwapOutcomeText(o)[0] != '\0');
}

static void TestRunningRelease() {
    Sha256Digest want{}, out{};
    CHECK(Sha256FromHex(kHex, want));
    const Version running = *ParseVersion("0.8.0-pre25");
    ReleaseList l = ParseReleases("[" + Good("v0.8.0-pre26") + "," + Good("v0.8.0-pre25") + "]");
    CHECK(l.releases.size() == 2 && RunningReleaseDigest(l.releases, running, out) && out == want);
    CHECK(!RunningReleaseDigest(l.releases, *ParseVersion("0.8.0-pre24"), out));         // not on the page
    l = ParseReleases("[" + Good("v0.8.0-pre25", "null") + "]");
    CHECK(!RunningReleaseDigest(l.releases, running, out));                                // no digest
    l = ParseReleases("[" + Good("v0.8.0-pre25", "\"sha1:abc\"") + "]");
    CHECK(!RunningReleaseDigest(l.releases, running, out));                                // not sha256
    l = ParseReleases("[" + ReleaseJson("v0.8.0-pre25", AssetJson("other.zip", Base("v0.8.0-pre25") + "other.zip", "5", "\"sha256:" + kHex + "\"")) + "]");
    CHECK(!RunningReleaseDigest(l.releases, running, out));                                // no dwmapi.dll
    l = ParseReleases("[" + Good("v0.8.0-pre25") + "]");
    l.releases[0].draft = true;
    CHECK(!RunningReleaseDigest(l.releases, running, out));
}

int main() {
    TestScan();
    TestSelect();
    TestUrls();
    TestShaFile();
    TestDigests();
    TestDownload();
    TestSwap();
    TestRunningRelease();
    if (g_fail) { std::printf("test_hsm_update: %d failure(s)\n", g_fail); return 1; }
    std::printf("test_hsm_update: ok\n");
    return 0;
}
