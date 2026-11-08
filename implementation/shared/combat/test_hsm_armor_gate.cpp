// ArmorGate: a stuck blade is refused only where the victim's own DCD said its armour took the whole cut, near that
// blow's point, on the same bone, from the same attacker and hand; everything else is replayed as before. Each lodged
// blade is decided once: a later contact does not pull a blade out, and time does not let a held one in.
#include "combat/hsm_armor_gate.h"
#include <cmath>
#include <cstdio>
#include <limits>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::combat;

static constexpr double kT = 5000.0;

static HitClaim Blow(double cut = 300.0, const char* bone = "spine_03", StrikeSource src = StrikeSource::WeaponRight) {
    HitClaim c;
    c.attacker = 2; c.victim = 1; c.hitBone = bone; c.source = src; c.cuttingPower = cut;
    c.location = {10, 0, 0};
    return c;
}
static HitClaim Stuck(const char* bone = "spine_03", StrikeSource src = StrikeSource::WeaponRight, double x = 12) {
    HitClaim c = Blow(300.0, bone, src);
    c.striker = StrikerKind::Stuck;
    c.location = {x, 0, 0};
    return c;
}

static void TestHeldBlowRefusesItsStuckBlade() {
    ArmorGate g;
    g.NoteBlow(Blow(), 0.0, 1000.0, kT);   // armour took the whole cut
    CHECK(g.StuckHeld(Stuck(), kT + 100));
}

static void TestCutBlowLetsItThrough() {
    ArmorGate g;
    g.NoteBlow(Blow(), 42.0, 1000.0, kT);
    CHECK(!g.StuckHeld(Stuck(), kT + 100));
}

static void TestNoVerdictIsReplayed() {
    ArmorGate g;
    CHECK(!g.StuckHeld(Stuck(), kT));                                              // nothing seen: as before
    ArmorGate h;
    h.NoteBlow(Blow(), 0.0, 1000.0, kT);
    CHECK(!h.StuckHeld(Stuck("head"), kT + 100));                                  // another bone
    CHECK(!h.StuckHeld(Stuck("spine_03", StrikeSource::WeaponLeft), kT + 100));    // the other hand
    CHECK(!h.StuckHeld(Stuck("spine_03", StrikeSource::Ammo), kT + 100));          // a bolt
    HitClaim other = Stuck(); other.attacker = 3;
    CHECK(!h.StuckHeld(other, kT + 100));                                          // another attacker
    ArmorGate beyond;
    beyond.NoteBlow(Blow(), 0.0, 1000.0, kT);
    CHECK(!beyond.StuckHeld(Stuck("spine_03", StrikeSource::WeaponRight, 10 + kArmorHeldMatchUu + 1), kT + 100));   // elsewhere on the bone
    ArmorGate within;
    within.NoteBlow(Blow(), 0.0, 1000.0, kT);
    CHECK(within.StuckHeld(Stuck("spine_03", StrikeSource::WeaponRight, 10 + kArmorHeldMatchUu - 1), kT + 100));
}

static void TestEarlyReturnIsNoVerdict() {
    // DCD returned before its end: Material Density Out stays 0, and so does Cutting Rate Out. Not a verdict.
    ArmorGate g;
    g.NoteBlow(Blow(), 0.0, 0.0, kT);
    CHECK(g.Size() == 0);
    CHECK(!g.StuckHeld(Stuck(), kT + 100));
}

static void TestBluntBlowKeepsVerdict() {
    ArmorGate g;
    g.NoteBlow(Blow(), 0.0, 1000.0, kT);
    g.NoteBlow(Blow(0.0), 0.0, 1000.0, kT + 10);   // no edge: says nothing about a blade
    CHECK(g.StuckHeld(Stuck(), kT + 100));
    ArmorGate through;
    through.NoteBlow(Blow(), 0.0, 1000.0, kT);
    through.NoteBlow(Blow(), 5.0, 1000.0, kT + 10);   // a newer cutting blow on the bone got through
    CHECK(!through.StuckHeld(Stuck(), kT + 100));
    ArmorGate again;
    again.NoteBlow(Blow(), 5.0, 1000.0, kT);
    again.NoteBlow(Blow(), 0.0, 1000.0, kT + 10);     // and a newer one was stopped
    CHECK(again.StuckHeld(Stuck(), kT + 100));
    ArmorGate h;
    h.NoteBlow(Blow(), 5.0, 1000.0, kT);              // a cut got through
    h.NoteBlow(Blow(0.0), 0.0, 1000.0, kT + 10);      // a pommel blow leaves no cut, but says nothing about the blade
    CHECK(!h.StuckHeld(Stuck(), kT + 100));
}

static void TestStuckClaimsAndBadValuesNeverNote() {
    ArmorGate g;
    g.NoteBlow(Stuck(), 0.0, 1000.0, kT);
    g.NoteBlow(Blow(), std::numeric_limits<double>::quiet_NaN(), 1000.0, kT);
    g.NoteBlow(Blow(), 0.0, std::numeric_limits<double>::quiet_NaN(), kT);
    CHECK(g.Size() == 0);
    CHECK(!g.StuckHeld(Blow(), kT));   // not a stuck claim
    CHECK(g.Size() == 0);
}

// The stab went in: its blade scraping plate on the same bone does not pull it out. Once it is out, a new blade is
// judged afresh.
static void TestBladeInStaysIn() {
    ArmorGate g;
    g.NoteBlow(Blow(), 42.0, 1000.0, kT);
    CHECK(!g.StuckHeld(Stuck(), kT + 100));
    g.NoteBlow(Blow(), 0.0, 1000.0, kT + 150);
    CHECK(!g.StuckHeld(Stuck(), kT + 200));
    CHECK(!g.StuckHeld(Stuck(), kT + 300));
    const double later = kT + 300 + kStuckStreamGapMs;
    g.NoteBlow(Blow(), 0.0, 1000.0, later);
    CHECK(g.StuckHeld(Stuck(), later + 100));
}

// The armour held: the blade stays refused however long it sits there, however its entry point moves and through a
// stall in its claims, until a newer blow on the bone gets through. Then it is in, and stays in.
static void TestHeldBladeStaysHeld() {
    ArmorGate g;
    g.NoteBlow(Blow(), 0.0, 1000.0, kT);
    double t = kT + 100;
    for (; t < kT + 10 * kArmorHeldMaxAgeMs; t += 100) CHECK(g.StuckHeld(Stuck(), t));
    CHECK(g.StuckHeld(Stuck("spine_03", StrikeSource::WeaponRight, 10 + kArmorHeldMatchUu + 5), t));
    t += kStuckStreamGapMs + 100;
    CHECK(g.StuckHeld(Stuck(), t));
    g.NoteBlow(Blow(), 5.0, 1000.0, t + 10);
    CHECK(!g.StuckHeld(Stuck(), t + 100));
    g.NoteBlow(Blow(), 0.0, 1000.0, t + 150);
    CHECK(!g.StuckHeld(Stuck(), t + 200));
}

// A refused blade survives a long gap in its claims (a hitch, lost packets): after the gap its entry point may have
// moved past the match distance and the verdict may be older than kArmorHeldMaxAgeMs; it stays refused until a newer
// blow on the bone gets through.
static void TestHeldBladeSurvivesLongGap() {
    ArmorGate g;
    g.NoteBlow(Blow(), 0.0, 1000.0, kT);
    for (double t = kT + 100; t <= kT + 1000; t += 100) CHECK(g.StuckHeld(Stuck(), t));
    const double moved = 10 + kArmorHeldMatchUu + 15;
    CHECK(g.StuckHeld(Stuck("spine_03", StrikeSource::WeaponRight, moved), kT + 1000 + kStuckStreamGapMs + 200));   // 700 ms gap, moved
    const double later = kT + 1700 + kArmorHeldMaxAgeMs + 200;
    CHECK(g.StuckHeld(Stuck(), later));                                                                          // 1.2 s gap
    g.NoteBlow(Blow(), 5.0, 1000.0, later + 10);    // a newer blow got through
    CHECK(!g.StuckHeld(Stuck(), later + 100));
    // After a gap, too: the refusal does not outlive a blow that got through.
    ArmorGate h;
    h.NoteBlow(Blow(), 0.0, 1000.0, kT);
    CHECK(h.StuckHeld(Stuck(), kT + 100));
    h.NoteBlow(Blow(), 5.0, 1000.0, kT + 200);
    CHECK(!h.StuckHeld(Stuck(), kT + 200 + kStuckStreamGapMs + 300));
}

// A blade with no blow of its own (capped, coalesced) is not refused for a stopped blow from long ago.
static void TestOldVerdictStartsNothing() {
    ArmorGate g;
    g.NoteBlow(Blow(), 0.0, 1000.0, kT);
    CHECK(!g.StuckHeld(Stuck(), kT + kArmorHeldMaxAgeMs + 1));
    ArmorGate h;
    h.NoteBlow(Blow(), 0.0, 1000.0, kT);
    CHECK(h.StuckHeld(Stuck(), kT + kArmorHeldMaxAgeMs));
}

static void TestForget() {
    ArmorGate g;
    g.NoteBlow(Blow(), 0.0, 1000.0, kT);
    g.Forget(1);   // the victim respawned
    CHECK(g.Size() == 0);
    CHECK(!g.StuckHeld(Stuck(), kT + 100));
    g.Forget(1);   // its stream goes too
    CHECK(g.Size() == 0);
    g.NoteBlow(Blow(), 0.0, 1000.0, kT);
    g.Forget(2);   // the attacker left
    CHECK(g.Size() == 0);
    g.NoteBlow(Blow(), 0.0, 1000.0, kT);
    g.Forget(7);
    CHECK(g.Size() == 1);
}

int main() {
    TestHeldBlowRefusesItsStuckBlade();
    TestCutBlowLetsItThrough();
    TestNoVerdictIsReplayed();
    TestEarlyReturnIsNoVerdict();
    TestBluntBlowKeepsVerdict();
    TestStuckClaimsAndBadValuesNeverNote();
    TestBladeInStaysIn();
    TestHeldBladeStaysHeld();
    TestHeldBladeSurvivesLongGap();
    TestOldVerdictStartsNothing();
    TestForget();
    if (g_fail) { std::printf("test_hsm_armor_gate: %d failures\n", g_fail); return 1; }
    std::printf("test_hsm_armor_gate: ok\n");
    return 0;
}
