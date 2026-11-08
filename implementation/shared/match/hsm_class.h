// A class: what a fighter's stats are, what it costs, and how it travels. This is the mod's own class system, with no other mod
// needed for the stats. Half Wolverine's parts need that mod. Each machine checks every class with these compiled rules, so
// picking, forging or hand-editing a class can't get past them. Game-independent.
#pragma once
#include "hsm_name_filter.h"
#include <cctype>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hsm {

// Bite's key moved from H to B. An INI still on H moves once: the mod marks it bite_key_moved=1 the first time it reads it, so a
// player who picks H again afterwards keeps it.
inline bool BiteKeyMovesToB(std::string_view key, std::string_view moved) { return moved != "1" && (key == "H" || key == "h"); }

constexpr double kRuleStatMin = 0.10, kRuleMassMin = 0.50, kRuleMassMax = 5.00, kRuleMassStep = 0.50, kRuleStatMax = 2.00;   // every multiplier in force, 1.0 being the game's own
constexpr double kRuleEarnFloor = 0.70;                         // lowering a stat past this earns no more power
constexpr double kRulePairGap = 0.10;                           // run and walk: how far one's lowering pays past the other
constexpr double kRuleMaxPoints = 0.15;                         // what a class may be over budget
constexpr double kRuleJumpBase = 5000.0, kRuleJumpMin = 0.0, kRuleJumpMax = 6000.0;
// Unrestricted lobbies have no budget, but these still hold, for everyone in the lobby: a fighter far past them can send the physics out
// of control on other players' machines. Everyone checks them against what each player sends, as with the rules above.
constexpr double kHackerStatMax = 5.00, kHackerJumpMax = 15000.0;
constexpr size_t kMaxClassValue = 200;                          // what the lobby's member data carries

enum class ClassHeal : uint8_t { Off, Logan, Weathered };

struct ClassSheet {
    double run = 1, walk = 1, strength = 1, grip = 1, hands = 1, kick = 1, knockback = 1, bones = 1, mass = 1, jump = kRuleJumpBase;
    double recovery = 1, endurance = 1;     // the game's Regen Rate; how much of a drop in Stamina is felt
    bool noPain = false, quickKicks = false, bite = false, noDislocation = false;
    bool grappler = false, panicker = false;   // a perk and a flaw: what they do is below (ClassHoldRate, ClassEnduranceNow)
    // Half Wolverine's. They need the mod.
    ClassHeal heal = ClassHeal::Off;
    bool metalSkeleton = false, claws = false, metalClaws = false, muscle = false;
    bool operator==(const ClassSheet&) const = default;
};

inline bool ClassWolverinePartsDiffer(const ClassSheet& a, const ClassSheet& b) {
    return a.heal != b.heal || a.metalSkeleton != b.metalSkeleton || a.claws != b.claws || (a.claws && a.metalClaws != b.metalClaws) || a.muscle != b.muscle;
}
inline bool ClassNeedsWolverine(const ClassSheet& c) { return c.heal != ClassHeal::Off || c.metalSkeleton || c.claws || c.muscle; }

// A class's power budget: gains cost points, lowering a real stat earns them back down to kRuleEarnFloor, and flavour stats earn
// nothing. Without this, every stat at the maximum would pass. Adamantium claws cut through armour, so they take almost everything
// a class can earn (1.875, plus the allowance): nearly every stat at its floor.
inline double ClassPoints(const ClassSheet& c) {
    double points = 0;
    if (c.heal == ClassHeal::Logan) points += 0.50;
    else if (c.heal == ClassHeal::Weathered) points += 0.80;
    if (c.metalSkeleton) points += 0.80;
    if (c.claws) points += c.metalClaws ? 1.80 : 0.60;
    if (c.muscle) points += 0.40;
    if (c.noPain) points += 1.00;
    if (c.quickKicks) points += 0.25;
    if (c.bite) points += 0.05;
    if (c.grappler) points += 0.06;
    if (c.panicker) points -= 0.05;
    const auto earned = [](double v) { return 1.0 - std::max(v, kRuleEarnFloor); };
    const auto stat = [&](double v, double weight) {
        if (v > 1.0001) points += (v - 1.0) * weight;
        else if (v < 0.9999) points -= earned(v) * weight;
    };
    // Strength is the game's Muscle Rate (ClassWeight). Grip costs and earns nothing, because the game never reads the field it would set
    // (R/L Grab Force Limit): what holds a grab is Muscle Rate. Hands only matter once they pass the game's clamp, so lowering them earns nothing.
    stat(c.strength, 0.6); stat(c.kick, 0.25); stat(c.knockback, 0.3); stat(c.bones, 1.0); stat(c.mass, 0.6); stat(c.recovery, 0.3); stat(c.endurance, 0.25);
    if (c.hands > 1.0001) points += (c.hands - 1.0) * 0.15;
    // Most fighting happens at a walk, so walk is worth more than run. They pay as a pair: lowering one earns nothing while the
    // other is raised, and only kRulePairGap past where the other has come down.
    const auto speed = [&](double v, double other, double weight) {
        if (v > 1.0001) points += (v - 1.0) * weight;
        else if (v < 0.9999 && other <= 1.0001) points -= std::min(earned(v), earned(other) + kRulePairGap) * weight;
    };
    speed(c.run, c.walk, 0.75);
    speed(c.walk, c.run, 1.5);
    if (c.noDislocation) points += 0.20;
    if (c.jump > kRuleJumpBase + 1) points += (c.jump / kRuleJumpBase - 1.0) * 0.3;
    return points;
}

// Power as it's shown, the same everywhere: a fraction rounds up, so a class that's over the limit never reads as exactly on it.
inline int ClassPowerShown(double points) { return int(std::ceil(points * 100.0 - 1e-6)); }

// What's wrong with a class, in words for the player; empty: it may fight. Hosts run this on what each player sends.
inline std::vector<std::string> ClassProblems(const ClassSheet& c, bool unrestricted = false) {
    std::vector<std::string> out;
    const auto in = [](double v, double lo, double hi) { return std::isfinite(v) && v >= lo - 1e-6 && v <= hi + 1e-6; };
    const struct { const char* name; double v, lo; } stats[] = {{"run", c.run, kRuleStatMin}, {"walk", c.walk, kRuleStatMin}, {"strength", c.strength, kRuleStatMin},
        {"grip", c.grip, kRuleStatMin}, {"hands", c.hands, kRuleStatMin}, {"kick", c.kick, kRuleStatMin}, {"knockback", c.knockback, kRuleStatMin},
        {"bones", c.bones, kRuleStatMin}, {"weight", c.mass, kRuleMassMin}, {"recovery", c.recovery, kRuleStatMin}, {"endurance", c.endurance, kRuleStatMin}};
    const double statMax = unrestricted ? kHackerStatMax : kRuleStatMax;
    for (const auto& s : stats) if (!in(s.v, s.lo, s.lo == kRuleMassMin ? kRuleMassMax : statMax)) out.push_back(std::string(s.name) + " is out of range");
    if (!in(c.jump, kRuleJumpMin, unrestricted ? kHackerJumpMax : kRuleJumpMax)) out.push_back("jump is out of range");
    if (c.metalClaws && !c.claws) out.push_back("adamantium claws without claws");
    if (!unrestricted && ClassPoints(c) > kRuleMaxPoints + 1e-6) out.push_back("the class is too strong");
    return out;
}

// On the wire and in the player's file: short keys, three decimals. Anything unknown or malformed refuses the whole value, because
// what a player sends is untrusted, and a class that half-reads could pass as a weaker one.
inline std::string EncodeClass(const ClassSheet& c) {
    char b[kMaxClassValue + 40];
    const int n = std::snprintf(b, sizeof b, "r%.3f w%.3f s%.3f g%.3f h%.3f k%.3f n%.3f b%.3f m%.3f j%.0f c%.3f e%.3f f%d%d%d%d%d%d%d%d%d",
        c.run, c.walk, c.strength, c.grip, c.hands, c.kick, c.knockback, c.bones, c.mass, c.jump, c.recovery, c.endurance,
        c.noPain, c.quickKicks, c.bite, c.noDislocation, int(c.heal), c.metalSkeleton, c.claws, c.metalClaws, c.muscle);
    if (n <= 0 || size_t(n) >= sizeof b) return {};
    std::string out(b, size_t(n));
    if (c.grappler || c.panicker) { out += " p"; out += c.grappler ? '1' : '0'; out += c.panicker ? '1' : '0'; }   // only when there's one, so older values read the same
    return out;
}
inline std::optional<ClassSheet> DecodeClass(std::string_view s) {
    if (s.empty() || s.size() > kMaxClassValue) return std::nullopt;
    ClassSheet c;
    double* const slots[] = {&c.run, &c.walk, &c.strength, &c.grip, &c.hands, &c.kick, &c.knockback, &c.bones, &c.mass, &c.jump};
    constexpr std::string_view keys = "rwsghknbmj";
    size_t at = 0;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (at >= s.size() || s[at] != keys[i]) return std::nullopt;
        const size_t end = s.find(' ', at);
        if (end == std::string_view::npos) return std::nullopt;
        const std::string text(s.substr(at + 1, end - at - 1));
        if (text.empty() || text.size() > 12) return std::nullopt;
        char* stop = nullptr;
        const double v = std::strtod(text.c_str(), &stop);
        if (!stop || *stop || !std::isfinite(v)) return std::nullopt;
        *slots[i] = v;
        at = end + 1;
    }
    // Saved before these two existed: the game's own values.
    double* const later[] = {&c.recovery, &c.endurance};
    constexpr std::string_view laterKeys = "ce";
    for (size_t i = 0; i < laterKeys.size(); ++i) {
        if (at >= s.size() || s[at] != laterKeys[i]) continue;
        const size_t end = s.find(' ', at);
        if (end == std::string_view::npos) return std::nullopt;
        const std::string text(s.substr(at + 1, end - at - 1));
        if (text.empty() || text.size() > 12) return std::nullopt;
        char* stop = nullptr;
        const double v = std::strtod(text.c_str(), &stop);
        if (!stop || *stop || !std::isfinite(v)) return std::nullopt;
        *later[i] = v;
        at = end + 1;
    }
    std::string_view flags = s.substr(at);
    if (const size_t space = flags.find(' '); space != std::string_view::npos) {
        const std::string_view perks = flags.substr(space + 1);
        flags = flags.substr(0, space);
        // One way to write each class: "p00" isn't, because it's written without.
        if (perks.size() != 3 || perks[0] != 'p' || (perks[1] != '0' && perks[1] != '1') || (perks[2] != '0' && perks[2] != '1') || perks == "p00") return std::nullopt;
        c.grappler = perks[1] == '1'; c.panicker = perks[2] == '1';
    }
    if (flags.size() != 10 || flags[0] != 'f') return std::nullopt;
    const auto bit = [&](size_t i, bool& out) { if (flags[i] != '0' && flags[i] != '1') return false; out = flags[i] == '1'; return true; };
    if (!bit(1, c.noPain) || !bit(2, c.quickKicks) || !bit(3, c.bite) || !bit(4, c.noDislocation)) return std::nullopt;
    if (flags[5] < '0' || flags[5] > '2') return std::nullopt;
    c.heal = ClassHeal(flags[5] - '0');
    if (!bit(6, c.metalSkeleton) || !bit(7, c.claws) || !bit(8, c.metalClaws) || !bit(9, c.muscle)) return std::nullopt;
    return c;
}

// The classes everyone has.
constexpr int kClassTabPlain = 0, kClassTabWolverine = 1, kClassTabCustom = 2, kClassTabHacker = 3;
inline constexpr const char* kPlainClass = "Man-at-Arms";      // the game's own fighter
inline constexpr const char* kHseClassName = "HS Enhancer";    // what a player using it reads as: classes are off for them
struct NamedClass { const char* name; const char* note; ClassSheet sheet; int tab = kClassTabPlain; };
inline const std::vector<NamedClass>& BuiltInClasses() {
    const auto make = [](ClassHeal heal, bool pain, bool kicks, bool bite, bool disloc, double run, double walk, double str, double grip, double hands, double knock, double kick, double bones, double mass, double jump) {
        ClassSheet c;
        c.heal = heal; c.noPain = pain; c.quickKicks = kicks; c.bite = bite; c.noDislocation = disloc;
        c.run = run; c.walk = walk; c.strength = str; c.grip = grip; c.hands = hands; c.knockback = knock; c.kick = kick; c.bones = bones; c.mass = mass; c.jump = jump;
        return c;
    };
    const auto withParts = [](ClassSheet c, bool metalSkeleton, bool claws, bool metalClaws, bool muscle) {
        c.metalSkeleton = metalSkeleton; c.claws = claws; c.metalClaws = metalClaws; c.muscle = muscle;
        return c;
    };
    const auto withStamina = [](ClassSheet c, double recovery, double endurance) {
        c.recovery = recovery; c.endurance = endurance;
        return c;
    };
    static const std::vector<NamedClass> all = {
        // No mods. The usual party, in the game's own terms: the all-rounder, the damage dealers, the tank and the bruisers.
        {kPlainClass, "The all-rounder: the game's own fighter, with no edge and no weakness.", ClassSheet{}},
        {"Revenant", "Quick and light, and heals slowly.", make(ClassHeal::Logan, false, false, false, false, 1.15, 1.15, 0.80, 1.00, 1.00, 0.80, 0.80, 0.70, 0.70, 5000), kClassTabWolverine},
        {"Barber-Surgeon", "The healer: weak and slow, but steady healing and sturdy bones.", make(ClassHeal::Weathered, false, false, false, false, 0.80, 0.80, 0.70, 1.00, 1.00, 0.70, 0.70, 1.05, 1.00, 5000), kClassTabWolverine},
        {"Crusader", "Strong and slow, and heals slowly.", make(ClassHeal::Logan, false, false, false, false, 0.75, 0.80, 1.25, 1.00, 0.85, 0.80, 0.80, 0.85, 1.00, 4000), kClassTabWolverine},
        {"Pugilist", "The bruiser: fists, kicks and teeth, and a poor hand with a blade.", make(ClassHeal::Off, false, true, true, false, 0.90, 0.90, 0.75, 1.60, 1.60, 1.25, 1.50, 0.80, 0.80, 5000)},
        {"Footpad", "The rogue: very fast and very fragile. Strike and be gone.", make(ClassHeal::Off, false, true, false, false, 1.50, 1.15, 0.80, 1.00, 1.00, 0.70, 0.85, 0.70, 0.70, 5000)},
        {"Knight", "The tank: slow, heavy and hard to break, and joints that stay in.", make(ClassHeal::Off, false, false, false, true, 0.70, 0.70, 0.85, 1.00, 1.00, 0.85, 0.70, 1.50, 1.50, 4000)},
        {"Flagellant", "Feels no pain and hits hard, but slow and easily broken.", make(ClassHeal::Off, true, false, false, false, 0.70, 0.70, 1.10, 0.70, 0.70, 1.30, 1.30, 0.70, 0.70, 5000)},
        {"Landsknecht", "The heavy hitter: big swings at a slow march.", make(ClassHeal::Off, false, false, false, false, 0.75, 0.80, 1.25, 1.00, 1.00, 1.00, 1.00, 0.90, 1.00, 5000)},
        {"Fencer", "Quick feet and a light frame, with less behind each cut.", make(ClassHeal::Off, false, false, false, false, 1.10, 1.10, 0.85, 1.00, 1.00, 1.00, 1.00, 1.00, 0.85, 5000)},
        {"Wrestler", "Strong hands on a slow body, and joints that stay in: lifts and holds people.", make(ClassHeal::Off, false, false, false, true, 0.80, 0.80, 1.60, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 5000)},
        {"Pilgrim", "Gets back up: recovers fast and lasts all day, but hits lightly.", withStamina(make(ClassHeal::Off, false, false, false, false, 1.00, 1.00, 0.70, 1.00, 1.00, 1.00, 0.80, 1.00, 0.85, 5000), 2.00, 1.50)},
        {"Veteran", "Never seems to tire, and shrugs off a beating. Nothing else about him stands out.", withStamina(make(ClassHeal::Off, false, false, false, false, 1.00, 1.00, 1.00, 1.00, 1.00, 0.80, 0.80, 0.90, 1.00, 5000), 1.30, 2.00)},
        // Half Wolverine's.
        {"Verity", "Huge and slow, with the strongest grip there is, and joints that stay in.", withParts(make(ClassHeal::Off, false, false, false, true, 0.80, 0.80, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 5000), false, false, false, true), kClassTabWolverine},
        {"Wulf", "Bone claws and a light frame. Sheathe them to hold a weapon.", withParts(make(ClassHeal::Off, false, false, false, false, 1.00, 1.00, 0.80, 1.00, 1.00, 1.00, 0.70, 0.80, 0.85, 5000), false, true, false, false), kClassTabWolverine},
        {"Ironside", "An adamantium skeleton, so nothing can be cut off. Very slow and weak to pay for it.", withParts(make(ClassHeal::Off, false, false, false, false, 0.75, 0.75, 0.70, 1.00, 1.00, 1.00, 1.00, 1.00, 1.00, 5000), true, false, false, false), kClassTabWolverine},
        {"Old Scar", "Heals slowly and has bone claws, but is frail and a little slow.", withParts(make(ClassHeal::Logan, false, false, false, false, 0.90, 0.90, 0.70, 1.00, 1.00, 0.70, 0.70, 0.70, 0.70, 5000), false, true, false, false), kClassTabWolverine},
        // Unrestricted lobbies only.
        {"Glitch", "Five times the speed, kicks and knockback, and a huge jump.", make(ClassHeal::Off, false, true, false, false, 5.00, 5.00, 1.00, 1.00, 1.00, 5.00, 5.00, 1.00, 1.00, 15000), kClassTabHacker},
        {"Juggernaut", "Three times the weight and five times the strength, and bones to match.", make(ClassHeal::Off, true, false, false, true, 1.00, 1.00, 5.00, 5.00, 5.00, 5.00, 5.00, 5.00, 3.00, 5000), kClassTabHacker},
        {"Immortal", "Wolverine's fastest healing, an adamantium skeleton and adamantium claws, at twice the speed.", withParts(make(ClassHeal::Weathered, true, true, true, true, 2.00, 2.00, 2.00, 2.00, 2.00, 2.00, 2.00, 2.00, 1.00, 5000), true, true, true, true), kClassTabHacker},
    };
    return all;
}

inline const NamedClass* FindClassNamed(std::string_view name) {
    for (const NamedClass& c : BuiltInClasses()) if (name == c.name) return &c;
    return nullptr;
}
// The player's own classes. A slot is always known by its id (class=Custom 1), and may carry a name the player chose for what others read.
// A slot with nothing saved in it isn't offered: NEW CLASS fills the first free one, and DELETE empties it again.
constexpr int kCustomClassSlots = 8;
inline std::string CustomClassName(int slot) { return "Custom " + std::to_string(slot + 1); }
inline int CustomClassSlot(std::string_view name) {
    for (int i = 0; i < kCustomClassSlots; ++i) if (name == CustomClassName(i)) return i;
    return -1;
}
constexpr size_t kMaxClassName = 20;
inline bool SameNameAnyCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
// Why a name can't be used for a class; empty when it can. Plain letters only, so nothing can look like something it isn't.
inline std::string ClassNameProblem(std::string_view name) {
    if (name.empty() || name.front() == ' ' || name.back() == ' ') return "Give it a name with no spaces at the ends.";
    if (name.size() > kMaxClassName) return "That name is too long: " + std::to_string(kMaxClassName) + " characters at most.";
    bool letter = false;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= 0x80 || !(std::isalnum(u) || c == ' ' || c == '-' || c == '_' || c == '\'')) return "Use only letters, numbers, spaces, - _ and '.";
        letter = letter || std::isalpha(u);
    }
    if (!letter) return "A name needs at least one letter.";
    for (const NamedClass& c : BuiltInClasses()) if (SameNameAnyCase(name, c.name)) return "That's the name of a built-in class.";
    for (int i = 0; i < kCustomClassSlots; ++i) if (SameNameAnyCase(name, CustomClassName(i))) return "That name is reserved.";
    if (SameNameAnyCase(name, kHseClassName)) return "That name is reserved.";
    if (NameHasSlur(name)) return "That name isn't allowed.";
    return {};
}
// What a slot reads as. The file can be edited by hand, so a saved name is checked every time.
inline std::string CustomClassLabel(int slot, std::string_view saved) { return ClassNameProblem(saved).empty() ? std::string(saved) : CustomClassName(slot); }
// What a class is called on screen; `names` are the saved slot names. Anything but a slot id is its own name.
inline std::string ClassLabel(std::string_view id, const std::string* names) {
    const int slot = CustomClassSlot(id);
    return slot < 0 ? std::string(id) : CustomClassLabel(slot, names[slot]);
}
// What another player's class is called here. Their copy of the mod sends the name, so it gets the same check, and a name that claims
// to be a built-in has to be that class: the rules go by the class they report, and the name can't say otherwise.
inline std::string ClassNameFromPeer(std::string_view name, const ClassSheet& sheet) {
    if (name.empty()) name = kPlainClass;
    if (SameNameAnyCase(name, kHseClassName)) return sheet == ClassSheet{} ? std::string(kHseClassName) : std::string("Custom");   // classes are off for them: the plain sheet
    for (const NamedClass& c : BuiltInClasses()) if (SameNameAnyCase(name, c.name)) return c.sheet == sheet ? std::string(c.name) : std::string("Custom");
    for (int i = 0; i < kCustomClassSlots; ++i) if (SameNameAnyCase(name, CustomClassName(i))) return CustomClassName(i);
    return ClassNameProblem(name).empty() ? std::string(name) : std::string("Custom");
}
// Every class the picker offers, built-ins then the player's own.
inline int ClassPickCount() { return int(BuiltInClasses().size()) + kCustomClassSlots; }
inline std::string ClassPickName(int i) {
    const auto& all = BuiltInClasses();
    return i < int(all.size()) ? std::string(all[size_t(i)].name) : CustomClassName(i - int(all.size()));
}
// The CLASSES tab's dropdown. A mod that gets support later adds a line here and lists its classes under it: its options go on the Custom
// tab's rows too, greyed unless the mod is on.
struct ClassTab { const char* title; const char* needsMod; const char* modName; bool unrestrictedOnly; };
inline const std::vector<ClassTab>& ClassTabs() {
    static const std::vector<ClassTab> all = {
        {"CLASSES", nullptr, nullptr, false},
        {"WOLVERINE", "HalfWolverine", "Half Wolverine", false},
        {"CUSTOM", nullptr, nullptr, false},
        {"HACKER", nullptr, nullptr, true},
    };
    return all;
}
inline std::vector<std::string> ClassTabPicks(int tab) {
    std::vector<std::string> out;
    for (const NamedClass& c : BuiltInClasses()) if (c.tab == tab) out.emplace_back(c.name);
    if (tab == kClassTabCustom) for (int i = 0; i < kCustomClassSlots; ++i) out.push_back(CustomClassName(i));
    return out;
}
inline bool IsHackerClass(std::string_view name) {
    const NamedClass* c = FindClassNamed(name);
    return c && c->tab == kClassTabHacker;
}
// A class only fights if Half Wolverine is on exactly when the class uses it, so the screen only offers those. A player's own classes can
// always be opened, so what's saved in them can be changed. A tab that can't be used says why.
inline bool ClassPickable(std::string_view name, bool wolverine) {
    const NamedClass* c = FindClassNamed(name);
    return !c || ClassNeedsWolverine(c->sheet) == wolverine;
}
inline bool ClassTabUsable(int tab, bool wolverine) {
    if (tab == kClassTabPlain) return !wolverine;
    if (tab == kClassTabWolverine) return wolverine;
    return true;
}
inline int ClassHomeTab(bool wolverine) { return wolverine ? kClassTabWolverine : kClassTabPlain; }
inline const char* ClassTabRefusal(int tab, bool wolverine) {
    if (tab == kClassTabPlain && wolverine) return "Half Wolverine is on, and these classes are for fighters without it. Turn it off and restart to use them.";
    if (tab == kClassTabWolverine && !wolverine) return "These classes need Half Wolverine. Install it and turn it on to use them.";
    return "";
}
// The class to fall back to when the picked one can't fight with the mods that are on.
inline std::string ClassHomeClass(bool wolverine) { return ClassTabPicks(ClassHomeTab(wolverine))[0]; }
// Where a pick has to move to; nothing when it can stay. Someone HS Enhancer has taken off classes keeps theirs for when it's off.
inline std::optional<std::string> ClassFallback(std::string_view picked, bool wolverine, bool classesOff) {
    if (classesOff || ClassPickable(picked, wolverine)) return std::nullopt;
    return ClassHomeClass(wolverine);
}
// What Strength and Muscle do: the game's Muscle Rate, which sets the mass, the grip's hold limit and how much armour a body carries. The
// lobby's fair build is the base, so a player who hasn't paid for more gets exactly that.
constexpr double kWolverineMuscleRate = 2.2, kMaxClassWeight = 2.5;
inline double ClassWeight(double base, const ClassSheet& c) {
    double w = std::clamp(base + (c.strength - 1.0), 0.0, kMaxClassWeight);
    if (c.muscle) w = std::max(w, kWolverineMuscleRate);
    return w;
}
// The game's Mass Scale field for a class: what the Muscle Rate gives (Willie_BP's own 1 to 1.25 per 1.0), times the class's Weight. It has to go
// through this field: the game's muscles are scaled by it (Control Physical Animation Str), and it rebuilds each bone's mass from it when armour
// or a weapon changes (Set Up Armor). Weight put on the bones alone left muscles too weak for the body, and the next armour change undid it.
inline double ClassMassScale(double rate, const ClassSheet& c) { return (1.0 + 0.25 * rate) * c.mass; }
// Grappler, on the holder's machine: a stronger grip while holding a fighter. The body's mass stays what it was, so this does nothing for the
// rest of a fight. What carries the held fighter is on their own machine (sync/hsm_grab.h GrabCarried).
inline double ClassHoldRate(double rate, const ClassSheet& c, bool holding) {
    return c.grappler && holding ? std::min(std::max(rate, 1.0) + 0.5, kMaxClassWeight) : rate;
}
// Panicker: below half health, stamina drops are felt half as much again. Health is 0 to 100 in the game.
constexpr double kPanickerHealth = 50.0, kPanickerDrain = 1.5;
inline double ClassEnduranceNow(const ClassSheet& c, double health) { return c.panicker && health < kPanickerHealth ? c.endurance / kPanickerDrain : c.endurance; }
// Stamina over one step: above 100% only part of each drop is felt, below it more is. Only drops change, never what's gained back.
inline double ClassStaminaNext(double last, double now, double endurance) {
    if (!(now < last) || !(endurance > 0.0)) return now;
    return std::max(0.0, last - (last - now) / endurance);
}
// The next class in the picker's order; an unknown name starts from the first.
inline std::string ClassNameStep(std::string_view current, int dir) {
    const int n = ClassPickCount();
    const auto nameAt = [](int i) { return ClassPickName(i); };
    int at = 0;
    for (int i = 0; i < n; ++i) if (current == nameAt(i)) at = i;
    return nameAt(((at + dir) % n + n) % n);
}

// A class by name: a built-in, or a slot and what's saved in it. A slot that's empty or doesn't read is the plain fighter.
inline std::optional<ClassSheet> ResolveClass(std::string_view name, const std::string* customs) {
    if (const NamedClass* c = FindClassNamed(name)) return c->sheet;
    const int slot = CustomClassSlot(name);
    if (slot < 0) return std::nullopt;
    if (const auto d = DecodeClass(customs[slot])) return d;
    return ClassSheet{};
}

// What a class does, for the class checker: only what differs from the plain fighter, then its power. Everyone builds this from the
// class the player reports, so it can't say anything other than what the rules were checked against.
inline std::string ClassSummary(const ClassSheet& c) {
    std::string out;
    const auto add = [&](const std::string& part) { out += (out.empty() ? "" : ", ") + part; };
    const auto stat = [&](const char* name, double v) { if (std::abs(v - 1.0) > 1e-3) add(std::string(name) + " " + std::to_string(int(std::lround(v * 100))) + "%"); };
    if (std::abs(c.mass - 1.0) > 1e-3) { char w[32]; std::snprintf(w, sizeof w, "weight %.1f", c.mass); add(w); }
    if (c.heal == ClassHeal::Logan) add("heals: Logan"); else if (c.heal == ClassHeal::Weathered) add("heals: Weathered");
    if (c.metalSkeleton) add("adamantium skeleton");
    if (c.claws) add(c.metalClaws ? "ADAMANTIUM CLAWS" : "bone claws");
    if (c.muscle) add("muscle");
    if (c.noPain) add("no pain");
    if (c.quickKicks) add("quick kicks");
    if (c.bite) add("bite");
    if (c.noDislocation) add("no dislocation");
    if (c.grappler) add("grappler");
    if (c.panicker) add("panicker");
    stat("run", c.run); stat("walk", c.walk); stat("strength", c.strength); stat("hands", c.hands);
    stat("knockback", c.knockback); stat("kick", c.kick); stat("bones", c.bones); stat("recovery", c.recovery); stat("endurance", c.endurance);
    add("power " + std::string(ClassPowerShown(ClassPoints(c)) > 0 ? "+" : "") + std::to_string(ClassPowerShown(ClassPoints(c))));
    return out;
}

// The Forge's rows. Stats move in 5% steps and stay inside what the rules allow, so the rows can't make a class the host refuses
// except by being too strong, and the points line says that as it happens.
constexpr int kClassFieldCount = 20;
struct ClassFieldView { const char* label; std::string value; };
inline ClassFieldView ClassField(const ClassSheet& c, int i) {
    const auto pct = [](double v) { return std::to_string(int(std::lround(v * 100))) + "%"; };
    const auto yes = [](bool b) { return std::string(b ? "On" : "Off"); };
    switch (i) {
        case 0: return {"HEALING (WOLVERINE)", c.heal == ClassHeal::Logan ? "Logan" : c.heal == ClassHeal::Weathered ? "Weathered" : "Off"};
        case 1: return {"SKELETON (WOLVERINE)", c.metalSkeleton ? "Adamantium" : "Bone"};
        case 2: return {"CLAWS (WOLVERINE)", !c.claws ? "Off" : c.metalClaws ? "Adamantium" : "Bone"};
        case 3: return {"MUSCLE (WOLVERINE)", yes(c.muscle)};
        case 4: return {"FEEL NO PAIN", yes(c.noPain)};
        case 5: return {"QUICK KICKS", yes(c.quickKicks)};
        case 6: return {"BITE", yes(c.bite)};
        case 7: return {"NO DISLOCATION", yes(c.noDislocation)};
        case 8: return {"RUN", pct(c.run)};
        case 9: return {"WALK", pct(c.walk)};
        case 10: return {"STRENGTH", pct(c.strength)};
        case 11: return {"RECOVERY", pct(c.recovery)};
        case 12: return {"ENDURANCE", pct(c.endurance)};
        case 13: return {"HANDS", pct(c.hands)};
        case 14: return {"KNOCKBACK", pct(c.knockback)};
        case 15: return {"KICK", pct(c.kick)};
        case 16: return {"BONES", pct(c.bones)};
        case 17: { char w[16]; std::snprintf(w, sizeof w, "%.1f", c.mass); return {"WEIGHT", w}; }   // a real number, not a share: 1.0 is the game's own
        case 18: return {"PERK: GRAPPLER", yes(c.grappler)};
        default: return {"FLAW: PANICKER", yes(c.panicker)};
    }
}
// What a row does, for the CLASSES tab's description box.
inline const char* ClassFieldInfo(int i) {
    switch (i) {
        case 0: return "Closes your wounds over time, from Half Wolverine. Off is the base game. Logan is slow and Weathered is faster. Any healing also protects you from pain, dismemberment and dislocation, so it costs a lot: 50 power for Logan, 80 for Weathered.";
        case 1: return "What your bones are made of, from Half Wolverine. Bone is the game's own bones and costs nothing. Adamantium is metal, so nothing can be cut off you, for 80 power.";
        case 2: return "Blades you draw from your hands, from Half Wolverine. Fighters can't hold weapons with claws out, so sheathe them to use one. Bone claws cost 60 power and armour stops them. Adamantium claws cut through armour, for 180 power: nearly every stat has to be at its lowest to afford them.";
        case 3: return "A bigger, more muscular build in fights, from Half Wolverine, and the strongest grip and lift there is. Costs 40 power.";
        case 4: return "You ignore pain and its effects, so hits don't slow you down. Very strong: it costs 100 power, so you have to give up a lot for it.";
        case 5: return "Removes the wait between kicks, so you can kick again straight away. Costs 25 power.";
        case 6: return "Adds a bite attack on your bite key, B unless you've changed it. Press once to bite and again to let go. Costs 5 power.";
        case 7: return "Your joints can't be dislocated. Costs 20 power.";
        case 8: return "How fast you run. Most fighting happens at a walk, so this costs and earns less than WALK. Lowering it earns nothing while WALK is raised, and pays fully only when WALK comes down too.";
        case 9: return "How fast you walk, your main speed in a fight, so this costs and earns more than RUN. Lowering it earns nothing while RUN is raised, and pays fully only when RUN comes down too.";
        case 10: return "How strong your body is: it's what lets you lift and hold people, carry armour without slowing, and it makes you heavier. Costs 60 power per 100%, and earns power when lowered, down to 70%.";
        case 11: return "How fast your body recovers by itself: health, consciousness and pain. No mod needed. Costs 30 power per 100%, and earns power when lowered, down to 70%.";
        case 12: return "How long your stamina lasts. Above 100% you tire more slowly. Costs 25 power per 100%, and earns power when lowered, down to 70%.";
        case 13: return "How rigid your hands are, which makes your punches hit harder. It only starts to matter well above 100% without gauntlets. Costs 15 power per 100%. Lowering it earns nothing.";
        case 14: return "How far your blows push and throw people. Earns power when lowered, down to 70%.";
        case 15: return "How much force your kicks have. Earns power when lowered, down to 70%.";
        case 16: return "How much force it takes to break your bones. Lower breaks easier. Earns power when lowered, down to 70%.";
        case 17: return "How heavy your body is, in half steps from 0.5 to 5.0, where 1.0 is the game's own. Heavier is harder to push and throw. Costs 60 power per 1.0, and earns power when lowered, down to 0.7.";
        case 18: return "You carry anyone you hold the way the game carries an NPC, whatever their weight or armour, so you can lift and throw them. They also have a harder time breaking free. Does nothing else. Costs 6 power.";
        default: return "Below half health you panic: your stamina drains half as fast again. Earns 5 power.";
    }
}
// The budget, where it's seen beside the bars. Over the limit says by how much, so the player knows what to give up.
struct ClassBudgetView { std::string text; bool over; };
inline ClassBudgetView ClassBudget(const ClassSheet& c, bool unrestricted) {
    const int used = ClassPowerShown(ClassPoints(c)), limit = ClassPowerShown(kRuleMaxPoints);
    if (unrestricted) return {"POWER " + std::string(used > 0 ? "+" : "") + std::to_string(used) + "  (Unrestricted: no limit)", false};
    const bool over = ClassPoints(c) > kRuleMaxPoints + 1e-6;
    return {"POWER " + std::string(used > 0 ? "+" : "") + std::to_string(used) + " OF +" + std::to_string(limit) + (over ? "  OVER BY " + std::to_string(used - limit) : ", " + std::to_string(limit - used) + " LEFT"), over};
}

// The CLASSES tab's three bars, 0 to 1 with the plain fighter in the middle and the 200% limit at the end.
struct ClassBars { float attack, speed, toughness; };
inline ClassBars ClassBarsFor(const ClassSheet& c) {
    const auto bar = [](double ratio) { return float(std::clamp(0.5 + (ratio - 1.0) * (ratio > 1.0 ? 0.5 : 1.0), 0.0, 1.0)); };
    return {bar((1.25 * c.strength + 0.3 * c.hands + 0.25 * c.kick + 0.3 * c.knockback) / 2.1), bar((c.run + c.walk) / 2), bar((c.bones + c.mass) / 2)};
}
// The Classes tab is for no mods, so it doesn't show Half Wolverine's rows at all.
inline bool ClassFieldShown(int i, int tab) { return i >= 4 || tab != kClassTabPlain; }
// Half Wolverine's rows can't be turned on without the mod. One that's already on can still be turned off.
inline bool ClassFieldLocked(const ClassSheet& c, int i, bool wolverine) {
    if (wolverine) return false;
    return (i == 0 && c.heal == ClassHeal::Off) || (i == 1 && !c.metalSkeleton) || (i == 2 && !c.claws) || (i == 3 && !c.muscle);
}
// The hacker tab's bars: the same value, shaking and spiking, and the same for a given frame.
inline float GlitchedBar(float value, uint32_t frame) {
    uint32_t h = (frame / 4) * 2654435761u + uint32_t(value * 1000.f);
    h ^= h >> 13; h *= 1274126177u; h ^= h >> 16;
    if (h % 9 == 0) return (h >> 8) % 2 ? 1.f : 0.05f;
    return std::clamp(value + (float(h % 1000) / 1000.f - 0.5f) * 0.30f, 0.f, 1.f);
}
inline void ClassFieldStep(ClassSheet& c, int i, int dir, bool unrestricted = false) {
    const double hi = unrestricted ? kHackerStatMax : kRuleStatMax;
    // Past 200%, where only Unrestricted goes, the steps are bigger: there's a long way to travel.
    const auto stepTo = [dir](double& v, double lo, double top) {
        const double by = v > 2.0 || (v == 2.0 && dir > 0) ? 0.25 : 0.05;
        v = std::clamp(std::round((v + dir * by) * 100.0) / 100.0, lo, top);
    };
    const auto step = [&](double& v, double lo) { stepTo(v, lo, hi); };
    switch (i) {
        case 0: c.heal = ClassHeal((int(c.heal) + (dir > 0 ? 1 : 2)) % 3); break;
        case 1: c.metalSkeleton = !c.metalSkeleton; break;
        case 2: {
            const int at = !c.claws ? 0 : c.metalClaws ? 2 : 1, to = (at + (dir > 0 ? 1 : 2)) % 3;
            c.claws = to != 0; c.metalClaws = to == 2;
            break;
        }
        case 3: c.muscle = !c.muscle; break;
        case 4: c.noPain = !c.noPain; break;
        case 5: c.quickKicks = !c.quickKicks; break;
        case 6: c.bite = !c.bite; break;
        case 7: c.noDislocation = !c.noDislocation; break;
        case 8: step(c.run, kRuleStatMin); break;
        case 9: step(c.walk, kRuleStatMin); break;
        case 10: step(c.strength, kRuleStatMin); break;
        case 11: step(c.recovery, kRuleStatMin); break;
        case 12: step(c.endurance, kRuleStatMin); break;
        case 13: step(c.hands, kRuleStatMin); break;
        case 14: step(c.knockback, kRuleStatMin); break;
        case 15: step(c.kick, kRuleStatMin); break;
        case 16: step(c.bones, kRuleStatMin); break;
        case 18: c.grappler = !c.grappler; break;
        case 19: c.panicker = !c.panicker; break;
        default: c.mass = std::clamp(std::round((c.mass + dir * kRuleMassStep) / kRuleMassStep) * kRuleMassStep, kRuleMassMin, kRuleMassMax); break;
    }
}

// When to ask UE4SS to reload its mods after a class change. Its reload runs on its own time and a second one landing on the first crashes
// the game (the report: two asked for two seconds apart), so it waits for the player to stop changing things, and leaves a long gap between.
constexpr uint32_t kReloadSettleMs = 2000, kReloadGapMs = 15000;
struct ClassReloadGate {
    bool pending = false, fired = false;
    uint32_t changedAt = 0, firedAt = 0;
    void Changed(uint32_t now) { pending = true; changedAt = now; }
    bool Due(uint32_t now, bool canSend) {
        if (!pending || !canSend || now - changedAt < kReloadSettleMs || (fired && now - firedAt < kReloadGapMs)) return false;
        pending = false; fired = true; firedAt = now;
        return true;
    }
};

// Whether this machine's own fighter is really simulated here. Under host physics a guest's is a replica of the host's, with its stats zeroed, so a
// class can't be applied to it and what it reads says nothing about cheating. The report: "strength 0.000" removed a tester from the lobby.
inline bool ClassRunsOnMyFighter(bool hostPhysics, bool isHost, bool handedToAnother) { return (!hostPhysics || isHost) && !handedToAnother; }
// Under host physics the host simulates every guest's real fighter, so it applies their classes: the same stats, from what each guest reports. What a
// guest's class can't do from there (Half Wolverine's parts and Bite, which run on their own machine) is said to them. Empty: the plain fighter.
inline std::optional<ClassSheet> GuestClassOnHost(std::string_view reported, bool classesLobby, bool unrestricted) {
    if (!classesLobby) return std::nullopt;
    const std::optional<ClassSheet> sheet = DecodeClass(reported);
    if (!sheet || !ClassProblems(*sheet, unrestricted).empty()) return std::nullopt;   // what a player sends is untrusted
    return sheet;
}
inline bool ClassLosesPartsOnGuest(const ClassSheet& c) { return ClassNeedsWolverine(c) || c.bite; }
// How a fighter's class shows on their body, on every machine: the game's 'Muscles' body material value, 0..1. The plain fighter in the middle, Strength
// to either side, Half Wolverine's Muscle at the top where their parts run (partsRun): under host physics a guest's don't (ClassLosesPartsOnGuest).
constexpr double kClassLookPlain = 0.5, kClassLookSpread = 1.5;
inline double ClassLookMuscles(const ClassSheet& c, bool partsRun) {
    return c.muscle && partsRun ? 1.0 : std::clamp(kClassLookPlain + (c.strength - 1.0) * kClassLookSpread, 0.0, 1.0);
}
// Whether that look goes on a body. Only under the lobby's forced build, where every body would look the same: without it the game's look shows the
// fighter's real Weight, a role's build has its own, and a zombie's body has its own.
inline bool ClassLookApplies(bool forcedBuild, bool roleBuild, bool zombie) { return forcedBuild && !roleBuild && !zombie; }
// The line under a fighter's name tag: their class, then what decides a grapple or a blow. Weight and Bones have no look in the game at all.
inline std::string ClassTagLine(std::string_view name, const ClassSheet& c) {
    std::string out(name);
    const auto percent = [](double v) { return std::to_string(int(std::lround(v * 100))) + "%"; };
    const auto differs = [](double v) { return std::abs(v - 1.0) > 1e-3; };
    if (differs(c.strength)) out += " STR " + percent(c.strength);
    if (differs(c.mass)) { char w[32]; std::snprintf(w, sizeof w, " WT %.3g", c.mass); out += w; }
    if (differs(c.bones)) out += " BONES " + percent(c.bones);
    return out;
}
// Where a class is in force: fighting alone, or in a lobby whose rule is Classes. Anywhere else it would be an unfair edge.
inline bool ClassAppliesHere(bool inLobby, bool classesLobby) { return !inLobby || classesLobby; }

// The game's own values for two stats its Blueprints only ever read (Willie_BP's defaults). Nothing in the game changes them, so
// a fighter who has anything else there, class aside, had them changed by something else. The class's multiplier goes on top.
constexpr double kGameMusclePower = 35.0, kGameRunRate = 2.0;
inline bool ClassBasesFair(double strengthBase, double runBase) { return std::abs(strengthBase - kGameMusclePower) < 1e-3 && std::abs(runBase - kGameRunRate) < 1e-3; }

// One value on the fighter. The game can change it (a new life, armour), so whatever differs from what was last written becomes
// the new base, and the multiplier goes on top of that. Never compounds, and 1.0 leaves the game's own value alone.
struct ClassStatTrack {
    double base = 0, written = 0;
    bool seen = false;
    double Next(double current, double multiplier) {
        if (!seen || std::abs(current - written) > 1e-5) { base = current; seen = true; }
        written = base * multiplier;
        return written;
    }
};

}  // namespace hsm
