// Moving follow of a late-found soft structure (sync/hsm_moving_follow.h): a copy follows its streamer's copy only while that copy
// moves (with a hold after the last fast state, so a slowing barrel does not flicker), from a snapshot whose states arrived close
// together and recently, and only for a late-found, settled copy of the host's shape.
#include "sync/hsm_moving_follow.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::sync;

static void TestMovingSpeedsAreStrictlyAbove() {
    MoveFollow f;
    CHECK(!f.Update(0, 0, 0));                                  // at rest
    CHECK(!f.Update(10, kFollowMoveCmS, kFollowMoveDegS));      // exactly at the speeds: not faster than them
    CHECK(f.Update(20, kFollowMoveCmS + 0.5, 0));               // a hair over, linear
    MoveFollow g;
    CHECK(g.Update(0, 0, kFollowMoveDegS + 0.5));               // ... or over, turning
    MoveFollow h;
    CHECK(!h.Update(0, 0, kFollowMoveDegS));                    // exactly at the turn speed: not moving
}

static void TestHoldAfterTheLastFastState() {
    MoveFollow f;
    CHECK(f.Update(100, 31, 0));                                // fast
    CHECK(f.Update(100 + kFollowStopMs, 0, 0));                 // still held at the limit
    CHECK(!f.Update(100 + kFollowStopMs + 1, 0, 0));            // then it stops
    CHECK(f.Update(1000, 0, 31));                               // fast again: moving once more
    CHECK(f.Update(1050, 2, 1));                                // slow states inside the hold keep it moving
    CHECK(!f.Update(1000 + kFollowStopMs + 1, 2, 1));
}

static void TestSlowingBarrelDoesNotFlicker() {
    // A barrel fast until 500 ms, then slowing: its states every 50 ms. Moving to 800 ms, not after.
    MoveFollow f;
    bool flicker = false;
    bool prev = true;
    for (double t = 500; t <= 900; t += 50) {
        const double cm = t <= 500 ? 90.0 : 5.0;
        const bool m = f.Update(t, cm, 0);
        if (t <= 800) CHECK(m);
        if (t >= 850) CHECK(!m);
        if (t > 500 && m != prev && t <= 800) flicker = true;
        prev = m;
    }
    CHECK(!flicker);
}

static void TestSnapshotCoherenceAndFreshness() {
    CHECK(SnapshotFollowable(0, 0));
    CHECK(SnapshotFollowable(kFollowFreshMs, kFollowFreshMs));  // the newest exactly at the fresh limit
    CHECK(!SnapshotFollowable(kFollowFreshMs + 1, kFollowFreshMs + 1));   // a newest one past it: stale
    CHECK(SnapshotFollowable(kFollowCoherentMs + 50, 50));      // 120 ms between the oldest and the newest: still one snapshot
    CHECK(!SnapshotFollowable(kFollowCoherentMs + 51, 50));     // 121 ms: a partial set, wait for the rest
    CHECK(!SnapshotFollowable(400, 10));                        // one body's state from an older batch
}

static void TestCopyFollowsNeedsEveryCondition() {
    const double settled = kSnapSettleMs, unsettled = kSnapSettleMs - 1;
    CHECK(CopyFollows(true, true, true, settled, true, true));
    CHECK(!CopyFollows(false, true, true, settled, true, true));    // structure_follow_moving=0
    CHECK(!CopyFollows(true, false, true, settled, true, true));    // a census structure (#230): steered as before
    CHECK(!CopyFollows(true, true, false, settled, true, true));    // another body count than the host's
    CHECK(!CopyFollows(true, true, true, unsettled, true, true));   // just bound: its joints still settle
    CHECK(!CopyFollows(true, true, true, settled, false, true));    // the host's copy is at rest
    CHECK(!CopyFollows(true, true, true, settled, true, false));    // no usable snapshot this frame
    CHECK(CopyFollows(true, false, true, settled, true, true, true));     // a bridge, with structure_bridges_follow
    CHECK(!CopyFollows(false, false, true, settled, true, true, true));   // the moving setting still decides
    CHECK(!CopyFollows(true, false, true, unsettled, true, true, true));  // and it settles first
}

int main() {
    TestMovingSpeedsAreStrictlyAbove();
    TestHoldAfterTheLastFastState();
    TestSlowingBarrelDoesNotFlicker();
    TestSnapshotCoherenceAndFreshness();
    TestCopyFollowsNeedsEveryCondition();
    if (g_fail) { std::printf("test_hsm_moving_follow: %d check(s) failed\n", g_fail); return 1; }
    std::printf("test_hsm_moving_follow: all checks passed\n");
    return 0;
}
