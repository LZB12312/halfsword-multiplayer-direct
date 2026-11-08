// The lobby's MODS tab (docs/PLAN_MOD_RULES.md 5), as data: the lobby's mod rule (three rows the host steps: preset,
// block cheats, official builds only), then one row per mod anyone in the session runs, from the host's SessionMods
// (session/hsm_mod_rules.h): its kind, what the known-mods table says about it, who runs it, and why this lobby's rule
// refuses it. The host also sees who it is still checking and who must fix their mods. ui/menu.cpp fills a
// LobbyModsLocal and draws the scene; the stepper arithmetic is here too.
#pragma once
#include "session/hsm_mod_rules.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hsm {

inline constexpr size_t kModSlots = 16;          // SessionMods masks: bit = slot
inline constexpr size_t kModHolderNames = 3;     // names on a row before "+N"

// Host only: a player its rule is still checking, or one that no longer fits and is counting down.
struct ModPending {
    std::string name;
    bool held = false;           // joined, not admitted yet (no fighter)
    bool haveList = false;       // its ModList came
    uint64_t graceSeconds = 0;   // admitted, no longer fits: removed in this many seconds unless fixed
    RuleResult result;           // CheckMods(the host's rule, its report)
};

struct LobbyModsLocal {
    bool host = false;
    bool rulesOn = true;         // host: its [mod] mod_rules (off: it enforces nothing)
    bool hostHasRule = true;     // member: the lobby data names a rule (a host with mod_rules=0 names Anything with none)
    bool haveList = false;       // a SessionMods came (member) or was built (host)
    ModRule rule;                // in force: the host's HostRule, or a member's reading of the lobby data
    ModChoice chosen;            // host: what it chose
    SessionModsMsg mods;
    std::array<std::string, kModSlots> names;   // by slot, empty when unknown
    uint16_t present = 0;        // the slots in the session (bit = slot)
    std::vector<ModPending> pending;            // host only
};

enum class ModRuleRow : uint8_t { Preset, Cheats, Official };
inline constexpr size_t kModRuleRows = 3;
enum class ModTone : uint8_t { Ok, Dim, Warn, Bad };   // the page: green, dim, amber, the accent's red

struct ModRuleView { ModRuleRow id = ModRuleRow::Preset; std::string label, value, hint; bool enabled = false; };
struct ModRowView {
    std::string name, kind, verdict, holders, note;   // note: why this lobby refuses it, or who lacks a required one
    ModTone tone = ModTone::Dim;
    bool refused = false;
};

struct LobbyModsScene {
    std::array<ModRuleView, kModRuleRows> rules{};
    std::string summary;                 // "In force: Maps only, cheats blocked"
    std::string status;                  // why there is no list or no rule; empty when all is well
    std::vector<ModRowView> items;       // refused first, then by kind and name, then required mods nobody runs; UE4SS's stock mods left out unless refused
    std::string empty;                   // a list came and nobody runs a mod
    std::string truncated;
    std::vector<std::string> pending;    // host only
    std::string privacy;
};

// The verdict column: what the known-mods table says, or what the kind says when it says nothing.
inline const char* ModVerdictText(KnownVerdict v, ModKind k) {
    switch (v) {
        case KnownVerdict::Cheat: return "CHEAT";
        case KnownVerdict::Gameplay: return "gameplay";
        case KnownVerdict::Suspect: return "suspect";
        case KnownVerdict::Cosmetic: return "cosmetic";
        case KnownVerdict::Stock: return "stock";
        case KnownVerdict::Unknown: break;
    }
    if (k == ModKind::MapPak) return "map";
    if (k == ModKind::Proxy) return "overlay or loader";
    return "unknown";
}

inline ModTone ModToneOf(KnownVerdict v, ModKind k, bool refused) {
    if (refused || v == KnownVerdict::Cheat) return ModTone::Bad;
    if (v == KnownVerdict::Gameplay || v == KnownVerdict::Suspect) return ModTone::Warn;
    if (v == KnownVerdict::Cosmetic || v == KnownVerdict::Stock || k == ModKind::MapPak) return ModTone::Ok;
    return ModTone::Dim;
}

inline std::string ModSlotName(const LobbyModsLocal& l, size_t slot) {
    if (slot < l.names.size() && !l.names[slot].empty()) return l.names[slot];
    return slot == 0 ? std::string("host") : "player " + std::to_string(slot + 1);
}

// "Ann, Bob, Cy +2" in slot order, at most kModHolderNames names.
inline std::string ModSlotList(const LobbyModsLocal& l, uint16_t mask) {
    std::string s;
    size_t shown = 0, more = 0;
    for (size_t slot = 0; slot < kModSlots; ++slot) {
        if (!(mask & (1u << slot))) continue;
        if (shown == kModHolderNames) { ++more; continue; }
        if (shown++) s += ", ";
        s += ModSlotName(l, slot);
    }
    if (more) s += " +" + std::to_string(more);
    return s;
}

inline size_t ModSlotCount(uint16_t mask) {
    size_t n = 0;
    for (; mask; mask = uint16_t(mask & (mask - 1))) ++n;
    return n;
}

// Who runs it: "everyone" when every player in the session does (two or more of them), else their names.
inline std::string ModHolders(const LobbyModsLocal& l, uint16_t mask) {
    if (ModSlotCount(l.present) >= 2 && (mask & l.present) == l.present) return "everyone";
    return ModSlotList(l, mask);
}

inline std::string ModRefusedNote(RuleFail f, ModPreset p) {
    switch (f) {
        case RuleFail::Cheat: return "refused: a known cheat";
        case RuleFail::NoMods: return "refused: Vanilla takes no mods";
        case RuleFail::NotAllowed: return std::string("refused: not allowed (") + ModPresetName(p) + ")";
        default: return {};
    }
}

// The host's line for a player it is checking: "Bob: checking mods", "Ann: runs HSEnhancer, leaving in 42 s".
inline std::string ModPendingLine(const ModPending& p) {
    if (p.held) {
        if (!p.haveList) return p.name + ": waiting for their mod list";
        if (p.result.fail == RuleFail::Pending) return p.name + ": still scanning their mods";
        return p.name + ": checking mods";
    }
    if (p.graceSeconds == 0) return {};
    std::string why;
    switch (p.result.fail) {
        case RuleFail::Cheat: case RuleFail::NoMods: case RuleFail::NotAllowed: why = "runs " + p.result.mod; break;
        case RuleFail::Missing: why = "lacks " + p.result.mod; break;
        case RuleFail::TooMany: why = "runs too many mods"; break;
        default: why = "does not fit"; break;
    }
    return p.name + ": " + why + ", leaving in " + std::to_string(p.graceSeconds) + " s unless fixed";
}

inline const char* ModPresetHint(ModPreset p, bool host) {
    switch (p) {
        case ModPreset::Vanilla: return "No mods: no map paks, no UE4SS mods.";
        case ModPreset::MapsOnly: return "Map paks and known cosmetic mods.";
        case ModPreset::Bundle: return "Exactly the lobby's modpack.";
        case ModPreset::Custom: return host ? "Your own mods (or the list in mod_allow in HSMMod.ini)." : "The mods the host listed.";
        case ModPreset::Anything: return "Any mods.";
        case ModPreset::Classes: return "Native classes. Map paks, content paks and supported UE4SS mods.";
        case ModPreset::Unrestricted: return "Native classes without the power budget. Set with UNRESTRICTED below.";
    }
    return "";
}

inline LobbyModsScene BuildLobbyMods(const LobbyModsLocal& l) {
    LobbyModsScene s;
    const bool live = l.host ? l.rulesOn : l.hostHasRule;   // a rule is in force in this lobby
    const ModChoice shown = l.host ? l.chosen : ChoiceOf(l.rule);
    const bool enabled = l.host && l.rulesOn;
    const auto onOff = [](bool b) { return std::string(b ? "On" : "Off"); };

    ModRuleView& preset = s.rules[0];
    preset.id = ModRuleRow::Preset; preset.label = "Mods allowed"; preset.value = ModPresetName(shown.preset);
    preset.hint = ModPresetHint(shown.preset, l.host);
    if (l.host && l.rulesOn && shown.preset == ModPreset::Bundle && l.rule.preset == ModPreset::MapsOnly)
        preset.hint += " No modpack is announced, so Maps only applies.";
    ModRuleView& cheats = s.rules[1];
    cheats.id = ModRuleRow::Cheats; cheats.label = "Block cheats"; cheats.value = onOff(shown.blockCheats);
    cheats.hint = "Refuses known cheat mods such as HS Enhancer.";
    if (l.host && l.rulesOn && shown.blockCheats && !l.rule.blockCheats) cheats.hint = "You run a cheat yourself, so this lobby allows them.";
    ModRuleView& official = s.rules[2];
    official.id = ModRuleRow::Official; official.label = "Official builds only"; official.value = onOff(shown.officialOnly);
    official.hint = "Only the release HSMMod from GitHub may join.";
    if (l.host && l.rulesOn && shown.officialOnly && !l.rule.officialOnly) official.hint = "Your HSMMod is not an official release, so this is off.";
    for (ModRuleView& r : s.rules) r.enabled = enabled;

    if (live) s.summary = "In force: " + RuleSummary(l.rule) + ".";
    if (l.host && !l.rulesOn) s.status = "Mod rules are off (mod_rules=0 in HSMMod.ini): anyone may join with any mods.";
    else if (!l.host && !l.hostHasRule) s.status = "The host does not check mods.";
    else if (!l.haveList) s.status = "Waiting for the session's mod list...";

    if (l.haveList) {
        for (const SessionModEntry& e : l.mods.entries) {
            const KnownVerdict v = VerdictOf(e.item);
            const RuleFail f = live ? ItemRefused(l.rule, e.item) : RuleFail::None;
            if (e.item.kind == ModKind::Ue4ssStock && f == RuleFail::None) continue;
            ModRowView r;
            r.name = e.item.name;
            r.kind = ModKindName(e.item.kind);
            r.verdict = ModVerdictText(v, e.item.kind);
            r.holders = ModHolders(l, e.slots);
            r.note = ModRefusedNote(f, l.rule.preset);
            r.refused = !r.note.empty();
            r.tone = ModToneOf(v, e.item.kind, r.refused);
            if (!r.refused && live && (l.rule.preset == ModPreset::Bundle || l.rule.preset == ModPreset::Custom)) {
                const uint16_t lacking = uint16_t(l.present & ~e.slots);
                for (const ModAllow& a : l.rule.list)
                    if (a.required && lacking && AllowMatches(a, e.item)) { r.note = "required: missing for " + ModSlotList(l, lacking); r.tone = ModTone::Warn; break; }
            }
            s.items.push_back(std::move(r));
        }
        if (live && (l.rule.preset == ModPreset::Bundle || l.rule.preset == ModPreset::Custom))
            for (const ModAllow& a : l.rule.list) {   // a required mod nobody here runs: no entry carries it
                if (!a.required) continue;
                bool held = false;
                for (const SessionModEntry& e : l.mods.entries) if (AllowMatches(a, e.item)) { held = true; break; }
                if (held) continue;
                ModRowView r;
                r.name = a.name; r.kind = "required"; r.verdict = "missing"; r.tone = ModTone::Warn;
                r.note = "required: missing for " + ModHolders(l, l.present);
                s.items.push_back(std::move(r));
            }
        std::stable_sort(s.items.begin(), s.items.end(), [](const ModRowView& a, const ModRowView& b) { return a.refused && !b.refused; });
        if (s.items.empty()) s.empty = "Nobody here runs mods.";
        if (l.mods.truncated) s.truncated = "The list stops at " + std::to_string(kMaxModItems) + " mods.";
    }
    if (l.host)
        for (const ModPending& p : l.pending)
            if (std::string line = ModPendingLine(p); !line.empty()) s.pending.push_back(std::move(line));
    s.privacy = "Everyone in the lobby sees this list: mod file names and hashes, nothing else.";
    return s;
}

// A host's click on a rule row: the preset steps round its six, the switches flip.
inline ModChoice StepModRule(ModChoice c, ModRuleRow row, int dir) {
    switch (row) {
        case ModRuleRow::Preset: {
            // Vanilla to Classes. Unrestricted is set with this tab's checkbox (it has its disclaimer there), so it steps as Classes.
            if (c.preset == ModPreset::Unrestricted) c.preset = ModPreset::Classes;
            const int n = int(ModPreset::Classes) + 1;
            c.preset = ModPreset(((int(c.preset) + (dir < 0 ? -1 : 1)) % n + n) % n);
            break;
        }
        case ModRuleRow::Cheats: c.blockCheats = !c.blockCheats; break;
        case ModRuleRow::Official: c.officialOnly = !c.officialOnly; break;
    }
    return c;
}

}  // namespace hsm
