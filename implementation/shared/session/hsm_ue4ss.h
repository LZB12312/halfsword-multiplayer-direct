// UE4SS chain-load decisions (pure, no engine or Windows types). HSMMod's dwmapi.dll is the one proxy next to the game
// exe and loads UE4SS itself (client src/ue4ss_load.cpp does the file checks and LoadLibrary; this header decides).
//
// Layouts: UE4SS 3.x keeps everything but its proxy under <exe dir>/ue4ss/ (UE4SS.dll, UE4SS-settings.ini, Mods/);
// older installs keep UE4SS.dll, the settings and Mods/ next to the exe; a 2.x install is proxied by xinput1_3.dll and
// loads itself until the bundled build takes over (see ChooseBundleStep). The UE4SS mod list is names only, for bug reports: enabled mods
// from Mods/mods.txt ("Name : 1" / "Name : 0", ';' comments) and folders holding an enabled.txt.
#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace hsm::ue4ss {

constexpr size_t kMaxMods = 32;        // mod names kept
constexpr size_t kMaxNameLen = 48;     // bytes of one name
constexpr size_t kMaxModsTxtBytes = 64 * 1024;   // mods.txt is read at most this far
constexpr size_t kMaxFactLen = 400;    // the mod list as one report fact

// ---------------------------------------------------------------- what to load
struct Files {
    bool sub = false;       // <exe dir>/ue4ss/UE4SS.dll (3.x)
    bool beside = false;    // <exe dir>/UE4SS.dll (older installs)
    bool xinput2x = false;  // <exe dir>/xinput1_3.dll that is UE4SS's (2.x proxy), see LooksLikeUe4ss2x
};
enum class Pick { None, Sub, Beside, Proxy2x };

// 2.x wins (UE4SS already runs itself; loading a second copy would double-hook), then the 3.x layout, then the old one.
inline Pick ChoosePick(const Files& f) {
    if (f.xinput2x) return Pick::Proxy2x;
    if (f.sub) return Pick::Sub;
    if (f.beside) return Pick::Beside;
    return Pick::None;
}
// Path relative to the exe dir, '/' separated; "" for None and Proxy2x.
inline const char* RelPath(Pick p) { return p == Pick::Sub ? "ue4ss/UE4SS.dll" : p == Pick::Beside ? "UE4SS.dll" : ""; }
// The folder holding Mods/ and UE4SS-settings.ini for that pick, relative to the exe dir ("" = the exe dir itself).
inline const char* RelDir(Pick p) { return p == Pick::Sub ? "ue4ss" : ""; }
inline const char* PickName(Pick p) {
    return p == Pick::Sub ? "3.x layout" : p == Pick::Beside ? "old layout" : p == Pick::Proxy2x ? "2.x proxy" : "none";
}
// An xinput1_3.dll beside the exe is UE4SS's when it sits next to UE4SS files (a real one lives in System32).
inline bool LooksLikeUe4ss2x(bool xinput13Present, bool settingsIni, bool modsDir) { return xinput13Present && (settingsIni || modsDir); }

// HSMMod carries one UE4SS build inside its DLL, and that's the one that runs. Install: ue4ss/ has no UE4SS.dll. Replace: it has
// an older accepted build. Ask: it has a build nobody here knows, which may be the player's own and can't be got back, so they
// choose. Keep: they already said no to this very file. Install and Replace write the DLL and every other file already there
// stays. Then an old-layout UE4SS.dll and a 2.x proxy beside the exe get .off on their names, so nothing else starts a second UE4SS.
enum class BundleStep { None, Install, Replace, Ask, Keep };
inline BundleStep ChooseBundleStep(bool subThere, bool subIsBundled, bool subIsAccepted, bool subIsKept) {
    if (!subThere) return BundleStep::Install;
    if (subIsBundled) return BundleStep::None;
    if (subIsAccepted) return BundleStep::Replace;
    return subIsKept ? BundleStep::Keep : BundleStep::Ask;
}
// Where a file of the bundled zip goes, relative to the exe dir with backslashes; "" for what stays in the zip. Only ue4ss/ is
// unpacked: the zip's dwmapi.dll is UE4SS's proxy, and that slot is HSMMod's.
inline std::string BundleFilePath(std::string_view zipName) {
    constexpr std::string_view kRoot = "ue4ss/";
    if (zipName.size() <= kRoot.size() || zipName.compare(0, kRoot.size(), kRoot) != 0 || zipName.back() == '/') return {};
    if (zipName.find("..") != std::string_view::npos || zipName.find('\\') != std::string_view::npos || zipName.find(':') != std::string_view::npos) return {};
    std::string out(zipName);
    std::replace(out.begin(), out.end(), '/', '\\');
    return out;
}

// ue4ss_order: "before" (load UE4SS first) or anything else = "after" (default: after HSMMod's ProcessEvent hook).
enum class Order { After, Before };
namespace detail {
inline bool Space(char c) { return c == ' ' || c == '\t' || c == '\r'; }
inline std::string_view Trim(std::string_view s) {
    while (!s.empty() && Space(s.front())) s.remove_prefix(1);
    while (!s.empty() && Space(s.back())) s.remove_suffix(1);
    return s;
}
inline char Low(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
inline bool SameName(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (Low(a[i]) != Low(b[i])) return false;
    return true;
}
}  // namespace detail
// UE4SS's stock console and cheat manager mods. The console's `set` is a cheat tool on its own, so the bundled build has them off
// and a Vanilla lobby refuses them; UE4SS without them is not a mod to a lobby.
inline constexpr const char* kConsoleMods[] = {"CheatManagerEnablerMod", "ConsoleCommandsMod", "ConsoleEnablerMod"};
inline bool IsConsoleMod(std::string_view name) {
    for (const char* m : kConsoleMods) if (detail::SameName(m, name)) return true;
    return false;
}
inline Order ParseOrder(const std::string& s) { return detail::SameName(detail::Trim(s), "before") ? Order::Before : Order::After; }
inline const char* OrderName(Order o) { return o == Order::Before ? "before" : "after"; }

// ---------------------------------------------------------------- mods.txt
struct ModEntry { std::string name; bool enabled = false; };

// A mod name fit for a report: no control characters, cut to kMaxNameLen. Empty when nothing is left or a control
// character is in it (names are folder names; a name with a control character is not one).
inline std::string CleanName(std::string_view raw) {
    std::string out;
    for (char c : detail::Trim(raw)) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) return {};
        if (out.size() < kMaxNameLen) out.push_back(c);
    }
    return out;
}

// mods.txt: "ModName : 1" enables, ": 0" disables (any other value counts as off); blank lines, ';' comment lines and
// lines without ':' are skipped; a UTF-8 BOM and CRLF are fine. A name listed twice keeps its last value and first
// position. At most kMaxMods entries; text past kMaxModsTxtBytes is ignored.
inline std::vector<ModEntry> ParseModsTxt(const std::string& text) {
    std::vector<ModEntry> out;
    std::string_view t(text.data(), std::min(text.size(), kMaxModsTxtBytes));
    if (t.size() >= 3 && t.compare(0, 3, "\xEF\xBB\xBF") == 0) t.remove_prefix(3);
    size_t pos = 0;
    while (pos <= t.size()) {
        const size_t nl = t.find('\n', pos);
        std::string_view line = t.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? t.size() + 1 : nl + 1;
        line = detail::Trim(line);
        if (line.empty() || line[0] == ';') continue;
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string name = CleanName(line.substr(0, colon));
        if (name.empty()) continue;
        const bool on = detail::Trim(line.substr(colon + 1)) == "1";
        bool merged = false;
        for (ModEntry& e : out) if (detail::SameName(e.name, name)) { e.enabled = on; merged = true; break; }
        if (!merged && out.size() < kMaxMods) out.push_back({name, on});
    }
    return out;
}

// The enabled mods: mods.txt entries set to 1, then folders with an enabled.txt that mods.txt does not list at all
// (a ": 0" in mods.txt wins). No duplicates by name (any case), at most kMaxMods, order kept. `enabledFolders`: the
// folder names under Mods/ that hold an enabled.txt.
inline std::vector<std::string> EnabledMods(const std::vector<ModEntry>& txt, const std::vector<std::string>& enabledFolders) {
    std::vector<std::string> out;
    auto add = [&out](const std::string& n) {
        if (n.empty() || out.size() >= kMaxMods) return;
        for (const std::string& o : out) if (detail::SameName(o, n)) return;
        out.push_back(n);
    };
    for (const ModEntry& e : txt) if (e.enabled) add(e.name);
    for (const std::string& f : enabledFolders) {
        const std::string n = CleanName(f);
        bool listed = false;
        for (const ModEntry& e : txt) if (detail::SameName(e.name, n)) { listed = true; break; }
        if (!listed) add(n);
    }
    return out;
}

// "A, B, C" as one report fact, at most kMaxFactLen bytes; names that do not fit become " +N more". "" for no mods.
inline std::string FormatMods(const std::vector<std::string>& names) {
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) {
        const std::string piece = (out.empty() ? "" : ", ") + names[i];
        const size_t left = names.size() - i - 1;
        const size_t reserve = left ? (" +" + std::to_string(left) + " more").size() : 0;
        if (out.size() + piece.size() + reserve > kMaxFactLen) {
            out += " +" + std::to_string(names.size() - i) + " more";
            return out;
        }
        out += piece;
    }
    return out;
}

// ---------------------------------------------------------------- the version and the proxy
// "a.b.c.d" from a file version; an all-zero version has no information.
inline std::string FormatFileVersion(unsigned a, unsigned b, unsigned c, unsigned d) {
    if (!a && !b && !c && !d) return "unknown version";
    return std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c) + "." + std::to_string(d);
}

// Proxy names other than dwmapi.dll that UE4SS can be built with and that can sit beside the exe.
inline bool OtherProxyName(std::string_view file) {
    for (const char* n : {"version.dll", "dsound.dll", "winmm.dll", "xinput1_3.dll"})
        if (detail::SameName(file, n)) return true;
    return false;
}

// What to tell the player (once, in the log and on the multiplayer page) about the proxies beside the exe, or "".
// `oursIsBesideDwmapi`: HSMMod itself is the dwmapi.dll next to the exe. `dwmapiPresent`: some dwmapi.dll is there.
// `ue4ssSpare`: ue4ss/dwmapi.dll exists (UE4SS's own proxy kept inside its folder: harmless). `others`: other known proxy names found beside the exe.
inline std::string ProxyNote(bool oursIsBesideDwmapi, bool dwmapiPresent, bool ue4ssSpare, const std::vector<std::string>& others) {
    std::string note;
    if (!oursIsBesideDwmapi && dwmapiPresent)
        note = "the dwmapi.dll next to the game exe is not HSMMod's (updating UE4SS overwrites it); copy HSMMod's dwmapi.dll over it again";
    else if (ue4ssSpare)
        note = oursIsBesideDwmapi ? "ue4ss/dwmapi.dll is UE4SS's own proxy and stays unused: HSMMod's dwmapi.dll next to the exe loads UE4SS"
                                  : "ue4ss/dwmapi.dll is UE4SS's own proxy; HSMMod's dwmapi.dll belongs next to the game exe";
    if (!others.empty()) {
        std::string list;
        for (size_t i = 0; i < others.size() && i < 4; ++i) list += (list.empty() ? "" : ", ") + others[i];
        if (!note.empty()) note += "; ";
        note += "another proxy DLL is next to the exe (" + list + "); if it is UE4SS's, HSMMod's dwmapi.dll alone should load UE4SS";
    }
    return note;
}

}  // namespace hsm::ue4ss
