// Constraint-built level props (session/hsm_structure.h). soft (default): streamed by the host as one snapshot at 20 Hz while any
// body moves, steered on receivers, never leased or reset. local: never streamed, driven, claimed or reset. full: as pre17.
// A body that detached streams; a plain prop, a one-body prop, a prop with no constraints and a weapon are unchanged.
// sync_structures parses "soft", "local", "full"; the old "0" is soft and "1" is full.
// 0.8.0-pre29: a level actor found after the census (announced under a spawned id: breakable barrels and planks) is a structure
// by the same shape; a copy with another body count is never driven; any machine passes on a break of its own copy, except
// while the structure settles or just after a snap; the host takes a bounded rate of them from one machine.
#include "session/hsm_structure.h"
#include <cstdio>
#include <cstring>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static const uint64_t kBridge = MakeLevelPropId(12);
static const StructureMode kSoft = StructureMode::Soft, kLocal = StructureMode::Local, kFull = StructureMode::Full;

static void TestStructureShape() {
    // The Pit's bridge: several planks joined by constraints. Whether a joint has broken is not part of the rule: the
    // bodies still joined are treated alike either way.
    CHECK(IsStructure(kBridge, false, 8, 7));
    CHECK(IsStructure(kBridge, false, 2, 1));                      // the smallest: two bodies, one joint
    CHECK(IsStructure(MakeLevelPropId(0), false, 30, 29));         // a chain
}

static void TestModesPickTheHandling() {
    CHECK(StructureSoft(kSoft, kBridge, false, 8, 7));
    CHECK(!StructureLocal(kSoft, kBridge, false, 8, 7));
    CHECK(StructureOwnCopy(kSoft, kBridge, false, 8, 7));          // no lease, no claimed blow, no reset move
    CHECK(StructureLocal(kLocal, kBridge, false, 8, 7));
    CHECK(!StructureSoft(kLocal, kBridge, false, 8, 7));
    CHECK(StructureOwnCopy(kLocal, kBridge, false, 8, 7));
    CHECK(!StructureSoft(kFull, kBridge, false, 8, 7));            // full: as pre17, streamed, teleported, leased and reset
    CHECK(!StructureLocal(kFull, kBridge, false, 8, 7));
    CHECK(!StructureOwnCopy(kFull, kBridge, false, 8, 7));
}

static void TestDetachedBodyStreams() {
    // A body that left the structure is its own object: one body, no constraints, a sub of the same actor.
    for (StructureMode m : {kSoft, kLocal, kFull}) {
        CHECK(!StructureOwnCopy(m, MakeSpawnedId(0, 5), true, 1, 0));
        CHECK(!StructureOwnCopy(m, kBridge, true, 1, 0));          // even under a level id, a detached body is not one
        CHECK(!StructureOwnCopy(m, kBridge, true, 8, 7));
    }
}

static void TestOtherPropsUnchanged() {
    for (StructureMode m : {kSoft, kLocal, kFull}) {
        CHECK(!StructureOwnCopy(m, kBridge, false, 1, 0));         // a barrel: one body
        CHECK(!StructureOwnCopy(m, kBridge, false, 1, 3));         // one body is never a structure
        CHECK(!StructureOwnCopy(m, kBridge, false, 4, 0));         // several bodies, nothing joining them
        CHECK(!StructureOwnCopy(m, kBridge, false, 0, 0));
    }
}

static void TestWeaponsAndSpawnedNeverStructures() {
    for (StructureMode m : {kSoft, kLocal, kFull}) {
        CHECK(!StructureOwnCopy(m, MakeWeaponPropId(1, 0), false, 3, 2));
        CHECK(!StructureOwnCopy(m, MakeDropPropId(2, 1, 3), false, 3, 2));
        CHECK(!StructureOwnCopy(m, MakeSpawnedId(1, 9), false, 3, 2));
    }
    CHECK(!IsStructure(MakeWeaponPropId(1, 0), false, 3, 2));
}

static void TestFoundLevelActorsAreStructures() {
    // A barrel (22 bodies, 2 joints) the host's walk found 5 s after the census: announced under a spawned id.
    const uint64_t barrel = MakeSpawnedId(0, 1);
    CHECK(IsStructure(barrel, false, 22, 2, true));
    CHECK(StructureSoft(kSoft, barrel, false, 22, 2, true));       // steered, never teleported body by body
    CHECK(StructureLocal(kLocal, barrel, false, 22, 2, true));
    CHECK(StructureOwnCopy(kSoft, barrel, false, 22, 2, true));    // nobody leases or resets it
    CHECK(!StructureOwnCopy(kFull, barrel, false, 22, 2, true));   // full: as pre17
    CHECK(!StructureSoft(kSoft, barrel, false, 22, 2));            // not found by the walk (thrown, proposed): unchanged
    for (StructureMode m : {kSoft, kLocal, kFull}) {
        CHECK(!StructureOwnCopy(m, barrel, true, 1, 0, true));     // a plank that came off is its own object
        CHECK(!StructureOwnCopy(m, barrel, false, 1, 0, true));    // one body: never
        CHECK(!StructureOwnCopy(m, barrel, false, 22, 0, true));   // nothing joining them
        CHECK(!StructureOwnCopy(m, MakeWeaponPropId(1, 0), false, 3, 2, true));   // a weapon never, found or not
    }
}

static void TestShapesAgree() {
    CHECK(ShapesAgree(22, 22));
    CHECK(!ShapesAgree(22, 19));                                   // three planks fewer here: body N is not body N there
    CHECK(!ShapesAgree(1, 22));
    CHECK(ShapesAgree(0, 22));                                     // not known (a census prop): no check
}

static void TestWhoReportsABreak() {
    CHECK(MayReportJoint(true, false));                            // the streamer of any object
    CHECK(MayReportJoint(true, true));
    CHECK(MayReportJoint(false, true));                            // anyone, for their own copy of a structure
    CHECK(!MayReportJoint(false, false));                          // a loose prop someone else streams: not mine to report
}

static void TestLocalBreakIsSharedOnlyWhenReal() {
    const double bound = 10000.0, never = -1e18;
    CHECK(ClassifyLocalBreak(bound, bound, never) == LocalBreak::Settling);                       // just bound here
    CHECK(ClassifyLocalBreak(bound + kBreakSettleMs - 1, bound, never) == LocalBreak::Settling);
    CHECK(ClassifyLocalBreak(bound + kBreakSettleMs, bound, never) == LocalBreak::Share);         // settled: a real break
    const double snap = 60000.0;
    CHECK(ClassifyLocalBreak(snap, bound, snap) == LocalBreak::AfterSnap);                        // our own placement did it
    CHECK(ClassifyLocalBreak(snap + kBreakAfterSnapMs - 1, bound, snap) == LocalBreak::AfterSnap);
    CHECK(ClassifyLocalBreak(snap + kBreakAfterSnapMs, bound, snap) == LocalBreak::Share);
    CHECK(std::string(LocalBreakName(LocalBreak::Settling)) == "settling");
    // Another shape than the host's: its joint N is not the host's, so a break there is never passed on, settled or not.
    CHECK(ClassifyLocalBreak(bound + 1e6, bound, never, true) == LocalBreak::ShapeDiffers);
    CHECK(ClassifyLocalBreak(bound, bound, never, true) == LocalBreak::ShapeDiffers);
    CHECK(ClassifyLocalBreak(bound + 1e6, bound, never, false) == LocalBreak::Share);   // NEGATIVE CONTROL
    CHECK(std::string(LocalBreakName(LocalBreak::ShapeDiffers)) == "body count differs");
    CHECK(std::string(LocalBreakName(LocalBreak::OverBudget)) == "too many at once");
}

// The 2026-11-05 Alley failure: a barrel that broke on a client alone was never corrected again, up to 1,416 cm off.
static void TestCopyDivergesOnAnyBreakKeptHere() {
    CHECK(!CopyDiverged(0, 0));                  // whole, or every break shared: steered as a structure
    CHECK(CopyDiverged(1, 0));
    CHECK(CopyDiverged(uint64_t(1) << 63, 0));   // the mask's last joint
    CHECK(CopyDiverged(0, uint64_t(1) << 18));   // a barrel's last body
    CHECK(CopyDiverged(3, 4));
}

// What happens to such a copy. 13 of the 39 breaks passed on in those logs came from copies already broken, so a later hit on one
// must still reach everyone.
static void TestBrokenCopyFollowsTheHost() {
    using M = StructureMode;
    CHECK(FollowsWhenBroken(M::Soft, true, true, false));
    CHECK(!FollowsWhenBroken(M::Soft, false, true, false));    // structure_follow_broken=0
    CHECK(!FollowsWhenBroken(M::Soft, true, false, false));    // a census bridge: teleports brought it down (#230)
    CHECK(!FollowsWhenBroken(M::Soft, true, true, true));      // body N here is not body N there
    CHECK(!FollowsWhenBroken(M::Local, true, true, false));    // never driven
    CHECK(!FollowsWhenBroken(M::Full, true, true, false));

    CHECK(BodyPlacedOnItsOwn(0, ~uint64_t(0)));                // the root always
    CHECK(!BodyPlacedOnItsOwn(1, 0b0110));                     // hangs on another: goes with it
    CHECK(!BodyPlacedOnItsOwn(2, 0b0110));
    CHECK(BodyPlacedOnItsOwn(3, 0b0110));                      // came off, or never hung
    CHECK(!BodyPlacedOnItsOwn(63, uint64_t(1) << 63));
    CHECK(BodyPlacedOnItsOwn(64, ~uint64_t(0)));               // past the mask

    CHECK(ShouldPlaceBody(false, 0, kPlaceDeadBandCm));        // never placed, and off
    CHECK(!ShouldPlaceBody(false, 0, kPlaceDeadBandCm - 0.1)); // near enough: left asleep
    CHECK(!ShouldPlaceBody(true, 0, 500));                     // fell here while the host's stood still: no reset every second
    CHECK(!ShouldPlaceBody(true, kPlaceDeadBandCm - 0.1, 500));
    CHECK(ShouldPlaceBody(true, kPlaceDeadBandCm, 500));       // the host's has moved on
    CHECK(!ShouldPlaceBody(true, 500, 1));                     // and this one kept up

    // A placement stamps lastSnapMs: a break right after it may be this machine's doing, one later is a real hit.
    const double bound = 0, placed = kBreakSettleMs + 5000;
    CHECK(ClassifyLocalBreak(placed + 100, bound, placed) == LocalBreak::AfterSnap);
    CHECK(ClassifyLocalBreak(placed + kBreakAfterSnapMs, bound, placed) == LocalBreak::Share);
}

// A break not sent at once. The game has no way back from one, so the copies only agree if the host's breaks too.
static void TestBreakNotSentAtOnce() {
    using L = LocalBreak;
    using S = BreakStep;
    CHECK(NextBreakStep(L::Share, true, true) == S::Send);
    CHECK(NextBreakStep(L::Share, false, false) == S::Send);            // no setting stops a break that was never in doubt
    CHECK(NextBreakStep(L::AfterSnap, true, true) == S::Hold);          // Alley's barrels
    CHECK(NextBreakStep(L::AfterSnap, true, false) == S::Keep);         // a census bridge: teleports brought it down (#230)
    CHECK(NextBreakStep(L::OverBudget, true, true) == S::Hold);
    CHECK(NextBreakStep(L::OverBudget, true, false) == S::Hold);        // a real break of a bridge, only too many at once
    CHECK(NextBreakStep(L::Settling, true, true) == S::Keep);           // nobody touched it: start-up differs per machine
    CHECK(NextBreakStep(L::ShapeDiffers, true, true) == S::Keep);       // joint N here is not joint N there
    CHECK(NextBreakStep(L::AfterSnap, false, true) == S::Keep);         // structure_share_held_breaks=0: as pre30
    CHECK(NextBreakStep(L::OverBudget, false, true) == S::Keep);

    CHECK(!BreaksHeld(0, 0));
    CHECK(BreaksHeld(1, 0));
    CHECK(BreaksHeld(0, uint64_t(1) << 18));                            // a barrel's last body
    CHECK(BreaksHeld(uint64_t(1) << 63, 0));

    // The hold ends. A break right after a snap waits; nothing places the copy meanwhile, so one poll after the wait it goes.
    const double bound = 0, snap = kBreakSettleMs + 5000;
    double t = snap + 100;
    CHECK(NextBreakStep(ClassifyLocalBreak(t, bound, snap), true, true) == S::Hold);
    int polls = 0;
    while (NextBreakStep(ClassifyLocalBreak(t, bound, snap), true, true) == S::Hold && polls < 100) { t += 250.0; ++polls; }
    CHECK(polls == 4);
    CHECK(t - snap >= kBreakAfterSnapMs);
    CHECK(ClassifyLocalBreak(t, bound, snap) == L::Share);
}

// What a client sends always fits what the host takes: a break the host dropped would leave the sender's copy broken and
// still corrected, since it never hears of the drop. The host hears the reports a little later, so its refill trails.
static void TestSendBudgetFitsTheHost() {
    CHECK(kJointSendBurst < kJointReportBurst);
    JointReportBudget send(kJointSendBurst), host;
    int sent = 0, taken = 0;
    for (int i = 0; i < 2000; ++i) {   // a report every 20 ms for 40 s, far over the rate
        const double t = 1000.0 + i * 20.0;
        if (!send.Take(t)) continue;
        ++sent;
        if (host.Take(t + 150.0 + (i % 7) * 40.0)) ++taken;   // uneven delay on the way
    }
    CHECK(sent > 0 && taken == sent);
    JointReportBudget capped(kJointSendBurst);   // NEGATIVE CONTROL: the burst is the smaller one
    int first = 0;
    for (int i = 0; i < 100; ++i) first += capped.Take(1000.0) ? 1 : 0;
    CHECK(first == int(kJointSendBurst));
}

static void TestJointReportBudget() {
    JointReportBudget b;
    int taken = 0;
    for (int i = 0; i < 100; ++i) taken += b.Take(1000.0) ? 1 : 0;
    CHECK(taken == int(kJointReportBurst));                        // a burst: two whole barrels at once
    CHECK(!b.Take(1000.0));
    CHECK(!b.Take(1000.0 + 1000.0 / kJointReportsPerSec - 1));     // refills at kJointReportsPerSec
    CHECK(b.Take(1000.0 + 2000.0 / kJointReportsPerSec));
    JointReportBudget c;
    CHECK(c.Take(5000.0));
    for (int i = 0; i < 1000; ++i) c.Take(5000.0);
    CHECK(c.Take(5000.0 + 60000.0));                               // a minute later it is full again ...
    taken = 1;
    for (int i = 0; i < 100; ++i) taken += c.Take(5000.0 + 60000.0) ? 1 : 0;
    CHECK(taken == int(kJointReportBurst));                        // ... but never more than the burst
    JointReportBudget d;
    CHECK(d.Take(9000.0));
    CHECK(d.Take(8000.0));                                         // a clock that steps back neither refills nor locks it
}

static void TestIniValues() {
    CHECK(ParseStructureMode("soft") == kSoft);
    CHECK(ParseStructureMode("local") == kLocal);
    CHECK(ParseStructureMode("full") == kFull);
    CHECK(ParseStructureMode(" Full ") == kFull);                  // case and blanks do not matter
    CHECK(ParseStructureMode("LOCAL") == kLocal);
    CHECK(ParseStructureMode("0") == kSoft);                       // an old INI: 0 meant "not pre17"
    CHECK(ParseStructureMode("1") == kFull);                       // 1 meant "pre17"
    CHECK(ParseStructureMode("") == kSoft);                        // no line: the default
    CHECK(ParseStructureMode("bogus") == kSoft);
    CHECK(std::string(StructureModeName(kSoft)) == "soft" && std::string(StructureModeName(kLocal)) == "local" &&
          std::string(StructureModeName(kFull)) == "full");
}

static void TestStreamRate() {
    // Moving: every 50 ms (20 Hz), not the loose prop's 16 ms; at rest once a second.
    CHECK(!StructureStreamDue(1000.0, false, 1000.0 + kStructureStreamMs - 1));
    CHECK(StructureStreamDue(1000.0, false, 1000.0 + kStructureStreamMs));
    CHECK(!StructureStreamDue(1000.0, true, 1000.0 + kFreeIdleMs - 1));
    CHECK(StructureStreamDue(1000.0, true, 1000.0 + kFreeIdleMs));
    CHECK(StructureStreamDue(-1e18, false, 0.0));                  // never sent: due at once
    CHECK(kStructureStreamMs > kFreeStreamMs);                     // slower than a loose moving prop (60 Hz)
    // 0.8.0-pre29: 20 Hz at any frame rate (the remainder carries, hsm_lease.h NextSentMs); before, about 18 Hz.
    CHECK(StructureStreamPeriodMs(false) == kStructureStreamMs && StructureStreamPeriodMs(true) == kFreeIdleMs);
    for (double fps : {30.0, 60.0, 90.0, 144.0, 165.0}) {
        double last = -1e18, now = 0.0;
        int sends = 0;
        while (now < 10000.0) {
            now += 1000.0 / fps;
            if (!StructureStreamDue(last, false, now)) continue;
            ++sends;
            last = NextSentMs(last, StructureStreamPeriodMs(false), now);
        }
        CHECK(sends >= 199 && sends <= 201);
    }
}

// A rest placement moves bodies as a snap does: a break in the next second is this machine's own and stays here.
// A hit in the first seconds is a real break; the same break with nobody near it is the start-up that differs per machine.
static void TestHitWhileSettlingIsShared() {
    using L = LocalBreak;
    const double bound = 1000, now = bound + 1000, never = -1e18;
    CHECK(ClassifyLocalBreak(now, bound, never) == L::Settling);
    CHECK(ClassifyLocalBreak(now, bound, never, false, never, now - 100) == L::Share);
    CHECK(ClassifyLocalBreak(now, bound, never, false, never, now - kBreakHitMs + 1) == L::Share);
    CHECK(ClassifyLocalBreak(now, bound, never, false, never, now - kBreakHitMs) == L::Settling);
    CHECK(ClassifyLocalBreak(now, bound, never, true, never, now - 100) == L::ShapeDiffers);   // joint N here is still not joint N there
}

static void TestGameBreakAndRestPlacing() {
    CHECK(ReplaysGameBreak(true, true, true));
    CHECK(!ReplaysGameBreak(false, true, true));   // structure_game_break=0
    CHECK(!ReplaysGameBreak(true, false, true));
    CHECK(!ReplaysGameBreak(true, true, false));   // a ring, or not a barrel
    const uint64_t hung = uint64_t(1) << 2;
    CHECK(BodyPlacedAtRest(2, hung, false));       // a bridge goes whole
    CHECK(!BodyPlacedAtRest(2, hung, true));       // a late-found copy: what hangs goes with its parent
    CHECK(BodyPlacedAtRest(0, hung, true));
}

static void TestBreakAfterRestPlaceStaysHere() {
    CHECK(ClassifyLocalBreak(10000, 0, -1e18, false, 9500) == LocalBreak::AfterRestPlace);
    CHECK(ClassifyLocalBreak(10000, 0, -1e18, false, 9500) != LocalBreak::Share);
    CHECK(NextBreakStep(LocalBreak::AfterRestPlace, true, true) == BreakStep::Keep);     // found or not: it stays here
    CHECK(NextBreakStep(LocalBreak::AfterRestPlace, true, false) == BreakStep::Keep);
    CHECK(std::strcmp(LocalBreakName(LocalBreak::AfterRestPlace), "after a rest placement") == 0);
    // The window is kBreakAfterSnapMs after the placing: open at 999 ms, shut at 1000 ms.
    CHECK(ClassifyLocalBreak(10000, 0, -1e18, false, 10000 - kBreakAfterSnapMs + 1) == LocalBreak::AfterRestPlace);
    CHECK(ClassifyLocalBreak(10000, 0, -1e18, false, 10000 - kBreakAfterSnapMs) == LocalBreak::Share);
    // Both windows open: the rest placement is the one that stays here.
    CHECK(ClassifyLocalBreak(10000, 0, 9500, false, 9500) == LocalBreak::AfterRestPlace);
    // A snap alone still holds a found copy's break; the old three-argument form is unchanged.
    CHECK(ClassifyLocalBreak(10000, 0, 9500) == LocalBreak::AfterSnap);
    CHECK(NextBreakStep(LocalBreak::AfterSnap, true, true) == BreakStep::Hold);
    CHECK(ClassifyLocalBreak(10000, 0, -1e18) == LocalBreak::Share);
    // Settling and a shape difference still come first.
    CHECK(ClassifyLocalBreak(1000, 0, -1e18, false, 900) == LocalBreak::Settling);
    CHECK(ClassifyLocalBreak(10000, 0, -1e18, true, 9500) == LocalBreak::ShapeDiffers);
}

int main() {
    TestStructureShape();
    TestModesPickTheHandling();
    TestDetachedBodyStreams();
    TestOtherPropsUnchanged();
    TestWeaponsAndSpawnedNeverStructures();
    TestFoundLevelActorsAreStructures();
    TestShapesAgree();
    TestWhoReportsABreak();
    TestLocalBreakIsSharedOnlyWhenReal();
    TestCopyDivergesOnAnyBreakKeptHere();
    TestBrokenCopyFollowsTheHost();
    TestBreakAfterRestPlaceStaysHere();
    TestHitWhileSettlingIsShared();
    TestGameBreakAndRestPlacing();
    TestBreakNotSentAtOnce();
    TestJointReportBudget();
    TestSendBudgetFitsTheHost();
    TestIniValues();
    TestStreamRate();
    if (g_fail) { std::printf("test_hsm_structure: %d check(s) failed\n", g_fail); return 1; }
    std::printf("test_hsm_structure: all checks passed\n");
    return 0;
}
