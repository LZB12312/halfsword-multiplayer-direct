// The lobby's MODS tab: who runs each mod ("everyone", names, "+N", unknown slots), UE4SS's stock mods left out, the
// refused rows first with their reason under each preset, the tones, the host's rows enabled and a member's not, the
// hints when the rule in force differs from the host's choice, the status lines, the truncated footer, the host's
// pending lines and the steppers.
#include "match/hsm_lobby_mods.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static ModItem Item(ModKind k, const char* name) { ModItem it; it.kind = k; it.name = name; return it; }
static ModRule Rule(ModPreset p, bool cheats = true) { ModRule r; r.preset = p; r.blockCheats = cheats; r.officialOnly = false; return r; }

static LobbyModsLocal Lobby(bool host, ModRule rule = Rule(ModPreset::MapsOnly)) {
    LobbyModsLocal l;
    l.host = host; l.rulesOn = true; l.hostHasRule = true; l.haveList = true; l.rule = rule;
    l.chosen = ModChoice{rule.preset, rule.blockCheats, rule.officialOnly};
    l.names[0] = "Ann"; l.names[1] = "Bob"; l.names[2] = "Cy";
    l.present = 0b111;
    l.mods.entries = {
        {Item(ModKind::MapPak, "Arena.pak"), 0b111},
        {Item(ModKind::Ue4ssStock, "BPModLoaderMod"), 0b111},
        {Item(ModKind::Ue4ssMod, "HSEnhancer"), 0b010},
        {Item(ModKind::Ue4ssMod, "WillieFace"), 0b101},
    };
    return l;
}
static const ModRowView* Row(const LobbyModsScene& s, const char* name) {
    for (const ModRowView& r : s.items) if (r.name == name) return &r;
    return nullptr;
}

static void Holders() {
    LobbyModsLocal l = Lobby(false);
    LobbyModsScene s = BuildLobbyMods(l);
    CHECK(Row(s, "Arena.pak") && Row(s, "Arena.pak")->holders == "everyone");
    CHECK(Row(s, "WillieFace") && Row(s, "WillieFace")->holders == "Ann, Cy");
    CHECK(Row(s, "HSEnhancer") && Row(s, "HSEnhancer")->holders == "Bob");
    // Alone in the session: a name, not "everyone".
    l.present = 0b001;
    CHECK(ModHolders(l, 0b001) == "Ann");
    // Five holders: three names and "+2"; unknown slots by number, slot 0 as the host.
    l.present = 0b111111;
    CHECK(ModHolders(l, 0b011111) == "Ann, Bob, Cy +2");
    LobbyModsLocal anon; anon.present = 0b1001;
    CHECK(ModHolders(anon, 0b1000) == "player 4");
    CHECK(ModHolders(anon, 0b0001) == "host");
    CHECK(ModHolders(anon, 0b1001) == "everyone");
    CHECK(ModSlotCount(0b1011) == 3);
}

static void StockHiddenAndOrder() {
    LobbyModsScene s = BuildLobbyMods(Lobby(false));
    CHECK(Row(s, "BPModLoaderMod") == nullptr);
    CHECK(s.items.size() == 3);
    // Maps only refuses HSEnhancer (a cheat): first; the rest keep the canonical order.
    CHECK(s.items.size() == 3 && s.items[0].name == "HSEnhancer" && s.items[1].name == "Arena.pak" && s.items[2].name == "WillieFace");
    CHECK(s.items[0].refused && s.items[0].note == "refused: a known cheat");
    CHECK(!s.items[1].refused && s.items[1].note.empty());
    CHECK(s.empty.empty());
}

static void Tones() {
    LobbyModsScene s = BuildLobbyMods(Lobby(false, Rule(ModPreset::Anything, false)));
    CHECK(Row(s, "HSEnhancer")->tone == ModTone::Bad && Row(s, "HSEnhancer")->verdict == "CHEAT" && !Row(s, "HSEnhancer")->refused);
    CHECK(Row(s, "Arena.pak")->tone == ModTone::Ok && Row(s, "Arena.pak")->verdict == "map");
    CHECK(Row(s, "WillieFace")->tone == ModTone::Ok && Row(s, "WillieFace")->verdict == "cosmetic");
    CHECK(ModToneOf(KnownVerdict::Gameplay, ModKind::Ue4ssMod, false) == ModTone::Warn);
    CHECK(ModToneOf(KnownVerdict::Suspect, ModKind::Ue4ssMod, false) == ModTone::Warn);
    CHECK(ModToneOf(KnownVerdict::Unknown, ModKind::Ue4ssMod, false) == ModTone::Dim);
    CHECK(ModToneOf(KnownVerdict::Unknown, ModKind::Ue4ssMod, true) == ModTone::Bad);
    CHECK(std::string(ModVerdictText(KnownVerdict::Unknown, ModKind::Proxy)) == "overlay or loader");
    CHECK(std::string(ModVerdictText(KnownVerdict::Unknown, ModKind::Ue4ssMod)) == "unknown");
    CHECK(std::string(ModVerdictText(KnownVerdict::Gameplay, ModKind::Ue4ssMod)) == "gameplay");
}

static void RefusedByPreset() {
    // Anything with cheats allowed: nothing refused.
    LobbyModsScene any = BuildLobbyMods(Lobby(false, Rule(ModPreset::Anything, false)));
    for (const ModRowView& r : any.items) CHECK(!r.refused);
    // Anything with cheats blocked: only the cheat.
    LobbyModsScene anyBlock = BuildLobbyMods(Lobby(false, Rule(ModPreset::Anything, true)));
    CHECK(Row(anyBlock, "HSEnhancer")->refused && !Row(anyBlock, "Arena.pak")->refused);
    // Vanilla: every mod.
    LobbyModsScene van = BuildLobbyMods(Lobby(false, Rule(ModPreset::Vanilla, false)));
    CHECK(Row(van, "Arena.pak")->refused && Row(van, "Arena.pak")->note == "refused: Vanilla takes no mods");
    CHECK(Row(van, "WillieFace")->refused);
    // Maps only, cheats allowed: an unknown UE4SS mod is not allowed, the map and the cosmetic are.
    LobbyModsLocal l = Lobby(false, Rule(ModPreset::MapsOnly, false));
    l.mods.entries.push_back({Item(ModKind::Ue4ssMod, "SomethingNew"), 0b100});
    LobbyModsScene maps = BuildLobbyMods(l);
    CHECK(Row(maps, "SomethingNew")->refused && Row(maps, "SomethingNew")->note == "refused: not allowed (Maps only)");
    CHECK(Row(maps, "HSEnhancer")->refused);   // not a map, not cosmetic
    CHECK(!Row(maps, "Arena.pak")->refused && !Row(maps, "WillieFace")->refused);
    // Custom: only the list (plus cosmetics); a required mod someone lacks says who.
    ModRule custom = Rule(ModPreset::Custom, false);
    ModAllow a; a.name = "Arena.pak"; a.required = true; custom.list.push_back(a);
    ModAllow b; b.name = "HSEnhancer"; b.required = true; custom.list.push_back(b);
    LobbyModsScene cs = BuildLobbyMods(Lobby(false, custom));
    CHECK(Row(cs, "Arena.pak")->note.empty());
    CHECK(Row(cs, "HSEnhancer")->note == "required: missing for Ann, Cy" && Row(cs, "HSEnhancer")->tone == ModTone::Warn && !Row(cs, "HSEnhancer")->refused);
    // A required mod nobody runs gets a row of its own.
    ModRule need = Rule(ModPreset::Custom, false);
    need.list = custom.list;
    ModAllow c; c.name = "Pack.pak"; c.required = true; need.list.push_back(c);
    LobbyModsScene ns0 = BuildLobbyMods(Lobby(false, need));
    CHECK(Row(ns0, "Pack.pak") && Row(ns0, "Pack.pak")->note == "required: missing for everyone" && Row(ns0, "Pack.pak")->verdict == "missing");
    CHECK(!ns0.items.empty() && ns0.items.back().name == "Pack.pak");
    size_t arenas = 0;
    for (const ModRowView& r : ns0.items) arenas += r.name == "Arena.pak";
    CHECK(arenas == 1);   // a required mod someone runs: its own row only
    ModAllow opt; opt.name = "Optional.pak"; need.list.push_back(opt);
    CHECK(Row(BuildLobbyMods(Lobby(false, need)), "Optional.pak") == nullptr);   // not required, nobody runs it: no row
    // A hash-only list the browser cannot read (decoded with no names): nothing marked refused.
    custom.list.clear(); custom.listHashOnly = true;
    LobbyModsScene hidden = BuildLobbyMods(Lobby(false, custom));
    for (const ModRowView& r : hidden.items) CHECK(!r.refused);
    // No rule in force: no notes at all.
    LobbyModsLocal none = Lobby(false, Rule(ModPreset::Vanilla, true));
    none.hostHasRule = false;
    LobbyModsScene ns = BuildLobbyMods(none);
    for (const ModRowView& r : ns.items) CHECK(!r.refused && r.note.empty());
    CHECK(ns.summary.empty() && ns.status == "The host does not check mods.");
}

static void RuleRows() {
    LobbyModsScene host = BuildLobbyMods(Lobby(true));
    for (const ModRuleView& r : host.rules) CHECK(r.enabled);
    CHECK(host.rules[0].id == ModRuleRow::Preset && host.rules[0].value == "Maps only");
    CHECK(host.rules[1].id == ModRuleRow::Cheats && host.rules[1].value == "On");
    CHECK(host.rules[2].id == ModRuleRow::Official && host.rules[2].value == "Off");
    CHECK(host.summary == "In force: Maps only, cheats blocked.");
    CHECK(host.status.empty());
    LobbyModsScene member = BuildLobbyMods(Lobby(false));
    for (const ModRuleView& r : member.rules) CHECK(!r.enabled);
    CHECK(member.rules[0].value == "Maps only");
    CHECK(std::string(ModPresetHint(ModPreset::Custom, false)) == "The mods the host listed.");
    CHECK(std::string(ModPresetHint(ModPreset::Classes, true)).find("classes") != std::string::npos);
    CHECK(std::string(ModPresetHint(ModPreset::Unrestricted, false)).find("UNRESTRICTED") != std::string::npos);
    // Rules off: rows disabled, the status says so, nothing refused.
    LobbyModsLocal off = Lobby(true);
    off.rulesOn = false;
    LobbyModsScene so = BuildLobbyMods(off);
    for (const ModRuleView& r : so.rules) CHECK(!r.enabled);
    CHECK(so.status.find("mod_rules=0") != std::string::npos && so.summary.empty());
    for (const ModRowView& r : so.items) CHECK(!r.refused);
}

static void DiffersHints() {
    // The host chose Bundle with no pack announced: Maps only is in force.
    LobbyModsLocal l = Lobby(true);
    l.chosen = ModChoice{ModPreset::Bundle, true, true};
    l.rule = Rule(ModPreset::MapsOnly, false);   // it runs a cheat itself; not an official build
    LobbyModsScene s = BuildLobbyMods(l);
    CHECK(s.rules[0].value == "Bundle" && s.rules[0].hint.find("Maps only applies") != std::string::npos);
    CHECK(s.rules[1].value == "On" && s.rules[1].hint.find("cheat yourself") != std::string::npos);
    CHECK(s.rules[2].value == "On" && s.rules[2].hint.find("not an official release") != std::string::npos);
    CHECK(s.summary == "In force: Maps only, cheats allowed.");
    // A member sees the rule in force, no host hints.
    LobbyModsLocal m = l; m.host = false;
    LobbyModsScene ms = BuildLobbyMods(m);
    CHECK(ms.rules[0].value == "Maps only" && ms.rules[1].value == "Off" && ms.rules[2].value == "Off");
    CHECK(ms.rules[1].hint.find("cheat yourself") == std::string::npos);
}

static void StatusAndFooters() {
    LobbyModsLocal l = Lobby(false);
    l.haveList = false;
    LobbyModsScene s = BuildLobbyMods(l);
    CHECK(s.items.empty() && s.empty.empty() && s.status.find("Waiting") == 0);
    l.haveList = true; l.mods.entries = {{Item(ModKind::Ue4ssStock, "Keybinds"), 0b111}};
    LobbyModsScene e = BuildLobbyMods(l);
    CHECK(e.items.empty() && e.empty == "Nobody here runs mods." && e.status.empty());
    l.mods.truncated = true;
    CHECK(BuildLobbyMods(l).truncated == "The list stops at 96 mods.");
    CHECK(!BuildLobbyMods(l).privacy.empty());
}

static void Pending() {
    ModPending wait; wait.name = "Bob"; wait.held = true;
    CHECK(ModPendingLine(wait) == "Bob: waiting for their mod list");
    wait.haveList = true; wait.result.fail = RuleFail::Pending;
    CHECK(ModPendingLine(wait) == "Bob: still scanning their mods");
    wait.result.fail = RuleFail::None;
    CHECK(ModPendingLine(wait) == "Bob: checking mods");
    ModPending leave; leave.name = "Ann"; leave.held = false; leave.graceSeconds = 42;
    leave.result.fail = RuleFail::Cheat; leave.result.mod = "HSEnhancer";
    CHECK(ModPendingLine(leave) == "Ann: runs HSEnhancer, leaving in 42 s unless fixed");
    leave.result.fail = RuleFail::Missing; leave.result.mod = "Arena.pak";
    CHECK(ModPendingLine(leave) == "Ann: lacks Arena.pak, leaving in 42 s unless fixed");
    leave.result.fail = RuleFail::NoMods; leave.result.mod = "ConsoleEnablerMod";
    CHECK(ModPendingLine(leave) == "Ann: runs ConsoleEnablerMod, leaving in 42 s unless fixed");
    leave.graceSeconds = 0;
    CHECK(ModPendingLine(leave).empty());
    // Only the host gets the lines; empty ones are dropped.
    LobbyModsLocal h = Lobby(true);
    h.pending = {wait, leave};
    CHECK(BuildLobbyMods(h).pending.size() == 1 && BuildLobbyMods(h).pending[0] == "Bob: checking mods");
    h.host = false;
    CHECK(BuildLobbyMods(h).pending.empty());
}

static void Steppers() {
    ModChoice c{ModPreset::Vanilla, true, false};
    CHECK(StepModRule(c, ModRuleRow::Preset, -1).preset == ModPreset::Classes);
    CHECK(StepModRule(c, ModRuleRow::Preset, 1).preset == ModPreset::MapsOnly);
    c.preset = ModPreset::Anything;
    CHECK(StepModRule(c, ModRuleRow::Preset, 1).preset == ModPreset::Classes);   // Classes is a stop of its own
    CHECK(StepModRule(c, ModRuleRow::Preset, -1).preset == ModPreset::Custom);
    ModChoice t = StepModRule(c, ModRuleRow::Cheats, 1);
    CHECK(!t.blockCheats && t.preset == c.preset && !t.officialOnly);
    t = StepModRule(c, ModRuleRow::Official, -1);
    CHECK(t.officialOnly && t.blockCheats);
    CHECK(StepModRule(StepModRule(c, ModRuleRow::Cheats, 1), ModRuleRow::Cheats, -1) == c);
    // Unrestricted is never stepped into, and steps as Classes. From the default, one row gets to Classes.
    for (ModPreset p : {ModPreset::Classes, ModPreset::Unrestricted}) {
        ModChoice k = c; k.preset = p;
        CHECK(StepModRule(k, ModRuleRow::Preset, 1).preset == ModPreset::Vanilla);
        CHECK(StepModRule(k, ModRuleRow::Preset, -1).preset == ModPreset::Anything);
    }
    ModChoice w = c;
    for (int i = 0; i < 12; ++i) { w = StepModRule(w, ModRuleRow::Preset, 1); CHECK(w.preset != ModPreset::Unrestricted); }
    w.preset = ModPreset::MapsOnly;
    CHECK(StepModRule(StepModRule(w, ModRuleRow::Preset, -1), ModRuleRow::Preset, -1).preset == ModPreset::Classes);
}

int main() {
    Holders();
    StockHiddenAndOrder();
    Tones();
    RefusedByPreset();
    RuleRows();
    DiffersHints();
    StatusAndFooters();
    Pending();
    Steppers();
    if (g_fail) { std::printf("test_hsm_lobby_mods: %d FAILED\n", g_fail); return 1; }
    std::printf("test_hsm_lobby_mods: PASS\n");
    return 0;
}
