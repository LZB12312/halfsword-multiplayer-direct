// Lobby mod rules (docs/PLAN_MOD_RULES.md): what a machine runs (the mod report), what a lobby accepts (the rule), the
// decision between the two, how the rule travels in Steam lobby data, the HSMMod integrity gates (build hash, official
// build), the countdown after a mid-lobby rule change, and the ModList / SessionMods messages. Pure, no engine or Win32
// types; the scans are gameplay/mod_maps.cpp, ue4ss_load.cpp and enhancer.cpp, the lobby side gameplay/mod_rules.cpp.
//
// Everything a client reports comes from code it controls: these rules keep honest players on matching setups and make
// the drop-in cheats (HS Enhancer, UE4SS cheat mods) unusable in lobbies that block them. Challenge-response
// (session/hsm_mod_challenge.h) and the behaviour detectors are the layers under this one.
#pragma once
#include "hsm_protocol.h"
#include "session/hsm_build.h"
#include "session/hsm_known_mods.h"
#include "session/hsm_modpack.h"
#include "session/hsm_stock_packages.h"
#include "session/hsm_ue4ss.h"
#include "util/hsm_sha256.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hsm {

inline constexpr size_t kMaxModItems = 96;          // items in one report on the wire (ModList; SessionMods' union)
inline constexpr size_t kMaxModItemName = 96;       // a pak file name, a UE4SS mod folder, a proxy DLL, a detector id
inline constexpr size_t kModPinHex = 8;             // hex digits of a SHA-256 an allow list pins (lobby data, PLAN 5)
inline constexpr size_t kMaxModAllow = 64;          // entries of a Custom or Bundle list
inline constexpr size_t kMaxLobbyModList = 4000;    // chars of hsm_mlist (Steam allows 8 KB per value); longer: hash only
inline constexpr uint64_t kModListWaitMs = 15000;   // the host holds a new player this long for its ModList
inline constexpr uint64_t kModRuleGraceMs = 60000;  // a player who no longer fits after a rule change has this long
inline constexpr uint64_t kModCompleteWaitMs = 120000;   // a held player whose own scan (hashing big paks) never finishes

// Lobby data keys (host) and member data keys (each player).
inline constexpr const char* kMemberLiveKey = "hsm_live";          // what a fighter reports running, as an encoded class (match/hsm_online_rules.h LiveSheet)
inline constexpr const char* kLobbyBlockKey = "hsm_mblock";        // how strict the host is (match/hsm_online_rules.h LobbyBlock)
inline constexpr const char* kLobbyModRuleKey = "hsm_mrule";        // preset digit, ModPreset
inline constexpr const char* kLobbyModCheatsKey = "hsm_mcheat";     // "1" cheats blocked (effective: off when the host runs one)
inline constexpr const char* kLobbyModOfficialKey = "hsm_mofficial";// "1" official builds only (effective)
inline constexpr const char* kLobbyModListKey = "hsm_mlist";        // Custom / Bundle list, or "#<16 hex>" when too long
inline constexpr const char* kLobbyHostCheatKey = "hsm_mhcheat";    // the host's own cheat mods, "A, B" (the browser says so)
inline constexpr const char* kMemberOfficialKey = "hsm_official";   // "official" | "unofficial" | "unknown"

// ------------------------------------------------------------------ the report

enum class ModKind : uint8_t { MapPak = 1, ContentPak = 2, Ue4ssMod = 3, Ue4ssStock = 4, Proxy = 5, Module = 6 };
inline constexpr uint8_t kMaxModKind = 6;
inline const char* ModKindName(ModKind k) {
    switch (k) {
        case ModKind::MapPak: return "map pak";
        case ModKind::ContentPak: return "content pak";
        case ModKind::Ue4ssMod: return "UE4SS mod";
        case ModKind::Ue4ssStock: return "UE4SS stock mod";
        case ModKind::Proxy: return "proxy DLL";
        case ModKind::Module: return "module";
    }
    return "?";
}

struct ModItem {
    ModKind kind = ModKind::ContentPak;
    std::string name;
    bool hashed = false;      // sha is valid (a Module or a stock UE4SS mod has none)
    Sha256Digest sha{};
    bool operator==(const ModItem& o) const { return kind == o.kind && name == o.name && hashed == o.hashed && (!hashed || sha == o.sha); }
};

struct ModReport {
    std::vector<ModItem> items;
    bool ue4ss = false;       // UE4SS runs in this process. Not a mod by itself: Vanilla refuses only its console mods (hsm_ue4ss.h)
    bool complete = false;    // every scan has finished and every hash is in
    bool truncated = false;   // more than kMaxModItems: the rest is not in the list
    bool operator==(const ModReport& o) const { return items == o.items && ue4ss == o.ue4ss && complete == o.complete && truncated == o.truncated; }
};

namespace modrules_detail {
inline char Low(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }
inline bool EqNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (Low(a[i]) != Low(b[i])) return false;
    return true;
}
inline bool LessNoCase(std::string_view a, std::string_view b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) if (Low(a[i]) != Low(b[i])) return Low(a[i]) < Low(b[i]);
    return a.size() < b.size();
}
inline bool IsHex(std::string_view s) {
    for (char c : s) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
inline std::string Join(const std::vector<std::string>& v, const char* sep = ", ") {
    std::string s;
    for (const std::string& x : v) s += (s.empty() ? "" : sep) + x;
    return s;
}
}  // namespace modrules_detail

// A name fit for the wire, the lobby list and the log: 1..kMaxModItemName printable ASCII, none of the list separators
// '|' '*', not starting with '+' or '#' (the list's own markers).
inline bool ModItemNameOk(std::string_view n) {
    if (n.empty() || n.size() > kMaxModItemName || n[0] == '+' || n[0] == '#') return false;
    for (char c : n) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u >= 0x7F || c == '|' || c == '*') return false;
    }
    return true;
}

// A name from disk made fit: bad characters become '_', a leading marker too, cut to the cap. Empty stays empty.
inline std::string SanitizeModItemName(std::string_view n) {
    std::string s;
    for (char c : n) {
        const unsigned char u = static_cast<unsigned char>(c);
        s += (u < 0x20 || u >= 0x7F || c == '|' || c == '*') ? '_' : c;
        if (s.size() >= kMaxModItemName) break;
    }
    if (!s.empty() && (s[0] == '+' || s[0] == '#')) s[0] = '_';
    return s;
}

// A report in its canonical order (kind, then name, any case), at most kMaxModItems; extras set `truncated`. Invalid
// names are dropped. Two machines with the same mods produce equal reports, so a re-send only happens on a real change.
inline void NormalizeReport(ModReport& r) {
    std::vector<ModItem> kept;
    for (ModItem& it : r.items) if (ModItemNameOk(it.name)) kept.push_back(std::move(it));
    std::stable_sort(kept.begin(), kept.end(), [](const ModItem& a, const ModItem& b) {
        if (a.kind != b.kind) return uint8_t(a.kind) < uint8_t(b.kind);
        return modrules_detail::LessNoCase(a.name, b.name);
    });
    if (kept.size() > kMaxModItems) { kept.resize(kMaxModItems); r.truncated = true; }
    r.items = std::move(kept);
}

// ------------------------------------------------------------------ the known-mods table

inline KnownKind KnownKindOf(ModKind k) {
    switch (k) {
        case ModKind::MapPak: case ModKind::ContentPak: return KnownKind::Pak;
        case ModKind::Ue4ssMod: case ModKind::Ue4ssStock: return KnownKind::Ue4ss;
        case ModKind::Proxy: return KnownKind::Proxy;
        case ModKind::Module: return KnownKind::Module;
    }
    return KnownKind::Pak;
}

// What the table says about one item: a known build's hash first (a renamed copy is still that mod), then the name with
// the same kind. Unknown when neither matches.
inline KnownVerdict VerdictOf(const ModItem& it) {
    if (it.hashed) {
        const std::string hex = Sha256Hex(it.sha);
        for (const KnownMod& m : kKnownMods)
            for (size_t i = 0; i < m.shaCount; ++i) if (hex == m.sha256[i]) return m.verdict;
    }
    const KnownKind kk = KnownKindOf(it.kind);
    for (const KnownMod& m : kKnownMods) if (m.kind == kk && modrules_detail::EqNoCase(m.name, it.name)) return m.verdict;
    return KnownVerdict::Unknown;
}

// A UE4SS mod folder that UE4SS ships enabled (the table's stock entries).
inline bool IsStockUe4ssMod(std::string_view name) {
    for (const KnownMod& m : kKnownMods)
        if (m.kind == KnownKind::Ue4ss && m.verdict == KnownVerdict::Stock && modrules_detail::EqNoCase(m.name, name)) return true;
    return false;
}

inline std::vector<std::string> NamesWith(const ModReport& r, KnownVerdict v) {
    std::vector<std::string> out;
    for (const ModItem& it : r.items) if (VerdictOf(it) == v) out.push_back(it.name);
    return out;
}
inline bool HasCheat(const ModReport& r) { return !NamesWith(r, KnownVerdict::Cheat).empty(); }

// The UE4SS mods a Classes lobby takes: the known cheat HS Enhancer among them, because its settings are checked instead.
inline bool IsSupportedUe4ssMod(std::string_view name) {
    for (const char* m : {"HSEnhancer", "HalfWolverine", "ModdedMapLoader", "WillieFace", "ZylarMod", "MeleeKillCam", "HalfSwordAdaptiveRival", "SteadyAI", "HSHelper", "ImmersiveCombat"})
        if (modrules_detail::EqNoCase(m, name)) return true;
    return IsStockUe4ssMod(name);
}

// ------------------------------------------------------------------ map pak or content pak

inline bool UnderFolder(std::string_view dir, std::string_view folder) {
    if (folder.empty() || dir.size() < folder.size()) return false;
    if (!modrules_detail::EqNoCase(dir.substr(0, folder.size()), folder)) return false;
    return dir.size() == folder.size() || dir[folder.size()] == '/';
}

// Folders a map pak may not touch. UE4SS's BPModLoaderMod, allowed in every lobby, runs what sits in /Game/Mods: weapon mods
// live there. /Engine is the engine's.
inline bool MapPakKeepsOut(std::string_view dir) { return UnderFolder(dir, "/Engine") || UnderFolder(dir, "/Game/Mods"); }

// A pak is a map pak when its index lists at least one map and it can only add to the game: it replaces no stock package and
// has nothing a loader runs. Where its other assets sit doesn't matter. A file in a new place is only reached by the pak's own
// maps, and real maps spread props, sounds and starter content over many folders.
// `maps`: the pak's maps. `badMaps`: more maps whose names can't be package paths; they count, but can't go on the map list.
// `assetDirs`: the package folders of its other files. `stockFiles`: those files that replace a stock package. `overflow`: the
// index had more folders than were kept, or a file that could change the game wherever it sits (.ini). All from
// session/hsm_pak_index.h. Anything else is a content pak.
// `why`: what a map pak's owner should know, or what kept it from being one, for the log. Empty for nothing to explain.
struct PakClass {
    ModKind kind = ModKind::ContentPak;
    std::string why;
};
inline PakClass ClassifyPakWhy(const std::vector<std::string>& maps, const std::vector<std::string>& assetDirs, bool overflow,
                               std::string_view overflowWhy = {}, size_t badMaps = 0, const std::vector<std::string>& stockFiles = {}) {
    constexpr size_t kNamed = 3;   // folders named in `why`
    PakClass c;
    if (maps.empty() && !badMaps) return c;
    if (overflow) { c.why = overflowWhy.empty() ? std::string("its index was not fully read") : std::string(overflowWhy); return c; }
    for (const std::string& m : maps) if (IsStockPackage(m)) { c.why = "replaces stock " + m; return c; }
    if (!stockFiles.empty()) {
        c.why = "replaces stock files:";
        for (const std::string& f : stockFiles) c.why += " " + f;
        return c;
    }
    size_t kept = 0;
    std::string named;
    for (const std::string& d : assetDirs) {
        if (!MapPakKeepsOut(d)) continue;
        if (++kept <= kNamed) named += (kept > 1 ? ", " : "") + d;
    }
    if (kept) {
        c.why = std::to_string(kept) + " asset folder(s) in /Game/Mods or /Engine: " + named + (kept > kNamed ? ", ..." : "");
        return c;
    }
    c.kind = ModKind::MapPak;
    if (badMaps) c.why = "a map's file name cannot be a package path";
    return c;
}
inline ModKind ClassifyPak(const std::vector<std::string>& maps, const std::vector<std::string>& assetDirs, bool overflow) {
    return ClassifyPakWhy(maps, assetDirs, overflow).kind;
}

// ------------------------------------------------------------------ the rule

// Classes: HS Enhancer and Half Wolverine are allowed with balanced settings (match/hsm_online_rules.h checks the values).
enum class ModPreset : uint8_t { Vanilla = 0, MapsOnly = 1, Bundle = 2, Custom = 3, Anything = 4, Classes = 5, Unrestricted = 6 };
inline constexpr uint8_t kMaxModPreset = 6;
// Both take classes and judge mods the same way. Unrestricted only lifts the class power budget (match/hsm_class.h).
inline constexpr bool IsClassesPreset(ModPreset p) { return p == ModPreset::Classes || p == ModPreset::Unrestricted; }
inline const char* ModPresetName(ModPreset p) {
    switch (p) {
        case ModPreset::Vanilla: return "Vanilla";
        case ModPreset::MapsOnly: return "Maps only";
        case ModPreset::Bundle: return "Bundle";
        case ModPreset::Custom: return "Custom";
        case ModPreset::Anything: return "Anything";
        case ModPreset::Classes: return "Classes";
        case ModPreset::Unrestricted: return "Unrestricted Classes";
    }
    return "?";
}
// The INI / log spelling: vanilla, maps, bundle, custom, anything.
inline const char* ModPresetKey(ModPreset p) {
    switch (p) {
        case ModPreset::Vanilla: return "vanilla";
        case ModPreset::MapsOnly: return "maps";
        case ModPreset::Bundle: return "bundle";
        case ModPreset::Custom: return "custom";
        case ModPreset::Anything: return "anything";
        case ModPreset::Classes: return "classes";
        case ModPreset::Unrestricted: return "unrestricted";
    }
    return "maps";
}
inline bool ParseModPreset(std::string_view s, ModPreset& out) {
    for (uint8_t i = 0; i <= kMaxModPreset; ++i)
        if (modrules_detail::EqNoCase(s, ModPresetKey(ModPreset(i)))) { out = ModPreset(i); return true; }
    return false;
}

struct ModAllow {
    std::string name;        // matched any case against every kind
    std::string pin;         // lowercase hex prefix of the SHA-256 (kModPinHex), empty: any build
    bool required = false;   // the player must have it (Bundle: every pak of the pack)
    bool operator==(const ModAllow& o) const { return name == o.name && pin == o.pin && required == o.required; }
};

struct ModRule {
    ModPreset preset = ModPreset::MapsOnly;
    bool blockCheats = true;
    bool officialOnly = true;
    std::vector<ModAllow> list;   // Custom and Bundle
    bool listHashOnly = false;    // read from lobby data whose list was too long: only the host can check it
    bool operator==(const ModRule& o) const { return preset == o.preset && blockCheats == o.blockCheats && officialOnly == o.officialOnly && list == o.list && listHashOnly == o.listHashOnly; }
};

// What a host chose in HSMMod.ini [mod] (mod_rule, mod_cheats, mod_official), before its pack, its own report and build
// shape it (HostRule, EffectiveRule): the lobby's MODS tab steps it. The defaults are the INI's.
struct ModChoice {
    ModPreset preset = ModPreset::MapsOnly;
    bool blockCheats = true;
    bool officialOnly = false;
    bool operator==(const ModChoice& o) const { return preset == o.preset && blockCheats == o.blockCheats && officialOnly == o.officialOnly; }
};
inline ModChoice ChoiceOf(const ModRule& r) { return ModChoice{r.preset, r.blockCheats, r.officialOnly}; }

// What a lobby whose host has no rule (mod_rules=0, or a build before rules) means to a client that has them.
inline ModRule NoRule() { ModRule r; r.preset = ModPreset::Anything; r.blockCheats = true; r.officialOnly = false; return r; }

// A cheat under this rule: Classes takes HS Enhancer, and only HS Enhancer.
inline bool IsCheatUnder(ModPreset preset, const ModItem& it) {
    if (VerdictOf(it) != KnownVerdict::Cheat) return false;
    return !(IsClassesPreset(preset) && modrules_detail::EqNoCase(it.name, "HSEnhancer"));
}

// The rule a host publishes and enforces: what it chose, with cheats allowed when the host runs a cheat itself (it
// cannot block what it runs; the browser says so) and official-only off when the host is not on an official build.
enum class Official : uint8_t { Unknown = 0, Yes = 1, No = 2 };
// Which releases the update notice follows, so a player hears only of public releases and a tester of test builds. Public: the
// build is its public release's own file, or its version isn't in the public list but is older than the newest there (an old
// public build the short list no longer reaches). Dev: a dev-only release newer than every public one, or a file that differs
// from its public release (a local build).
enum class UpdateChannel : uint8_t { Public, Dev };
inline UpdateChannel UpdateChannelOf(Official o, bool newerThanEveryPublic) {
    if (o == Official::Yes) return UpdateChannel::Public;
    if (o == Official::No) return UpdateChannel::Dev;
    return newerThanEveryPublic ? UpdateChannel::Dev : UpdateChannel::Public;
}
inline ModRule EffectiveRule(const ModRule& chosen, const ModReport& hostReport, Official hostOfficial) {
    ModRule r = chosen;
    for (const ModItem& it : hostReport.items) if (IsCheatUnder(chosen.preset, it)) r.blockCheats = false;
    if (hostOfficial == Official::No) r.officialOnly = false;
    return r;
}

// Bundle: every pak of the host's pack, required, pinned to its hash.
inline std::vector<ModAllow> BundleList(const PackManifest& m) {
    std::vector<ModAllow> out;
    for (const PackPak& p : m.paks) {
        if (out.size() >= kMaxModAllow) break;
        ModAllow a; a.name = p.file; a.pin = Sha256Hex(p.sha).substr(0, kModPinHex); a.required = true;
        out.push_back(a);
    }
    return out;
}

// Custom with no list configured: exactly what the host runs, each pinned to its build, none required.
inline std::vector<ModAllow> ListFromReport(const ModReport& r) {
    std::vector<ModAllow> out;
    for (const ModItem& it : r.items) {
        if (out.size() >= kMaxModAllow) break;
        if (it.kind == ModKind::Ue4ssStock || it.kind == ModKind::Module || it.kind == ModKind::Proxy) continue;
        ModAllow a; a.name = it.name; a.pin = it.hashed ? Sha256Hex(it.sha).substr(0, kModPinHex) : std::string(); out.push_back(a);
    }
    return out;
}

// "Name1, Name2*1a2b3c4d, +Required" ([mod] mod_allow=): a list from the INI. A bad entry is skipped.
inline std::vector<ModAllow> ParseAllowList(std::string_view text) {
    std::vector<ModAllow> out;
    size_t i = 0;
    while (i <= text.size() && out.size() < kMaxModAllow) {
        size_t j = text.find_first_of(",|", i);
        if (j == std::string_view::npos) j = text.size();
        std::string_view e = text.substr(i, j - i);
        while (!e.empty() && (e.front() == ' ' || e.front() == '\t')) e.remove_prefix(1);
        while (!e.empty() && (e.back() == ' ' || e.back() == '\t' || e.back() == '\r')) e.remove_suffix(1);
        ModAllow a;
        if (!e.empty() && e[0] == '+') { a.required = true; e.remove_prefix(1); }
        const size_t star = e.find('*');
        if (star != std::string_view::npos) {
            std::string pin(e.substr(star + 1));
            for (char& c : pin) c = modrules_detail::Low(c);
            e = e.substr(0, star);
            if (pin.size() == kModPinHex && modrules_detail::IsHex(pin)) a.pin = pin; else e = {};
        }
        a.name = std::string(e);
        if (ModItemNameOk(a.name)) out.push_back(a);
        i = j + 1;
    }
    return out;
}

inline bool AllowMatches(const ModAllow& a, const ModItem& it) {
    if (!modrules_detail::EqNoCase(a.name, it.name)) return false;
    if (a.pin.empty()) return true;
    return it.hashed && Sha256Hex(it.sha).compare(0, a.pin.size(), a.pin) == 0;
}

// ------------------------------------------------------------------ the decision

enum class RuleFail : uint8_t {
    None,
    Pending,      // the report is not complete yet: ask again
    Cheat,        // `mod` is a known cheat and the lobby blocks cheats
    NoMods,       // Vanilla: `mod` must go
    NotAllowed,   // `mod` is not allowed by this preset or list
    Missing,      // `mod` is required and not here (or another build)
    TooMany,      // the report was cut at kMaxModItems; only Anything accepts that
    Unchecked,    // the browser cannot see the list (hash only); the host decides after the join
};
struct RuleResult {
    RuleFail fail = RuleFail::None;
    std::string mod;
    size_t more = 0;   // other items the preset refuses too (NoMods, NotAllowed)
    bool ok() const { return fail == RuleFail::None || fail == RuleFail::Unchecked; }
};

// One item against the preset and its list alone (no cheat check, no report-wide check): NoMods (Vanilla) or
// NotAllowed when the preset refuses it, else None.
inline RuleFail ItemPresetFail(const ModRule& rule, const ModItem& it) {
    if (rule.preset == ModPreset::Anything) return RuleFail::None;
    if (rule.preset == ModPreset::Vanilla) {
        if (it.kind == ModKind::Ue4ssStock) return ue4ss::IsConsoleMod(it.name) ? RuleFail::NoMods : RuleFail::None;
        return it.kind != ModKind::Proxy ? RuleFail::NoMods : RuleFail::None;
    }
    const KnownVerdict v = VerdictOf(it);
    bool allowed = it.kind == ModKind::Ue4ssStock || it.kind == ModKind::Proxy || v == KnownVerdict::Cosmetic || v == KnownVerdict::Stock;
    if (rule.preset == ModPreset::MapsOnly) allowed = allowed || it.kind == ModKind::MapPak;
    else if (IsClassesPreset(rule.preset))
        allowed = allowed || it.kind == ModKind::MapPak || it.kind == ModKind::ContentPak || (it.kind == ModKind::Ue4ssMod && IsSupportedUe4ssMod(it.name)) ||
                  (it.kind == ModKind::Module && modrules_detail::EqNoCase(it.name, "HSEnhancer"));
    else for (const ModAllow& a : rule.list) if (AllowMatches(a, it)) { allowed = true; break; }
    return allowed ? RuleFail::None : RuleFail::NotAllowed;
}

// Why the rule refuses this one item, as CheckMods would (the lobby's MODS tab marks each row): Cheat, NoMods or
// NotAllowed; Unchecked when the list is known by hash only; None when the item is fine.
inline RuleFail ItemRefused(const ModRule& rule, const ModItem& it) {
    if (rule.blockCheats && VerdictOf(it) == KnownVerdict::Cheat) return RuleFail::Cheat;
    if (rule.preset == ModPreset::Anything) return RuleFail::None;
    if (rule.listHashOnly && (rule.preset == ModPreset::Custom || rule.preset == ModPreset::Bundle)) return RuleFail::Unchecked;
    return ItemPresetFail(rule, it);
}

// Is the report allowed by the rule? The first problem, in this order: a cheat, then each item the preset refuses (in
// report order), then a missing required mod. A proxy DLL next to the game (ReShade's dxgi.dll, an ASI loader's
// version.dll, an old UE4SS's xinput1_3.dll) is reported but no preset refuses it: its name cannot tell an overlay from
// a mod, and a known cheat among them is still caught above.
inline RuleResult CheckMods(const ModRule& rule, const ModReport& rep) {
    RuleResult r;
    if (!rep.complete) { r.fail = RuleFail::Pending; return r; }
    if (rule.blockCheats)
        for (const ModItem& it : rep.items)
            if (IsCheatUnder(rule.preset, it)) { r.fail = RuleFail::Cheat; r.mod = it.name; return r; }
    if (rule.preset == ModPreset::Anything) return r;
    if (rep.truncated) { r.fail = RuleFail::TooMany; return r; }
    if (rule.listHashOnly && (rule.preset == ModPreset::Custom || rule.preset == ModPreset::Bundle)) { r.fail = RuleFail::Unchecked; return r; }
    for (const ModItem& it : rep.items) {
        const RuleFail f = ItemPresetFail(rule, it);
        if (f == RuleFail::None) continue;
        if (r.fail == RuleFail::None) { r.fail = f; r.mod = it.name; } else ++r.more;
    }
    if (r.fail != RuleFail::None) return r;
    if (rule.preset == ModPreset::Bundle || rule.preset == ModPreset::Custom)
        for (const ModAllow& a : rule.list) {
            if (!a.required) continue;
            bool have = false;
            for (const ModItem& it : rep.items) if (AllowMatches(a, it)) { have = true; break; }
            if (!have) { r.fail = RuleFail::Missing; r.mod = a.name; return r; }
        }
    return r;
}

// What the player is told: a sentence that says what to remove or install. Empty for an accepted report.
inline std::string RuleFailText(const ModRule& rule, const RuleResult& r) {
    const std::string preset = ModPresetName(rule.preset);
    const std::string others = r.more ? " and " + std::to_string(r.more) + " more" : "";
    switch (r.fail) {
        case RuleFail::None: return {};
        case RuleFail::Pending: return "Still checking your mods. Try again in a moment.";
        case RuleFail::Cheat: return "This lobby blocks cheats: remove " + r.mod + ".";
        case RuleFail::NoMods: return "This lobby is Vanilla: remove " + r.mod + others + ".";
        case RuleFail::NotAllowed: return "This lobby (" + preset + ") does not allow " + r.mod + others + (r.more ? ": remove them." : ": remove it.");
        case RuleFail::Missing: return "This lobby needs " + r.mod + ": install the host's build of it.";
        case RuleFail::TooMany: return "You run more than 96 mods; this lobby (" + preset + ") cannot check them all.";
        case RuleFail::Unchecked: return "This lobby has a long mod list: the host checks it when you join.";
    }
    return {};
}

// The browser row's short note (joining shows RuleFailText).
inline std::string RuleFailShort(const RuleResult& r) {
    switch (r.fail) {
        case RuleFail::None: return {};
        case RuleFail::Pending: return "Checking your mods";
        case RuleFail::Cheat: return "Blocks " + r.mod;
        case RuleFail::NoMods: return "Vanilla only";
        case RuleFail::NotAllowed:   // "No Abyss.pak" read as a file the player lacked
            return "Remove " + r.mod + (r.more ? " +" + std::to_string(r.more) : "");
        case RuleFail::Missing: return "Needs " + r.mod;
        case RuleFail::TooMany: return "Too many mods";
        case RuleFail::Unchecked: return "Custom mod rules";
    }
    return {};
}

// "Maps only, cheats blocked, official builds only": the system chat line after a change and the browser tooltip.
inline std::string RuleSummary(const ModRule& r) {
    std::string s = ModPresetName(r.preset);
    s += r.blockCheats ? ", cheats blocked" : ", cheats allowed";
    if (r.officialOnly) s += ", official builds only";
    return s;
}

// The host's own cheat mods as the lobby data names them ("A, B", kLobbyHostCheatKey), made fit for the browser: the
// value is peer-written, so only names ModItemNameOk passes are kept, at most kMaxHostCheatNames, and "+N" for the rest.
inline constexpr size_t kMaxHostCheatNames = 3;
inline std::string HostCheatNames(std::string_view raw) {
    std::vector<std::string> keep;
    size_t more = 0, i = 0;
    while (i < raw.size()) {
        size_t j = raw.find(',', i);
        if (j == std::string_view::npos) j = raw.size();
        std::string_view e = raw.substr(i, j - i);
        while (!e.empty() && e.front() == ' ') e.remove_prefix(1);
        while (!e.empty() && e.back() == ' ') e.remove_suffix(1);
        if (ModItemNameOk(e)) { if (keep.size() < kMaxHostCheatNames) keep.emplace_back(e); else ++more; }
        i = j + 1;
    }
    std::string s = modrules_detail::Join(keep);
    if (more) s += " +" + std::to_string(more);
    return s;
}

// The browser row's tag for a lobby's rule: empty for the default (Maps only, cheats blocked, no cheat on the host),
// else "mods: Vanilla", "mods: Anything, cheats ok", ", official only", ", host runs HSEnhancer" (`hostCheats` is the
// lobby data's kLobbyHostCheatKey, raw).
inline std::string LobbyRuleTag(const ModRule& r, std::string_view hostCheats = {}) {
    const std::string cheats = HostCheatNames(hostCheats);
    if (r.preset == ModPreset::MapsOnly && r.blockCheats && !r.officialOnly && cheats.empty()) return {};
    std::string s = std::string("mods: ") + ModPresetName(r.preset);
    if (!r.blockCheats) s += ", cheats ok";
    if (r.officialOnly) s += ", official only";
    if (!cheats.empty()) s += ", host runs " + cheats;
    return s;
}

// ------------------------------------------------------------------ lobby data

// hsm_mlist: "+Name*1a2b3c4d|Other|..." ('+' required, '*' the pin). Longer than kMaxLobbyModList: "#<16 hex>" of the
// whole list, so a client can only say "custom mod rules" and the host checks after the join.
inline std::string EncodeLobbyModList(const std::vector<ModAllow>& list) {
    std::string s;
    for (const ModAllow& a : list) {
        if (!ModItemNameOk(a.name)) continue;
        if (!s.empty()) s += '|';
        if (a.required) s += '+';
        s += a.name;
        if (!a.pin.empty()) { s += '*'; s += a.pin; }
    }
    if (s.size() <= kMaxLobbyModList) return s;
    const Sha256Digest d = Sha256Of(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    return "#" + Sha256Hex(d).substr(0, 16);
}

struct LobbyModData { std::string rule, cheats, official, list; };

inline LobbyModData EncodeLobbyRule(const ModRule& r) {
    LobbyModData d;
    d.rule = std::to_string(unsigned(r.preset));
    d.cheats = r.blockCheats ? "1" : "0";
    d.official = r.officialOnly ? "1" : "0";
    if (r.preset == ModPreset::Custom || r.preset == ModPreset::Bundle) d.list = EncodeLobbyModList(r.list);
    return d;
}

// Peer-written lobby data back to a rule. A lobby without hsm_mrule has no rule (NoRule); a bad value is read as the
// strictest reading of that field (cheats blocked), never as a looser one.
inline ModRule DecodeLobbyRule(const LobbyModData& d) {
    if (d.rule.empty()) return NoRule();
    ModRule r;
    if (d.rule.size() != 1 || d.rule[0] < '0' || d.rule[0] > char('0' + kMaxModPreset)) r.preset = ModPreset::Vanilla;
    else r.preset = ModPreset(d.rule[0] - '0');
    r.blockCheats = d.cheats != "0";
    r.officialOnly = d.official == "1";
    if (r.preset == ModPreset::Custom || r.preset == ModPreset::Bundle) {
        if (!d.list.empty() && d.list[0] == '#') r.listHashOnly = true;
        else r.list = ParseAllowList(d.list);
    }
    return r;
}

// ------------------------------------------------------------------ HSMMod integrity (PLAN 4a, 4b)

// The kBuildHashHex hex digits after " #" at the end of a build string (session/hsm_build.h), empty when it has none.
inline std::string BuildHashOf(std::string_view build) {
    const size_t k = build.rfind(" #");
    if (k == std::string_view::npos || build.size() - k - 2 != kBuildHashHex) return {};
    const std::string_view h = build.substr(k + 2);
    return modrules_detail::IsHex(h) ? std::string(h) : std::string();
}

enum class BuildGate : uint8_t { Ok, NoHash, Different };
// The join gate: a host that knows its own hash refuses a joiner without one or with another. A host that could not hash
// itself cannot check (Ok). allow_build_mismatch=1 lets test builds in.
inline BuildGate CheckJoinBuild(std::string_view hostBuild, std::string_view joinerBuild, bool allowMismatch) {
    const std::string host = BuildHashOf(hostBuild);
    if (allowMismatch || host.empty()) return BuildGate::Ok;
    const std::string joiner = BuildHashOf(joinerBuild);
    if (joiner.empty()) return BuildGate::NoHash;
    return joiner == host ? BuildGate::Ok : BuildGate::Different;
}
inline std::string BuildGateText(BuildGate g, std::string_view hostBuild, std::string_view joinerBuild) {
    switch (g) {
        case BuildGate::Ok: return {};
        case BuildGate::NoHash: return "Your HSMMod could not identify its build. Install the host's release (" + std::string(hostBuild) + ").";
        case BuildGate::Different: return "Different HSMMod build: host #" + BuildHashOf(hostBuild) + ", you #" + BuildHashOf(joinerBuild) + ". Install the same release as the host.";
    }
    return {};
}

// Official: this DLL's SHA-256 equals its release's dwmapi.dll.sha256. `releaseKnown`: the update check fetched it for
// this version (offline, update_check=0 or a version with no release: Unknown).
inline Official OfficialOf(bool releaseKnown, const Sha256Digest& own, const Sha256Digest& release) {
    if (!releaseKnown || Sha256IsZero(own) || Sha256IsZero(release)) return Official::Unknown;
    return own == release ? Official::Yes : Official::No;
}
inline const char* OfficialName(Official o) { return o == Official::Yes ? "official" : o == Official::No ? "unofficial" : "unknown"; }
inline Official ParseOfficial(std::string_view s) { return s == "official" ? Official::Yes : s == "unofficial" ? Official::No : Official::Unknown; }

// Official builds only: an official joiner passes; an unknown one passes on the host's exact build; an unofficial one never.
inline bool OfficialMayJoin(bool officialOnly, Official joiner, bool sameBuildAsHost) {
    if (!officialOnly) return true;
    if (joiner == Official::Yes) return true;
    return joiner == Official::Unknown && sameBuildAsHost;
}
inline constexpr const char* kUnofficialText = "This lobby takes official HSMMod builds only: install the release from the HSMMod page.";

// What the browser says about one lobby for this machine (gameplay/lobby.cpp Describe): `reason` (empty: joinable as far
// as mods go) for the row's note, `text` the full sentence (the join status line), `tag` for the row's detail.
struct LobbyModView { std::string reason, text, tag; };
inline LobbyModView ViewLobbyRule(const ModRule& rule, const ModReport& mine, Official myOfficial, std::string_view hostCheats = {}) {
    LobbyModView v;
    v.tag = LobbyRuleTag(rule, hostCheats);
    const RuleResult r = CheckMods(rule, mine);
    if (r.fail == RuleFail::Pending) return v;   // this machine's scan is still running: the host decides after the join
    if (!r.ok()) { v.reason = RuleFailShort(r); v.text = RuleFailText(rule, r); return v; }
    if (r.fail == RuleFail::Unchecked) v.text = RuleFailText(rule, r);
    // An unofficial build is turned away; an unknown one may still be the host's own build (OfficialMayJoin).
    if (rule.officialOnly && myOfficial == Official::No) { v.reason = "Official builds only"; v.text = kUnofficialText; }
    return v;
}

// ------------------------------------------------------------------ after a rule change (PLAN 6)

// Per player on the host: a player who no longer fits gets kModRuleGraceMs to fix it, then is removed (not banned).
struct ModGrace {
    bool active = false;
    uint64_t deadlineMs = 0;
};
enum class GraceStep : uint8_t { None, Started, Cleared, Remove };
inline GraceStep StepGrace(ModGrace& g, bool fits, uint64_t nowMs) {
    if (fits) {
        if (!g.active) return GraceStep::None;
        g = ModGrace{};
        return GraceStep::Cleared;
    }
    if (!g.active) { g.active = true; g.deadlineMs = nowMs + kModRuleGraceMs; return GraceStep::Started; }
    return nowMs >= g.deadlineMs ? GraceStep::Remove : GraceStep::None;
}
inline uint64_t GraceSecondsLeft(const ModGrace& g, uint64_t nowMs) {
    return !g.active || nowMs >= g.deadlineMs ? 0 : (g.deadlineMs - nowMs + 999) / 1000;
}

// ------------------------------------------------------------------ the host's view of one player

// What the host ignores from a held player (its ModList has not passed): everything that acts on the session (fighter
// streams, claims, props, hand-offs, a give-up, a loadout change, a progression vote). Joining (Ready, BlobChunk, pack
// status, ping tables), chat and the mod messages still count; its chat commands are refused in session.cpp.
inline bool HeldPlayerIgnores(MsgType t) {
    switch (t) {
        case MsgType::Input: case MsgType::OwnerState: case MsgType::DeathReport: case MsgType::Pose: case MsgType::Vitals:
        case MsgType::HitClaim: case MsgType::SeverClaim: case MsgType::GrabState: case MsgType::StuckBlade:
        case MsgType::PropLease: case MsgType::PaintFx: case MsgType::Equip: case MsgType::SeverNotice: case MsgType::GiveUp:
        case MsgType::ObjectAnnounce: case MsgType::BodyState: case MsgType::ObjectEvent: case MsgType::RemoteWrite:
        case MsgType::HandOff: case MsgType::HandOffAsk: case MsgType::LoadoutChange: case MsgType::ProgressionVote:
            return true;
        default:
            return false;
    }
}

// From the Hello on. `held`: joined but not admitted yet, so no fighter is spawned for it; the host waits for a complete
// ModList that passes the rule. Admitted players are checked again on every new ModList and every rule change, with the
// grace countdown before removal.
struct PlayerMods {
    bool held = true;
    uint64_t heldSinceMs = 0;
    bool haveList = false;
    ModReport report;
    Official official = Official::Unknown;
    ModGrace grace;
};
enum class PlayerModStep : uint8_t {
    Wait,     // nothing to do (held: still waiting; admitted: still fits, or its rescan is running)
    Admit,    // held -> admitted: spawn its fighter
    Warn,     // admitted, no longer fits: tell it `detail`, the countdown started
    Clear,    // admitted, fits again: the countdown stopped
    Remove,   // reject or drop it with `reason` and `detail`
};
struct PlayerModVerdict {
    PlayerModStep step = PlayerModStep::Wait;
    RejectReason reason = RejectReason::ModsNotAllowed;
    std::string detail;   // at most kMaxRejectDetail bytes
};
inline constexpr const char* kNoModListText = "Your HSMMod did not send its mod list. Update HSMMod and join again.";
inline constexpr const char* kModScanSlowText = "Your mod scan did not finish in time. Wait in the menu a moment and join again.";

inline std::string ClipDetail(std::string s) { if (s.size() > kMaxRejectDetail) s.resize(kMaxRejectDetail); return s; }

// One step of the host's check for one player. sameBuildAsHost: the player's HSMMod build hash equals the host's.
inline PlayerModVerdict StepPlayerMods(PlayerMods& p, const ModRule& rule, bool sameBuildAsHost, uint64_t nowMs) {
    PlayerModVerdict v;
    const auto remove = [&](RejectReason why, std::string text) { v.step = PlayerModStep::Remove; v.reason = why; v.detail = ClipDetail(std::move(text)); return v; };
    if (p.held) {
        const uint64_t waited = nowMs - p.heldSinceMs;
        if (!p.haveList) return waited >= kModListWaitMs ? remove(RejectReason::ModsNotAllowed, kNoModListText) : v;
        const RuleResult r = CheckMods(rule, p.report);
        if (r.fail == RuleFail::Pending) return waited >= kModCompleteWaitMs ? remove(RejectReason::ModsNotAllowed, kModScanSlowText) : v;
        if (!r.ok()) return remove(RejectReason::ModsNotAllowed, RuleFailText(rule, r));
        if (!OfficialMayJoin(rule.officialOnly, p.official, sameBuildAsHost)) return remove(RejectReason::ModifiedClient, kUnofficialText);
        p.held = false;
        v.step = PlayerModStep::Admit;
        return v;
    }
    const RuleResult r = CheckMods(rule, p.report);
    if (r.fail == RuleFail::Pending) return v;   // its rescan is running: the complete list decides
    const bool official = OfficialMayJoin(rule.officialOnly, p.official, sameBuildAsHost);
    const bool fits = r.ok() && official;
    switch (StepGrace(p.grace, fits, nowMs)) {
        case GraceStep::None: return v;
        case GraceStep::Cleared: v.step = PlayerModStep::Clear; return v;
        case GraceStep::Started:
            v.step = PlayerModStep::Warn;
            v.reason = r.ok() ? RejectReason::ModifiedClient : RejectReason::ModsNotAllowed;
            v.detail = ClipDetail(r.ok() ? kUnofficialText : RuleFailText(rule, r));
            return v;
        case GraceStep::Remove: return remove(r.ok() ? RejectReason::ModifiedClient : RejectReason::ModsNotAllowed, r.ok() ? kUnofficialText : RuleFailText(rule, r));
    }
    return v;
}

// ------------------------------------------------------------------ logs

// "2 map paks, 1 content paks, ue4ss 7 (6 stock), proxies [winmm.dll], cheats [HSEnhancer], suspects []"
inline std::string ReportLogLine(const ModReport& r) {
    size_t maps = 0, content = 0, ue = 0, stock = 0;
    std::vector<std::string> proxies;
    for (const ModItem& it : r.items) {
        if (it.kind == ModKind::MapPak) ++maps;
        else if (it.kind == ModKind::ContentPak) ++content;
        else if (it.kind == ModKind::Ue4ssMod) ++ue;
        else if (it.kind == ModKind::Ue4ssStock) { ++ue; ++stock; }
        else if (it.kind == ModKind::Proxy) proxies.push_back(it.name);
    }
    using modrules_detail::Join;
    return std::to_string(maps) + " map paks, " + std::to_string(content) + " content paks, ue4ss " +
           (r.ue4ss ? std::to_string(ue) + " (" + std::to_string(stock) + " stock)" : std::string("off")) +
           ", proxies [" + Join(proxies) + "], cheats [" + Join(NamesWith(r, KnownVerdict::Cheat)) + "], suspects [" +
           Join(NamesWith(r, KnownVerdict::Suspect)) + "]" + (r.truncated ? " (list cut)" : "") + (r.complete ? "" : " (scan running)");
}

// ------------------------------------------------------------------ wire: ModList (48), SessionMods (49)

// ModList, client -> host after Welcome and on every change: [official u8][flags u8: 1 ue4ss, 2 complete, 4 truncated]
// [count u8] then per item [kind u8][hashed u8][name str][sha 32 when hashed].
struct ModListMsg {
    Official official = Official::Unknown;
    ModReport report;
};

namespace modrules_detail {
inline void PutItem(Writer& w, const ModItem& it) {
    w.u8(uint8_t(it.kind)); w.u8(it.hashed ? 1 : 0); w.str(it.name, kMaxModItemName);
    if (it.hashed) w.buf.insert(w.buf.end(), it.sha.begin(), it.sha.end());
}
inline bool GetItem(Reader& r, ModItem& it) {
    const uint8_t k = r.u8(), h = r.u8();
    it.name = r.str(kMaxModItemName);
    if (!r.ok() || k < 1 || k > kMaxModKind || h > 1 || !ModItemNameOk(it.name)) return false;
    it.kind = ModKind(k); it.hashed = h == 1;
    if (it.hashed) for (uint8_t& b : it.sha) b = r.u8();
    return r.ok();
}
}  // namespace modrules_detail

inline std::vector<uint8_t> Encode(const ModListMsg& m) {
    Writer w; w.header(MsgType::ModList);
    w.u8(uint8_t(m.official));
    w.u8(uint8_t((m.report.ue4ss ? 1 : 0) | (m.report.complete ? 2 : 0) | (m.report.truncated ? 4 : 0)));
    const size_t n = std::min(m.report.items.size(), kMaxModItems);
    w.u8(uint8_t(n));
    for (size_t i = 0; i < n; ++i) modrules_detail::PutItem(w, m.report.items[i]);
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, ModListMsg& m) {
    if (n > kMaxPacket) return false;
    Reader r(d, n); MsgType t;
    if (!r.header(t) || t != MsgType::ModList) return false;
    const uint8_t off = r.u8(), flags = r.u8(), count = r.u8();
    if (!r.ok() || off > 2 || flags > 7 || count > kMaxModItems) return false;
    m = ModListMsg{};
    m.official = Official(off);
    m.report.ue4ss = flags & 1; m.report.complete = flags & 2; m.report.truncated = flags & 4;
    m.report.items.resize(count);
    for (ModItem& it : m.report.items) if (!modrules_detail::GetItem(r, it)) return false;
    return r.done();
}

// SessionMods, host -> everyone: the union of every player's report, each item with the slots that run it (bit = slot),
// for the multiplayer page's MODS block. [flags u8: 1 truncated][count u8] then per item [item][mask u16].
struct SessionModEntry {
    ModItem item;
    uint16_t slots = 0;
    bool operator==(const SessionModEntry& o) const { return item == o.item && slots == o.slots; }
};
struct SessionModsMsg {
    std::vector<SessionModEntry> entries;
    bool truncated = false;
};

inline std::vector<uint8_t> Encode(const SessionModsMsg& m) {
    Writer w; w.header(MsgType::SessionMods);
    const size_t n = std::min(m.entries.size(), kMaxModItems);
    w.u8(m.truncated || n < m.entries.size() ? 1 : 0);
    w.u8(uint8_t(n));
    for (size_t i = 0; i < n; ++i) { modrules_detail::PutItem(w, m.entries[i].item); w.u16(m.entries[i].slots); }
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, SessionModsMsg& m) {
    if (n > kMaxPacket) return false;
    Reader r(d, n); MsgType t;
    if (!r.header(t) || t != MsgType::SessionMods) return false;
    const uint8_t flags = r.u8(), count = r.u8();
    if (!r.ok() || flags > 1 || count > kMaxModItems) return false;
    m = SessionModsMsg{};
    m.truncated = flags & 1;
    m.entries.resize(count);
    for (SessionModEntry& e : m.entries) {
        if (!modrules_detail::GetItem(r, e.item)) return false;
        e.slots = r.u16();
        if (!r.ok() || e.slots == 0) return false;
    }
    return r.done();
}

// The union for SessionMods: reports by slot (slot < 16), merged by kind + name + hash, in canonical order.
struct SlotReport { uint8_t slot = 0; const ModReport* report = nullptr; };
inline SessionModsMsg BuildSessionMods(const std::vector<SlotReport>& reports) {
    SessionModsMsg m;
    for (const SlotReport& sr : reports) {
        if (!sr.report || sr.slot >= 16) continue;
        for (const ModItem& it : sr.report->items) {
            bool merged = false;
            for (SessionModEntry& e : m.entries) if (e.item == it) { e.slots = uint16_t(e.slots | (1u << sr.slot)); merged = true; break; }
            if (merged) continue;
            if (m.entries.size() >= kMaxModItems) { m.truncated = true; continue; }
            m.entries.push_back({it, uint16_t(1u << sr.slot)});
        }
    }
    std::stable_sort(m.entries.begin(), m.entries.end(), [](const SessionModEntry& a, const SessionModEntry& b) {
        if (a.item.kind != b.item.kind) return uint8_t(a.item.kind) < uint8_t(b.item.kind);
        return modrules_detail::LessNoCase(a.item.name, b.item.name);
    });
    return m;
}

}  // namespace hsm
