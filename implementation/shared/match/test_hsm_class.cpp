#include "match/hsm_class.h"
#include "match/hsm_online_rules.h"
#include "session/hsm_ue4ss.h"
#include <cstdio>
#include <limits>
using namespace hsm;
static int g_fail = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); ++g_fail; } } while (0)

int main() {
    // Bite: H, the old default, moves to B once; a key the player chose, or H after the move, stays.
    CHECK(BiteKeyMovesToB("H", "") && BiteKeyMovesToB("h", "0") && !BiteKeyMovesToB("H", "1") && !BiteKeyMovesToB("G", "") && !BiteKeyMovesToB("F5", ""));
    // The built-ins keep the power the launcher's versions had, and all fit the rules.
    struct Want { const char* name; double power; };
    for (const Want& w : {Want{"Man-at-Arms", 0}, {"Revenant", 12.75}, {"Barber-Surgeon", 5.5}, {"Crusader", -9.75}, {"Pugilist", -10.5}, {"Footpad", 12.25}, {"Knight", 11.5}, {"Flagellant", 7.0}, {"Wrestler", 11.0}, {"Pilgrim", 10.5}, {"Veteran", 13.0}, {"Verity", 15.0}, {"Wulf", 11.5}, {"Ironside", 5.75}, {"Old Scar", 5.0}}) {
        bool found = false;
        for (const NamedClass& c : BuiltInClasses()) if (std::string_view(c.name) == w.name) {
            found = true;
            CHECK(std::abs(ClassPoints(c.sheet) * 100 - w.power) < 0.01);
            CHECK(ClassProblems(c.sheet).empty());
        }
        CHECK(found);
    }
    // The plain fighter changes nothing and needs no mod. Stat classes need no mod either: that's what makes them fair on vanilla players.
    CHECK(ClassSheet{} == BuiltInClasses()[0].sheet && ClassPoints(ClassSheet{}) == 0 && !ClassNeedsWolverine(ClassSheet{}));
    for (const NamedClass& c : BuiltInClasses()) if (std::string_view(c.name) == "Footpad") CHECK(!ClassNeedsWolverine(c.sheet));
    ClassSheet clawed; clawed.claws = true;
    CHECK(ClassNeedsWolverine(clawed) && std::abs(ClassPoints(clawed) - 0.60) < 1e-9);
    clawed.metalClaws = true;
    CHECK(std::abs(ClassPoints(clawed) - 1.80) < 1e-9 && !ClassProblems(clawed).empty());

    // Over the limits, however it's spelled.
    ClassSheet s;
    s.strength = 2.01; CHECK(!ClassProblems(s).empty());
    s = {}; s.mass = 0.4; CHECK(!ClassProblems(s).empty());
    s = {}; s.run = std::nan(""); CHECK(!ClassProblems(s).empty());
    s = {}; s.jump = 6001; CHECK(!ClassProblems(s).empty());
    s = {}; s.metalClaws = true; CHECK(!ClassProblems(s).empty());
    s = {}; s.noPain = true; CHECK(!ClassProblems(s).empty());

    // Every class survives the trip, and what a player sends can't half-read.
    for (const NamedClass& c : BuiltInClasses()) {
        const std::string e = EncodeClass(c.sheet);
        CHECK(!e.empty() && e.size() <= kMaxClassValue);
        const auto d = DecodeClass(e);
        CHECK(d && std::abs(ClassPoints(*d) - ClassPoints(c.sheet)) < 1e-9 && d->heal == c.sheet.heal && d->bite == c.sheet.bite && d->noPain == c.sheet.noPain);
    }
    const std::string good = EncodeClass(ClassSheet{});
    CHECK(DecodeClass(good).has_value());
    for (const char* bad : {"", "r1", "x", "r1.000 w1.000", "r1.000 w1.000 s1.000 g1.000 h1.000 k1.000 n1.000 b1.000 m1.000 j5000 f000000000 ",
                            "rnan w1.000 s1.000 g1.000 h1.000 k1.000 n1.000 b1.000 m1.000 j5000 f000000000",
                            "rinf w1.000 s1.000 g1.000 h1.000 k1.000 n1.000 b1.000 m1.000 j5000 f000000000",
                            "r1.000 w1.000 s1.000 g1.000 h1.000 k1.000 n1.000 b1.000 m1.000 j5000 f000030000",
                            "r1.000 w1.000 s1.000 g1.000 h1.000 k1.000 n1.000 b1.000 m1.000 j5000 f00000000",
                            "w1.000 r1.000 s1.000 g1.000 h1.000 k1.000 n1.000 b1.000 m1.000 j5000 f000000000"})
        CHECK(!DecodeClass(bad).has_value());
    CHECK(!DecodeClass(std::string(kMaxClassValue + 1, 'r')).has_value());

    // A class gives no edge in a lobby without the rule.
    CHECK(ClassAppliesHere(false, false) && ClassAppliesHere(true, true) && !ClassAppliesHere(true, false));

    // A stat never compounds, follows the game when it changes, and 1.0 leaves it alone.
    ClassStatTrack t;
    CHECK(std::abs(t.Next(100, 0.7) - 70) < 1e-9);
    CHECK(std::abs(t.Next(70, 0.7) - 70) < 1e-9);
    CHECK(std::abs(t.Next(70, 0.7) - 70) < 1e-9);
    CHECK(std::abs(t.Next(200, 0.7) - 140) < 1e-9);   // the game set a new value
    CHECK(std::abs(t.Next(140, 0.7) - 140) < 1e-9);
    ClassStatTrack same;
    CHECK(same.Next(100, 1.0) == 100 && same.Next(100, 1.0) == 100);

    // The picker walks every class and wraps.
    CHECK(ClassNameStep("Man-at-Arms", 1) == "Revenant" && ClassNameStep("Man-at-Arms", -1) == "Custom 8" && ClassNameStep("Flagellant", 1) == "Landsknecht" && ClassNameStep("Custom 3", 1) == "Custom 4" && ClassNameStep("Custom 8", 1) == "Man-at-Arms" && ClassNameStep("nonsense", 1) == "Revenant");
    CHECK(ClassPickCount() == 28 && ClassPickName(0) == "Man-at-Arms" && ClassPickName(20) == "Custom 1" && ClassPickName(27) == "Custom 8");
    // The CLASSES tab: every row explains itself, and the bars sit in the middle for the plain fighter and follow the stats.
    for (int i = 0; i < kClassFieldCount; ++i) CHECK(std::string_view(ClassFieldInfo(i)).size() > 20);
    const ClassBars plain = ClassBarsFor(ClassSheet{});
    CHECK(plain.attack == 0.5f && plain.speed == 0.5f && plain.toughness == 0.5f);
    ClassSheet fast; fast.run = 2; fast.walk = 2; fast.bones = 0.5; fast.mass = 0.5;
    CHECK(ClassBarsFor(fast).speed == 1.f && ClassBarsFor(fast).toughness == 0.f);

    // The player's own: a slot reads as what's saved, an empty or broken one as the plain fighter, and anything else isn't a class.
    ClassSheet saved; saved.run = 0.8; saved.strength = 1.1;
    const std::string customs[kCustomClassSlots] = {EncodeClass(saved), "", "junk", EncodeClass(saved), "", "", ""};
    CHECK(ResolveClass("Custom 1", customs)->run == 0.8 && *ResolveClass("Custom 2", customs) == ClassSheet{} && *ResolveClass("Custom 3", customs) == ClassSheet{});
    CHECK(ResolveClass("Knight", customs)->mass == 1.5 && ResolveClass("Custom 4", customs)->run == 0.8 && !ResolveClass("Launcher", customs) && !ResolveClass("Custom 9", customs) && *ResolveClass("Custom 8", customs) == ClassSheet{} && !ResolveClass("", customs));
    CHECK(CustomClassSlot("Custom 2") == 1 && CustomClassSlot("Man-at-Arms") == -1 && CustomClassName(0) == "Custom 1");

    // A class can carry a name. What the file says is checked every time it's read, so editing it by hand gets nothing past this.
    const std::string names[kCustomClassSlots] = {"Bleeder", "", "knight", "Name That Is Far Too Long For It", "", "", ""};
    CHECK(ClassLabel("Custom 1", names) == "Bleeder" && ClassLabel("Custom 2", names) == "Custom 2" && ClassLabel("Custom 3", names) == "Custom 3" && ClassLabel("Custom 4", names) == "Custom 4" && ClassLabel("Knight", names) == "Knight");
    CHECK(ClassNameProblem("Bone Reaver 2").empty() && ClassNameProblem("Night-Owl").empty() && ClassNameProblem("Sir_Hit's").empty());
    CHECK(!ClassNameProblem("").empty() && !ClassNameProblem(" a").empty() && !ClassNameProblem("1234").empty() && !ClassNameProblem("Knight").empty() && !ClassNameProblem("KNIGHT").empty() && !ClassNameProblem("custom 2").empty());
    CHECK(!ClassNameProblem("Caf\xc3\xa9").empty());   // a lookalike letter from another alphabet could spell anything
    CHECK(!ClassNameProblem("a\nb").empty() && !ClassNameProblem(std::string(kMaxClassName + 1, 'a')).empty() && ClassNameProblem(std::string(kMaxClassName, 'a')).empty());
    CHECK(!ClassNameProblem("sp1c").empty() && ClassNameProblem("Spice").empty());
    // A peer's name goes through the same check here, and can't claim a built-in it isn't.
    ClassSheet boosted; boosted.run = 1.3;
    CHECK(ClassNameFromPeer("Bleeder", boosted) == "Bleeder" && ClassNameFromPeer("", ClassSheet{}) == "Man-at-Arms");
    CHECK(ClassNameFromPeer("Man-at-Arms", ClassSheet{}) == "Man-at-Arms" && ClassNameFromPeer("MAN-AT-ARMS", ClassSheet{}) == "Man-at-Arms");
    // A player on HS Enhancer reads as that, not as a custom class, and the name is no cover for a real one.
    CHECK(ClassNameFromPeer(kHseClassName, ClassSheet{}) == kHseClassName && ClassNameFromPeer("hs enhancer", ClassSheet{}) == kHseClassName && ClassNameFromPeer(kHseClassName, boosted) == "Custom");
    CHECK(ClassNameFromPeer("Man-at-Arms", boosted) == "Custom" && ClassNameFromPeer("", boosted) == "Custom" && ClassNameFromPeer("sp1c", boosted) == "Custom" && ClassNameFromPeer("Custom 2", boosted) == "Custom 2");
    for (const NamedClass& c : BuiltInClasses()) {
        CHECK(ClassNameFromPeer(c.name, *DecodeClass(EncodeClass(c.sheet))) == c.name);   // what a real one sends reads as itself
        CHECK(!NameHasSlur(c.name));
    }

    // The tabs. Every class is on exactly one, the first tab needs no mod, and a Wolverine class is on Wolverine's or the hacker tab.
    CHECK(ClassTabs().size() == 4 && ClassTabs()[kClassTabHacker].unrestrictedOnly && std::string(ClassTabs()[kClassTabWolverine].needsMod) == "HalfWolverine");
    int onTabs = 0;
    for (int t = 0; t < int(ClassTabs().size()); ++t) onTabs += int(ClassTabPicks(t).size());
    CHECK(onTabs == ClassPickCount());
    CHECK(ClassTabPicks(kClassTabPlain)[0] == "Man-at-Arms");
    for (const std::string& n : ClassTabPicks(kClassTabPlain)) { CHECK(!ClassNeedsWolverine(FindClassNamed(n)->sheet) && ClassPickable(n, false) && !ClassPickable(n, true)); CHECK(ClassProblems(FindClassNamed(n)->sheet).empty()); }
    CHECK(ClassTabPicks(kClassTabWolverine).size() >= 6);
    for (const std::string& n : ClassTabPicks(kClassTabWolverine)) {
        const ClassSheet& c = FindClassNamed(n)->sheet;
        CHECK(ClassNeedsWolverine(c) && !ClassPickable(n, false) && ClassPickable(n, true));
        CHECK(ClassProblems(c).empty());   // balanced: they fit the same budget as everything else
    }
    CHECK(ClassTabPicks(kClassTabCustom).size() == 8 && ClassPickable("Custom 1", false));
    // A player's own classes are on the Custom tab and nowhere else, and the Classes tab shows none of Half Wolverine's rows.
    for (int t = 0; t < int(ClassTabs().size()); ++t) if (t != kClassTabCustom) for (const std::string& n : ClassTabPicks(t)) CHECK(CustomClassSlot(n) < 0);
    for (int i = 0; i < 4; ++i) CHECK(!ClassFieldShown(i, kClassTabPlain) && ClassFieldShown(i, kClassTabCustom) && ClassFieldShown(i, kClassTabWolverine));
    CHECK(ClassFieldShown(4, kClassTabPlain));   // your own stay open, so what's saved can be changed
    // Without the mod its rows can't be turned on, and one that's on can be turned off.
    ClassSheet noMod;
    CHECK(ClassFieldLocked(noMod, 0, false) && ClassFieldLocked(noMod, 3, false) && !ClassFieldLocked(noMod, 4, false) && !ClassFieldLocked(noMod, 0, true));
    noMod.claws = true; CHECK(!ClassFieldLocked(noMod, 2, false));

    // Unrestricted: no budget, but still a ceiling, however a player's copy of the mod is changed.
    CHECK(IsHackerClass("Glitch") && !IsHackerClass("Custom 2") && !IsHackerClass("Man-at-Arms"));
    for (const NamedClass& c : BuiltInClasses()) {
        CHECK(ClassProblems(c.sheet, true).empty());
        if (c.tab == kClassTabHacker) CHECK(!ClassProblems(c.sheet).empty());   // none of them works in a normal Classes lobby
    }
    ClassSheet big; big.strength = 5; big.run = 5; big.mass = 5; big.jump = 15000; big.noPain = true; big.claws = true; big.metalClaws = true; big.heal = ClassHeal::Weathered;
    CHECK(ClassProblems(big, true).empty() && !ClassProblems(big).empty());
    big = {}; big.strength = 5.01; CHECK(!ClassProblems(big, true).empty());
    big = {}; big.mass = 5.01; CHECK(!ClassProblems(big, true).empty());
    big = {}; big.mass = 0.4; CHECK(!ClassProblems(big, true).empty());
    big = {}; big.jump = 15001; CHECK(!ClassProblems(big, true).empty());
    big = {}; big.run = std::nan(""); CHECK(!ClassProblems(big, true).empty());
    big = {}; big.kick = std::numeric_limits<double>::infinity(); CHECK(!ClassProblems(big, true).empty());
    big = {}; big.metalClaws = true; CHECK(!ClassProblems(big, true).empty());
    ClassSheet hack;
    for (int i = 0; i < 40; ++i) { ClassFieldStep(hack, 10, 1, true); ClassFieldStep(hack, 17, 1, true); }
    CHECK(hack.strength == kHackerStatMax && hack.mass == kRuleMassMax && ClassProblems(hack, true).empty());
    for (int i = 0; i < 40; ++i) ClassFieldStep(hack, 17, -1, true);
    CHECK(hack.mass == kRuleMassMin);
    // A peer in an Unrestricted lobby is judged by its limits, and HS Enhancer stays refused unless this player turned that off.
    CHECK(CheckClassValue(EncodeClass(FindClassNamed("Glitch")->sheet), "", true).empty());
    CHECK(!CheckClassValue(EncodeClass(FindClassNamed("Glitch")->sheet), "", false).empty());
    CHECK(!CheckClassValue(EncodeClass(ClassSheet{}), "HSEnhancer", true).empty() && !CheckClassValue(EncodeClass(ClassSheet{}), "HSEnhancer", false).empty());
    CHECK(CheckClassValue(EncodeClass(ClassSheet{}), "HSEnhancer", false, false).empty());
    // A lobby's strictness comes from its host, and a value that doesn't read is strict.
    CHECK(DecodeLobbyBlock(EncodeLobbyBlock({false, false})).hse == false && DecodeLobbyBlock(EncodeLobbyBlock({false, false})).allLobbies == false);
    CHECK(DecodeLobbyBlock(EncodeLobbyBlock({})).hse && DecodeLobbyBlock("").hse && DecodeLobbyBlock("").allLobbies && DecodeLobbyBlock("xyz").hse);
    // Half Wolverine on or off decides which classes are offered, and where the screen opens.
    CHECK(ClassHomeTab(true) == kClassTabWolverine && ClassHomeTab(false) == kClassTabPlain);
    CHECK(!ClassTabUsable(kClassTabPlain, true) && ClassTabUsable(kClassTabPlain, false) && ClassTabUsable(kClassTabWolverine, true) && !ClassTabUsable(kClassTabWolverine, false));
    CHECK(ClassTabUsable(kClassTabCustom, true) && ClassTabUsable(kClassTabCustom, false) && ClassTabUsable(kClassTabHacker, true) && ClassTabUsable(kClassTabHacker, false));
    CHECK(std::string(ClassTabRefusal(kClassTabPlain, true)).size() > 10 && std::string(ClassTabRefusal(kClassTabWolverine, false)).size() > 10 && std::string(ClassTabRefusal(kClassTabCustom, true)).empty());
    CHECK(ClassHomeClass(false) == kPlainClass && ClassNeedsWolverine(FindClassNamed(ClassHomeClass(true))->sheet));
    CHECK(!ClassPickable(kPlainClass, true) && ClassPickable(kPlainClass, false) && ClassPickable("Wulf", true) && !ClassPickable("Wulf", false) && ClassPickable("Custom 1", true) && ClassPickable("Custom 1", false));
    // A pick that can't fight moves without the CLASSES tab being opened; one that can, or one HS Enhancer has switched off, stays.
    CHECK(ClassFallback("Wulf", false, false) == std::optional<std::string>(kPlainClass));
    CHECK(ClassFallback(kPlainClass, true, false) == std::optional<std::string>(ClassHomeClass(true)));
    CHECK(!ClassFallback("Wulf", true, false) && !ClassFallback(kPlainClass, false, false) && !ClassFallback("Custom 1", false, false));
    CHECK(!ClassFallback("Wulf", false, true));
    // A lobby that allows HS Enhancer doesn't then throw you out for using it, and the overlay doesn't say online is closed to you.
    CHECK(BaseStatCheckApplies(true, {}) && !BaseStatCheckApplies(true, {false, true}) && !BaseStatCheckApplies(false, {}));
    CHECK(ModBlockIntro(ModBlockLines({}, {}, true), true).find("allows it") != std::string::npos);
    CHECK(ModBlockIntro(ModBlockLines({"Cheat"}, {}, true), true).find("stays off") != std::string::npos);
    // What a fighter's really running: the fighter's own values win over what the class or a file says, and anything past Unrestricted's limits stands out.
    ClassSheet declared; declared.run = 1.2;
    const ClassSheet liveOne = LiveSheet(declared, "", kGameRunRate * 3, kGameMusclePower * 2);
    CHECK(liveOne.run == 3 && liveOne.strength == 2 && !LivePastLimits(liveOne));
    CHECK(LiveSheet(declared, "", 0, 0).run == 1.2);   // no fighter to read: what the class says
    CHECK(LivePastLimits(LiveSheet(declared, "", kGameRunRate * 50, 0)) && LivePastLimits(LiveSheet(declared, "", 0, kGameMusclePower * 50)));
    const ClassSheet fromFile = LiveSheet({}, "[KickMultiplier]\nenabled = true\nkick_power_multiplier = 9.000000\n", 0, 0);
    CHECK(fromFile.kick == 9 && LivePastLimits(fromFile));
    CHECK(DecodeClass(EncodeClass(liveOne)).has_value());
    CHECK(!ClassNameProblem(kHseClassName).empty() && !ClassNameProblem("hs enhancer").empty());
    // The budget beside the bars: where a class stands, and by how much it's over.
    CHECK(ClassBudget(ClassSheet{}, false).text == "POWER 0 OF +15, 15 LEFT" && !ClassBudget(ClassSheet{}, false).over);
    ClassSheet overBudget; overBudget.noPain = true;
    CHECK(ClassBudget(overBudget, false).over && ClassBudget(overBudget, false).text == "POWER +100 OF +15  OVER BY 85");
    CHECK(!ClassBudget(overBudget, true).over && ClassBudget(overBudget, true).text.find("no limit") != std::string::npos);
    // Weight is a real number in half steps, up and down, and goes to 5.0 without Unrestricted (the budget is what stops it).
    ClassSheet heavy;
    CHECK(ClassField(heavy, 17).value == "1.0");
    ClassFieldStep(heavy, 17, 1); CHECK(heavy.mass == 1.5 && ClassField(heavy, 17).value == "1.5");
    for (int i = 0; i < 20; ++i) ClassFieldStep(heavy, 17, 1);
    CHECK(heavy.mass == 5.0 && ClassField(heavy, 17).value == "5.0" && !ClassProblems(heavy).empty() && ClassProblems(heavy, true).empty());
    // Strength and Muscle are the game's Muscle Rate, on top of the lobby's fair build. Nobody gets more than they paid for.
    ClassSheet plainBuild, strongBuild, muscled; strongBuild.strength = 2.0; muscled.muscle = true;
    CHECK(ClassWeight(0.3, plainBuild) == 0.3 && std::abs(ClassWeight(0.3, strongBuild) - 1.3) < 1e-9 && ClassWeight(0.3, muscled) == kWolverineMuscleRate);
    strongBuild.strength = 5.0; CHECK(ClassWeight(1.0, strongBuild) == kMaxClassWeight);
    strongBuild.strength = 0.1; CHECK(ClassWeight(0.3, strongBuild) == 0.0);
    // Dead fields earn nothing: lowering grip or hands can't pay for anything real.
    ClassSheet noGrip; noGrip.grip = 0.1; noGrip.hands = 0.1; CHECK(ClassPoints(noGrip) == 0);
    // Stamina: above 100% only part of a drop is felt, below it more, and gains are left alone.
    CHECK(ClassStaminaNext(100, 90, 2.0) == 95 && ClassStaminaNext(100, 90, 0.5) == 80 && ClassStaminaNext(90, 100, 2.0) == 100 && ClassStaminaNext(1, 0, 0.1) == 0);
    // A value saved before Recovery and Endurance reads as the game's own, and new ones come through.
    CHECK(DecodeClass("r1 w1 s1 g1 h1 k1 n1 b1 m1 j5000 f000000000").has_value() && DecodeClass("r1 w1 s1 g1 h1 k1 n1 b1 m1 j5000 f000000000")->recovery == 1);
    ClassSheet newOnes; newOnes.recovery = 2; newOnes.endurance = 1.5;
    CHECK(DecodeClass(EncodeClass(newOnes))->recovery == 2 && DecodeClass(EncodeClass(newOnes))->endurance == 1.5);
    // Only a change to Half Wolverine's own parts needs it reloaded, so stepping a stat doesn't reload every mod.
    ClassSheet partsA, partsB; partsB.run = 2;
    CHECK(!ClassWolverinePartsDiffer(partsA, partsB));
    partsB.claws = true; CHECK(ClassWolverinePartsDiffer(partsA, partsB));
    // The glitched bars: in range, moving, and repeatable.
    for (uint32_t f = 0; f < 500; ++f) { const float v = GlitchedBar(0.5f, f); CHECK(v >= 0.f && v <= 1.f && v == GlitchedBar(0.5f, f)); }
    CHECK(GlitchedBar(0.5f, 0) != GlitchedBar(0.5f, 4) || GlitchedBar(0.5f, 4) != GlitchedBar(0.5f, 8));
    // The overlay names everything, with what to do about it.
    const std::vector<ModBlockLine> block = ModBlockLines({"CheatMod"}, {"Multiplayer.dll", "version"}, true);
    CHECK(block.size() == 4 && block[0].what == "HS Enhancer" && block[1].how.find("CheatMod : 0") != std::string::npos && block[1].modsTxtName == "CheatMod" && block[1].how.find("delete") == std::string::npos && block[2].modsTxtName.empty() && block[0].modsTxtName == "HSEnhancer" && block[2].how.find("Move Multiplayer.dll") != std::string::npos && block[3].how.find("Move version.dll") != std::string::npos && block[3].setAsideFile == "version.dll" && block[2].setAsideFile == "Multiplayer.dll" && block[1].setAsideFile.empty());
    CHECK(ModBlockLines({}, {}, false).empty());
    // Turning mods off in mods.txt: their lines change, one it lacks is added, comments and everything else stay, and it reads back as off.
    const std::string modsBefore = "; a comment : 1\r\nCheat : 1\r\nKeep : 1\r\n";
    const std::string modsAfter = ModsTxtWithOff(modsBefore, {"cheat", "Other"});
    CHECK(modsAfter == "; a comment : 1\r\ncheat : 0\r\nKeep : 1\r\nOther : 0\r\n");
    CHECK(ModsTxtWithOff("", {"A"}) == "A : 0\n" && ModsTxtWithOff("B : 1", {"A"}) == "B : 1\nA : 0\n");
    const std::vector<std::string> stillOn = ue4ss::EnabledMods(ue4ss::ParseModsTxt(modsAfter), {"Other", "Keep", "cheat"});
    CHECK(stillOn.size() == 1 && stillOn[0] == "Keep");
    // Each changed file gets the fix that works for it. Steam only restores the game's own.
    const std::vector<ModBlockLine> changedLines = ModBlockLines({}, {"UE4SS.dll (changed)", "dxgi.dll (changed)", "tbb.dll (changed)"}, false);
    CHECK(changedLines.size() == 3 && changedLines[0].how.find("the mod puts its own back") != std::string::npos && changedLines[0].how.find("Steam") == std::string::npos);
    CHECK(changedLines[1].how.find("ReShade") != std::string::npos && changedLines[1].how.find("Steam") == std::string::npos && changedLines[2].how.find("Verify") != std::string::npos);
    // Three copies of one temporary DLL are one line, and it says where.
    const std::vector<ModBlockLine> outsideLines = ModBlockLines({}, {}, false, {"C:\\Temp\\a\\190_E658703.dll", "C:\\Temp\\b\\190_E658703.dll", "C:\\Temp\\c.dll"});
    CHECK(outsideLines.size() == 2 && outsideLines[0].what == "190_E658703.dll" && outsideLines[0].how.find("C:\\Temp\\a") != std::string::npos && outsideLines[1].what == "c.dll");

    // The Forge's rows move and stop at the limits, and don't drift.
    ClassSheet forge;
    for (int i = 0; i < 40; ++i) ClassFieldStep(forge, 8, 1);
    CHECK(forge.run == kRuleStatMax && ClassField(forge, 8).value == "200%");
    for (int i = 0; i < 60; ++i) ClassFieldStep(forge, 8, -1);
    CHECK(forge.run == kRuleStatMin && ClassField(forge, 8).value == "10%");
    for (int i = 0; i < 60; ++i) ClassFieldStep(forge, 17, -1);
    CHECK(forge.mass == kRuleMassMin);
    ClassSheet drift; for (int i = 0; i < 7; ++i) ClassFieldStep(drift, 10, -1);
    CHECK(drift.strength == 0.65 && ClassField(drift, 10).value == "65%");
    ClassSheet cycle; ClassFieldStep(cycle, 2, 1); CHECK(cycle.claws && !cycle.metalClaws && ClassField(cycle, 2).value == "Bone");
    ClassFieldStep(cycle, 2, 1); CHECK(cycle.claws && cycle.metalClaws); ClassFieldStep(cycle, 2, 1); CHECK(!cycle.claws && !cycle.metalClaws);
    ClassFieldStep(cycle, 2, -1); CHECK(cycle.metalClaws);
    ClassFieldStep(cycle, 0, 1); CHECK(cycle.heal == ClassHeal::Logan); ClassFieldStep(cycle, 0, -1); CHECK(cycle.heal == ClassHeal::Off); ClassFieldStep(cycle, 0, -1); CHECK(cycle.heal == ClassHeal::Weathered);
    for (int i = 0; i < kClassFieldCount; ++i) { ClassSheet a; ClassFieldStep(a, i, 1); CHECK(!(a == ClassSheet{})); CHECK(ClassField(a, i).label && !ClassField(a, i).value.empty()); }

    // The class checker's line says what differs, and shouts about the one that ignores armour.
    CHECK(ClassSummary(ClassSheet{}) == "power 0");
    ClassSheet shown; shown.claws = true; shown.metalClaws = true; shown.run = 0.7; shown.noPain = true;
    const std::string sum = ClassSummary(shown);
    CHECK(sum.find("ADAMANTIUM CLAWS") != std::string::npos && sum.find("run 70%") != std::string::npos && sum.find("no pain") != std::string::npos && sum.find("walk") == std::string::npos && sum.find("power +") != std::string::npos);

    // The game's own values pass, and a trainer's don't.
    CHECK(ClassBasesFair(35.0, 2.0) && !ClassBasesFair(350.0, 2.0) && !ClassBasesFair(35.0, 5.0));
    CHECK(FindClassNamed("Knight") && !FindClassNamed("knight") && !FindClassNamed(""));

    // A class another player sends.
    const std::string fair = EncodeClass(ClassSheet{});
    CHECK(CheckClassValue(fair, "HSHelper, SteadyAI").empty());
    CHECK(CheckClassValue("", "").size() == 1 && CheckClassValue("junk", "")[0] == "no class data");
    CHECK(!CheckClassValue(fair, "HSHelper, HSEnhancer").empty());                     // HSE's menu can change anything
    ClassSheet god; god.strength = 2.0; god.run = 2.0;
    CHECK(!CheckClassValue(EncodeClass(god), "").empty());
    ClassSheet bone; bone.claws = true; bone.strength = 0.7; bone.run = 0.8; bone.walk = 0.8; bone.bones = 0.7;
    CHECK(ClassProblems(bone).empty());
    CHECK(!CheckClassValue(EncodeClass(bone), "SteadyAI").empty() && CheckClassValue(EncodeClass(bone), "HalfWolverine").empty());

    // Half Wolverine's file: only the class's parts change, comments, order and line endings stay.
    const std::string wolverineFile = "; top\r\n[Healing]\r\n; how fast\r\nMode = WeaponX\r\n\r\n[Body]\r\nSkeleton = Adamantium\r\nClaws = Same\r\n\r\n[Claws]\r\nEnabled = Off\r\nKey = F3\r\n";
    const std::string armed = WolverineFileFor(wolverineFile, bone);
    CHECK(armed.find("Mode = Off\r\n") != std::string::npos && armed.find("Skeleton = Bone\r\n") != std::string::npos && armed.find("Claws = Bone\r\n") != std::string::npos);
    CHECK(armed.find("Enabled = On\r\nKey = F3\r\n") != std::string::npos && armed.find("; how fast\r\n") != std::string::npos && armed.rfind("; top\r\n", 0) == 0);
    CHECK(WolverineFileFor(armed, bone) == armed);                                     // writing again changes nothing
    CHECK(WolverineFileMatches(armed, bone));
    CHECK(!WolverineFileMatches(wolverineFile, bone));                                 // not written yet: a restart is due
    ClassSheet metalClaw = bone; metalClaw.metalClaws = true;
    CHECK(!WolverineFileMatches(armed, metalClaw));                                    // someone edited the material
    CHECK(!WolverineFileMatches(IniWithValue(armed, "Strength", "Enabled", "On"), bone));  // or switched on what classes refuse
    CHECK(WolverineFileMatches(IniWithValue(armed, "Claws", "Key", "T"), bone));       // their own key is theirs
    CHECK(IniWithValue("", "A", "b", "1") == "\n[A]\nb = 1\n");
    CHECK(IniWithValue("[A]\nc = 2\n", "A", "b", "1") == "[A]\nb = 1\nc = 2\n");
    CHECK(IniWithValue("[A]\nb = 2", "A", "b", "1") == "[A]\nb = 1");
    CHECK(IniWithValue("[B]\nb = 2\n[A]\nb = 3\n", "A", "b", "1") == "[B]\nb = 2\n[A]\nb = 1\n");
    CHECK(IniWithValue("[A]\n; b = 9\n", "A", "b", "1") == "[A]\nb = 1\n; b = 9\n");

    // The files' reading agrees with the same class in the mod's own form, so the launcher-era sheets still work.
    CHECK(std::abs(ClassPoints(std::string_view("[SpeedMultiplier]\nenabled = true\nrun_speed_multiplier = 1.25\nwalk_speed_multiplier = 1.0\n"), std::string_view("")) - 0.1875) < 1e-9);
    // Grappler and Panicker: priced, carried exactly, and refused when malformed, because a flaw that fell off in transit is free power.
    {
        ClassSheet g; g.grappler = true;
        ClassSheet p; p.panicker = true;
        CHECK(std::abs(ClassPoints(g) - 0.06) < 1e-9 && std::abs(ClassPoints(p) + 0.05) < 1e-9);
        CHECK(DecodeClass(EncodeClass(g)) == g && DecodeClass(EncodeClass(p)) == p);
        ClassSheet both = g; both.panicker = true;
        CHECK(DecodeClass(EncodeClass(both)) == both && EncodeClass(both).ends_with(" p11"));
        const std::string plainText = EncodeClass(ClassSheet{});
        CHECK(plainText.find(" p") == std::string::npos);   // a class with neither reads as it always has
        for (const char* bad : {" p00", " p2", " p1", " p111", " p12", " q10", " "}) CHECK(!DecodeClass(plainText + bad));
        ClassSheet step; ClassFieldStep(step, 18, 1); CHECK(step.grappler && ClassField(step, 18).value == "On");
        ClassFieldStep(step, 19, 1); CHECK(step.panicker && ClassField(step, 19).value == "On");
        ClassFieldStep(step, 18, -1); CHECK(!step.grappler);
        CHECK(ClassField(step, 17).value == "1.0" && ClassSummary(both).find("grappler") != std::string::npos && ClassSummary(both).find("panicker") != std::string::npos);
        // Only while holding, and never past the limit on Muscle Rate.
        CHECK(ClassHoldRate(1.0, g, false) == 1.0 && ClassHoldRate(1.0, g, true) == 1.5 && ClassHoldRate(0.2, g, true) == 1.5 && ClassHoldRate(2.2, g, true) == kMaxClassWeight);
        CHECK(ClassHoldRate(1.0, ClassSheet{}, true) == 1.0);
        // A plain class is exactly the lobby's build, so being lifted works as it did before classes moved here. The report: a base of 1.0 meant nobody could be picked up.
        CHECK(ClassWeight(0.3, ClassSheet{}) == 0.3);
        // Weight goes through the game's Mass Scale, which its muscles follow. The report: pelvis 1.406 with the class at x2.00, and a floaty body.
        ClassSheet heavy2; heavy2.mass = 2.0;
        CHECK(std::abs(ClassMassScale(1.0, heavy2) - 2.5) < 1e-9 && std::abs(ClassMassScale(0.3, ClassSheet{}) - 1.075) < 1e-9);
        // The power shown is one number everywhere, and over the limit can't read as on it. The report: +4 in the note and +3 beside it.
        CHECK(ClassPowerShown(0.035) == 4 && ClassPowerShown(0.15) == 15 && ClassPowerShown(0.1501) == 16 && ClassPowerShown(-0.025) == -2 && ClassPowerShown(0.0) == 0);
        ClassSheet over; over.knockback = 1.5004;
        CHECK(ClassBudget(over, false).over && ClassBudget(over, false).text == "POWER +16 OF +15  OVER BY 1");
        CHECK(ClassBudget(g, false).text == "POWER +6 OF +15, 9 LEFT" && ClassSummary(g).find("power +6") != std::string::npos);
        // UE4SS's reload: two changes two seconds apart are one reload, and the next waits out the gap. The report crashed on exactly this.
        ClassReloadGate gate;
        CHECK(!gate.Due(100, true));
        gate.Changed(1000); CHECK(!gate.Due(2000, true));
        gate.Changed(3000); CHECK(!gate.Due(4900, true) && !gate.Due(5000, false) && gate.Due(5000, true) && !gate.Due(5100, true));
        gate.Changed(6000); CHECK(!gate.Due(8000, true) && !gate.Due(19999, true) && gate.Due(20000, true));
        // The clock wraps every 49 days.
        ClassReloadGate wrap; wrap.Changed(0xFFFFFF00u); CHECK(!wrap.Due(0xFFFFFF10u, true) && wrap.Due(0x00001000u, true));
        // A guest's replica under host physics isn't theirs to change or to judge; a host's fighter and an owner-sync fighter are.
        CHECK(!ClassRunsOnMyFighter(true, false, false) && ClassRunsOnMyFighter(true, true, false) && ClassRunsOnMyFighter(false, false, false) && !ClassRunsOnMyFighter(false, false, true));
        // The host applies a guest's class only when it's a real, legal class in a Classes lobby. A guest who sends rubbish or a broken class is the plain fighter.
        ClassSheet guest; guest.grappler = true; guest.walk = 0.8;
        CHECK(GuestClassOnHost(EncodeClass(guest), true, false) == guest && !GuestClassOnHost(EncodeClass(guest), false, false) && !GuestClassOnHost("rubbish", true, false) && !GuestClassOnHost("", true, false));
        ClassSheet cheat; cheat.strength = 5.0;
        CHECK(!GuestClassOnHost(EncodeClass(cheat), true, false) && GuestClassOnHost(EncodeClass(cheat), true, true));
        ClassSheet biter; biter.bite = true; ClassSheet clawed; clawed.claws = true;
        CHECK(ClassLosesPartsOnGuest(biter) && ClassLosesPartsOnGuest(clawed) && !ClassLosesPartsOnGuest(guest));
        // Other players can tell classes apart. The report: nobody could see at a glance who was strong and who was frail.
        ClassSheet strong; strong.strength = 1.2; ClassSheet weak; weak.strength = 0.8; ClassSheet maxed; maxed.strength = 5.0; ClassSheet feeble; feeble.strength = 0.1;
        ClassSheet muscled2; muscled2.muscle = true; muscled2.strength = 0.7;
        CHECK(ClassLookMuscles(ClassSheet{}, true) == kClassLookPlain && std::abs(ClassLookMuscles(strong, true) - 0.8) < 1e-9 && std::abs(ClassLookMuscles(weak, true) - 0.2) < 1e-9);
        CHECK(ClassLookMuscles(maxed, true) == 1.0 && ClassLookMuscles(feeble, true) == 0.0 && ClassLookMuscles(muscled2, true) == 1.0);
        // Muscle only shows where it runs: a guest under host physics looks as strong as they are.
        CHECK(std::abs(ClassLookMuscles(muscled2, false) - 0.05) < 1e-9);
        // The look fills in for a forced build only. A real passport Weight, a role's build and a zombie's body keep the game's look.
        CHECK(ClassLookApplies(true, false, false) && !ClassLookApplies(false, false, false) && !ClassLookApplies(true, true, false) && !ClassLookApplies(true, false, true));
        // The tag says what the body can't: a plain class is only its name, and each stat shows once it differs.
        ClassSheet tagged; tagged.strength = 0.85; tagged.mass = 1.5; tagged.bones = 0.7;
        CHECK(ClassTagLine("Fencer", ClassSheet{}) == "Fencer" && ClassTagLine("Knight", tagged) == "Knight STR 85% WT 1.5 BONES 70%");
        ClassSheet onlyWeight; onlyWeight.mass = 0.5;
        CHECK(ClassTagLine("Custom", onlyWeight) == "Custom WT 0.5");
        ClassSheet nearlyPlain; nearlyPlain.mass = 1.04;   // a hand-edited class: the tag can't say it differs and then round that away
        CHECK(ClassTagLine("Custom", nearlyPlain) == "Custom WT 1.04");
        // Panicker only bites below half health, and on top of the class's own endurance.
        ClassSheet hurt; hurt.panicker = true; hurt.endurance = 1.5;
        CHECK(ClassEnduranceNow(hurt, 50.0) == 1.5 && ClassEnduranceNow(hurt, 49.0) == 1.0 && ClassEnduranceNow(ClassSheet{}, 1.0) == 1.0);
        CHECK(std::abs(ClassStaminaNext(50.0, 40.0, ClassEnduranceNow(p, 10.0)) - 35.0) < 1e-9);
    }
    if (g_fail == 0) std::printf("ok\n");
    return g_fail != 0;
}
