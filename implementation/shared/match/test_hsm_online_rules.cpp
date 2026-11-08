// Unit tests for hsm_online_rules.h. Run via implementation/shared/run_tests.sh test_hsm_online_rules.
#include "match/hsm_online_rules.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

// What the launcher's lock adds: a key line for each of HSE's default-key sections.
static std::string LockedDefaults() {
    std::string s;
    for (const char* n : kHseDefaultKeySections) s += std::string("[") + n + "]\nkey = 143\n";
    return s;
}

static const char* kGood =
    "[InfiniteStamina]\r\nkey = -1\r\nenabled = false\r\n\r\n[SpeedMultiplier]\r\nkey = -1\r\nenabled = true\r\nrun_speed_multiplier = 0.750000\r\nwalk_speed_multiplier = 0.800000\r\n"
    "[StrengthMultiplier]\r\nenabled = true\r\nstrength_multiplier = 1.250000\r\ngrab_force_multiplier = 1.000000\r\nhands_rigidity_multiplier = 1.000000\r\n"
    "[Jump]\r\nforce = 5000.000000\r\n[UE]\r\nconsole_enabled = false\r\n[Keybinds]\r\ntoggle_gui_key = 143\r\n";

static std::string Swap(std::string s, const std::string& a, const std::string& b) { s.replace(s.find(a), a.size(), b); return s; }

int main() {
    // A host who chose `anything` with cheats allowed hosts with a cheat mod in: the supported list kept them out even of their own lobby.
    CHECK(!ModListKeepsOut(ModPreset::Anything, {true, true}) && !ModListKeepsOut(ModPreset::Anything, {false, false}));
    CHECK(ModListKeepsOut(ModPreset::MapsOnly, {true, true}) && !ModListKeepsOut(ModPreset::MapsOnly, {true, false}));
    CHECK(ModListKeepsOut(ModPreset::Classes, {true, false}) && ModListKeepsOut(ModPreset::Unrestricted, {false, false}));
    CHECK(IsSupportedUe4ssMod("HSEnhancer") && IsSupportedUe4ssMod("Keybinds") && !IsSupportedUe4ssMod("HulkMod") && !IsSupportedUe4ssMod("AbilityFrameworkMod"));
    CHECK(IsExpectedExeDll("boost_regex-mt-x64.dll") && !IsExpectedExeDll("boost_cheat.dll") && !IsExpectedExeDll("tbbx.dll") && IsExpectedExeDll("DWMAPI.DLL") && !IsExpectedExeDll("xinput1_3.dll") && !IsExpectedExeDll("version.dll"));

    CHECK(HseRuleProblems(kGood).empty());
    const std::string good = kGood;
    CHECK(!HseRuleProblems(Swap(good, "key = -1\r\nenabled = false", "key = 73\r\nenabled = false")).empty());     // hotkey bound: not locked
    CHECK(!HseRuleProblems(Swap(good, "enabled = false", "enabled = true")).empty());                              // infinite stamina
    CHECK(!HseRuleProblems(Swap(good, "strength_multiplier = 1.250000", "strength_multiplier = 10.000000")).empty());
    CHECK(!HseRuleProblems(Swap(good, "strength_multiplier = 1.250000", "strength_multiplier = nan")).empty());    // NaN is out of range
    CHECK(!HseRuleProblems(Swap(good, "force = 5000.000000", "force = 12500.000000")).empty());
    CHECK(!HseRuleProblems(Swap(good, "toggle_gui_key = 143", "toggle_gui_key = 45")).empty());                    // launcher skipped
    CHECK(!HseRuleProblems("").empty());                                                                           // no settings at all
    // A value out of range in a section that is off doesn't count.
    CHECK(HseRuleProblems(good + "[KickMultiplier]\r\nenabled = false\r\nkick_power_multiplier = 35.000000\r\n").empty());

    const std::string wolv = "[Healing]\r\nMode = Logan\r\n[Body]\r\nSkeleton = Bone\r\n[Strength]\r\nEnabled = Off\r\n[Muscle mass]\r\nEnabled = Off\r\n[Claws]\r\nEnabled = Off\r\n";
    CHECK(WolverineRuleProblems(wolv).empty());
    CHECK(!WolverineRuleProblems(Swap(wolv, "Mode = Logan", "Mode = WeaponX")).empty());
    CHECK(!WolverineRuleProblems(Swap(wolv, "[Strength]\r\nEnabled = Off", "[Strength]\r\nEnabled = On")).empty());
    // Claws and a metal skeleton are allowed, but they have to be paid for.
    const std::string clawed = Swap(wolv, "[Claws]\r\nEnabled = Off", "[Claws]\r\nEnabled = On");
    CHECK(WolverineRuleProblems(clawed).empty() && ClawsForbidWeapons(clawed) && !ClawsForbidWeapons(wolv));
    CHECK(ClassPoints("", Swap(clawed, "Skeleton = Bone", "Skeleton = Adamantium")) > ClassPoints("", clawed) + 0.9);
    CHECK(!CheckRuleSheet(BuildRuleSheet("", clawed), "HalfWolverine").empty());

    const auto bad = UnsupportedOnCard("HSEnhancer, HulkMod, version.dll, HalfWolverine");
    CHECK(bad.size() == 2 && bad[0] == "HulkMod" && bad[1] == "version.dll");
    CHECK(UnsupportedOnCard("").empty());
    // The sheet: a good class passes, and each kind of cheat fails on the other player's machine.
    const std::string fullHse = good + "[Dash]\r\nkey = -1\r\n";
    CHECK(CheckRuleSheet(BuildRuleSheet(HseSheetFrom(fullHse + LockedDefaults()), WolverineSheetFrom(Swap(wolv, "Mode = Logan", "Mode = Off"))), "HSEnhancer, HalfWolverine").empty());
    CHECK(CheckRuleSheet(BuildRuleSheet("", ""), "WillieFace").empty());                                  // Fair: nothing to report
    CHECK(!CheckRuleSheet("", "").empty());                                                                // sent nothing
    CHECK(!CheckRuleSheet(BuildRuleSheet("", ""), "HSEnhancer").empty());                                  // hides HSE's settings
    CHECK(!CheckRuleSheet(BuildRuleSheet(HseSheetFrom(Swap(fullHse, "strength_multiplier = 1.250000", "strength_multiplier = 10.000000")), ""), "HSEnhancer").empty());
    CHECK(!CheckRuleSheet(BuildRuleSheet(HseSheetFrom(fullHse + "[NPCSpawnBinding_1]\r\nkey = 78\r\n"), ""), "HSEnhancer").empty());   // a hotkey left bound
    CHECK(!CheckRuleSheet(BuildRuleSheet("", WolverineSheetFrom(Swap(wolv, "Mode = Logan", "Mode = WeaponX"))), "HalfWolverine").empty());
    CHECK(HseSheetFrom(fullHse).size() < kMaxRuleSheet);
    // The budget: an edited file with every stat at the maximum is caught even though each value is in range.
    const std::string on = "[SpeedMultiplier]\nenabled = true\nrun_speed_multiplier = 1.250000\nwalk_speed_multiplier = 1.250000\n"
                           "[StrengthMultiplier]\nenabled = true\nstrength_multiplier = 1.250000\ngrab_force_multiplier = 1.250000\nhands_rigidity_multiplier = 1.250000\n"
                           "[Jump]\nforce = 6000.000000\n[UE]\nconsole_enabled = false\n[Keybinds]\ntoggle_gui_key = 143\nkey = -1\n";
    CHECK(ClassPoints(on, "") > kRuleMaxPoints);   // over the limit, whatever the limit is set to
    CHECK(!CheckRuleSheet(BuildRuleSheet(HseSheetFrom(on), ""), "HSEnhancer").empty());
    // The launcher's classes, value for value: run, walk, str, grab, hands, knock, kick, bones, mass, jump, power x100.
    struct Class { const char* name; const char* heal; bool pain, kickCd, bite, disloc; double run, walk, str, grab, hands, knock, kick, brk, mass, jump, power; };
    static constexpr Class kClasses[] = {
        {"Revenant", "Logan", false, false, false, false, 1.15, 1.15, 0.80, 1.00, 1.00, 0.80, 0.80, 0.70, 0.70, 5000, 12.75},
        {"Barber-Surgeon", "Weathered", false, false, false, false, 0.80, 0.80, 0.70, 1.00, 1.00, 0.70, 0.70, 1.05, 1.00, 5000, 5.5},
        {"Crusader", "Logan", false, false, false, false, 0.75, 0.80, 1.25, 1.00, 0.85, 0.80, 0.80, 0.85, 1.00, 4000, -9.75},
        {"Pugilist", "Off", false, true, true, false, 0.90, 0.90, 0.75, 1.60, 1.60, 1.25, 1.50, 0.80, 0.80, 5000, -10.5},
        {"Footpad", "Off", false, true, false, false, 1.50, 1.15, 0.80, 1.00, 1.00, 0.70, 0.85, 0.70, 0.70, 5000, 12.25},
        {"Knight", "Off", false, false, false, true, 0.70, 0.70, 0.85, 1.00, 1.00, 0.85, 0.70, 1.50, 1.50, 4000, 11.5},
        {"Flagellant", "Off", true, false, false, false, 0.70, 0.70, 1.10, 0.70, 0.70, 1.30, 1.30, 0.70, 0.70, 5000, 7.0},
    };
    auto classIni = [](const Class& c) {
        char b[900];
        std::snprintf(b, sizeof b, "[NoPain]\nenabled = %s\n[NoKickCooldown]\nenabled = %s\n[BiteAttack]\nkey = %s\nenabled = %s\n"
            "[SpeedMultiplier]\nenabled = true\nrun_speed_multiplier = %f\nwalk_speed_multiplier = %f\n"
            "[StrengthMultiplier]\nenabled = true\nstrength_multiplier = %f\ngrab_force_multiplier = %f\nhands_rigidity_multiplier = %f\n"
            "[KnockbackMultiplier]\nenabled = true\nknockback_multiplier = %f\n[KickMultiplier]\nenabled = true\nkick_power_multiplier = %f\n"
            "[BoneControl]\nenabled = true\nbreak_strength_multiplier = %f\nmass_multiplier = %f\nblock_dislocation = %s\n"
            "[Jump]\nforce = %f\n[UE]\nconsole_enabled = false\n[Keybinds]\ntoggle_gui_key = 143\n",
            c.pain ? "true" : "false", c.kickCd ? "true" : "false", c.bite ? "72" : "-1", c.bite ? "true" : "false", c.run, c.walk, c.str, c.grab, c.hands, c.knock, c.kick, c.brk, c.mass, c.disloc ? "true" : "false", c.jump);
        return std::string(b);
    };
    for (const Class& c : kClasses) {
        const std::string hseIni = classIni(c), wolvIni = Swap(wolv, "Mode = Logan", std::string("Mode = ") + c.heal);
        CHECK(std::abs(ClassPoints(hseIni, wolvIni) * 100 - c.power) < 0.01);
        CHECK(CheckRuleSheet(BuildRuleSheet(HseSheetFrom(hseIni + LockedDefaults()), WolverineSheetFrom(wolvIni)), "HSEnhancer, HalfWolverine").empty());
    }
    // Run and walk pay as a pair.
    auto speedOnly = [](const char* run, const char* walk) { return std::string("[SpeedMultiplier]\nenabled = true\nrun_speed_multiplier = ") + run + "\nwalk_speed_multiplier = " + walk + "\n"; };
    CHECK(std::abs(ClassPoints(speedOnly("0.100000", "1.250000"), "") - 0.375) < 0.001);      // run 10%, walk 125%: run earns nothing
    CHECK(std::abs(ClassPoints(speedOnly("0.700000", "1.000000"), "") + 0.075) < 0.001);      // run alone: only the gap
    CHECK(std::abs(ClassPoints(speedOnly("1.000000", "0.700000"), "") + 0.15) < 0.001);       // walk alone: only the gap, at walk's rate
    CHECK(std::abs(ClassPoints(speedOnly("0.700000", "0.700000"), "") + 0.675) < 0.001);      // both down: full
    CHECK(ClassPoints(speedOnly("0.100000", "0.100000"), "") == ClassPoints(speedOnly("0.700000", "0.700000"), ""));   // past 70% earns no more
    // Jump is flavour: dropping it buys nothing. The cap is 200%.
    const std::string paladin = classIni(kClasses[2]);
    CHECK(ClassPoints(Swap(paladin, "force = 4000.000000", "force = 0.000000"), wolv) == ClassPoints(paladin, wolv));
    CHECK(HseRuleProblems(Swap(paladin, "strength_multiplier = 1.250000", "strength_multiplier = 2.000000")).empty());
    CHECK(!HseRuleProblems(Swap(paladin, "strength_multiplier = 1.250000", "strength_multiplier = 2.010000")).empty());
    // Only a class with Bite on keeps Bite's key, and no other key stays bound.
    const std::string brawler = classIni(kClasses[3]);
    CHECK(!AnyHotkeyBound(brawler) && HseRuleProblems(brawler).empty());
    CHECK(AnyHotkeyBound(Swap(brawler, "key = 72\nenabled = true", "key = 72\nenabled = false")));
    CHECK(AnyHotkeyBound(brawler + "[NoPain]\nkey = 73\n"));
    CHECK(!AnyHotkeyBound("[NoPain]\nkey = 143\n[Dash]\nkey = -1\n"));   // the lock's key code is no key
    // Claws cost by what they're made of, whatever the skeleton is, and the sheet carries that to the host.
    auto clawCost = [](const char* body) { return ClassPoints("", std::string("[Body]\n") + body + "[Claws]\nEnabled = On\n"); };
    CHECK(std::abs(clawCost("Skeleton = Bone\nClaws = Bone\n") - 0.60) < 0.001);
    CHECK(std::abs(clawCost("Skeleton = Bone\nClaws = Adamantium\n") - 1.80) < 0.001);
    CHECK(std::abs(clawCost("Skeleton = Bone\nClaws = Same\n") - 0.60) < 0.001);
    CHECK(std::abs(clawCost("Skeleton = Adamantium\nClaws = Bone\n") - 1.40) < 0.001);
    CHECK(std::abs(clawCost("Skeleton = Adamantium\nClaws = Same\n") - 2.60) < 0.001);
    CHECK(WolverineSheetFrom("[Body]\nSkeleton = Bone\nClaws = Adamantium\n").find("Claws = Adamantium") != std::string::npos);
    // A section with no key line keeps HSE's default key, as J for Jump did, and the sheet says so.
    std::string allKeys;
    for (const char* s : kHseDefaultKeySections) allKeys += std::string("[") + s + "]\nkey = 143\n";
    CHECK(!DefaultKeyLeft(allKeys));
    CHECK(DefaultKeyLeft("[Jump]\nforce = 5000.000000\n"));
    CHECK(HseSheetFrom("[Jump]\nforce = 5000.000000\n[Keybinds]\ntoggle_gui_key = 143\n").find("key = 1\n") != std::string::npos);
    CHECK(HseSheetFrom(allKeys).find("key = -1\n") != std::string::npos);
    const RuleMods listed = ReadRuleMods("Keybinds, HSEnhancer, HulkMod, HalfWolverine");
    CHECK(listed.hse && listed.wolverine && listed.unsupported.size() == 1 && listed.unsupported[0] == "HulkMod");
    CHECK(LooksLikeHseLoader("xx HSE_Initialize yy") && !LooksLikeHseLoader("a chainloader"));
    // Sheathed claws leave the hands free; unseen claws don't.
    CHECK(ClawsDropWeapons(true, true, true) && !ClawsDropWeapons(true, true, false) && ClawsDropWeapons(true, false, false));
    CHECK(!ClawsDropWeapons(false, true, true) && !ClawsDropWeapons(false, false, false));
    if (g_fail == 0) std::printf("ok\n");
    return g_fail != 0;
}
