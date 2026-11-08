// Rest lock (sync/hsm_rest_lock.h): a copy of a soft structure at rest is placed once, then held still until the host moves
// or something touches it here. Game-independent.
#include "sync/hsm_rest_lock.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::sync;

static const double kNever = 1e18;   // "never" for the touch and snapshot clocks
static const V3 kHome{0, 0, 0};

// One call with the host at rest.
static RestStep Rest(RestLock& s, double nowMs, const V3& root, double errCm, double sinceTouchMs = kNever,
                     bool mayPlace = true, bool snapReady = true, double sinceSnapMs = kNever) {
    return DecideRestStep(s, nowMs, true, root, errCm, sinceTouchMs, mayPlace, snapReady, sinceSnapMs);
}

static void TestMovingHostIsSteered() {
    RestLock s;
    CHECK(DecideRestStep(s, 0, false, kHome, 2, kNever, true, true, kNever) == RestStep::Steer);
    CHECK(!s.locked);
    // Still and close, but the rest clock has only just started.
    CHECK(Rest(s, 100, kHome, 2) == RestStep::Steer);
    CHECK(!s.locked);
}

static void TestLockWaitsForTheRestClock() {
    RestLock s;
    CHECK(Rest(s, 1000, kHome, 2) == RestStep::Steer);   // the rest clock starts here
    CHECK(Rest(s, 1000 + kRestLockAfterMs - 1, kHome, 2) == RestStep::Steer);
    CHECK(!s.locked);
    CHECK(Rest(s, 1000 + kRestLockAfterMs, kHome, 2) == RestStep::Hold);
    CHECK(s.locked);
}

static void TestCloseCopyLocksWithoutPlacing() {
    RestLock s;
    CHECK(Rest(s, 0, kHome, 4) == RestStep::Steer);
    // 4 cm off: it locks where it lies. Nothing is placed, though a snapshot is ready and the gap has passed.
    CHECK(Rest(s, kRestLockAfterMs, kHome, 4, kNever, true, true, kNever) == RestStep::Hold);
    CHECK(s.locked);
    CHECK(Rest(s, 2000, kHome, 4) == RestStep::Hold);
    // Exactly kRestPlaceCm is not close: that copy is placed.
    RestLock far;
    CHECK(Rest(far, 0, kHome, kRestPlaceCm) == RestStep::Steer);
    CHECK(Rest(far, kRestLockAfterMs, kHome, kRestPlaceCm, kNever, true, true, kNever) == RestStep::Place);
}

static void TestFarCopyIsPlacedOnce() {
    RestLock s;
    const V3 root{100, 50, 300};
    CHECK(Rest(s, 0, root, 80) == RestStep::Steer);
    // 80 cm off, a ready snapshot, the gap passed: placed, to the host's root.
    CHECK(Rest(s, kRestLockAfterMs, root, 80, kNever, true, true, 5000) == RestStep::Place);
    CHECK(s.locked);
    CHECK(s.lockedRoot.x == 100 && s.lockedRoot.y == 50 && s.lockedRoot.z == 300);
    // The place stamped the snapshot clock, so the copy is held. No second place, even though it is still far off.
    CHECK(Rest(s, 3000, root, 80, kNever, true, true, 0) == RestStep::Hold);
    CHECK(Rest(s, 9000, root, 80, kNever, true, true, 6000) == RestStep::Hold);
}

static void TestFarCopyWaitsForTheSnapGap() {
    RestLock s;
    CHECK(Rest(s, 0, kHome, 80) == RestStep::Steer);
    CHECK(Rest(s, kRestLockAfterMs, kHome, 80, kNever, true, true, kSnapGapMs - 1) == RestStep::Steer);
    CHECK(!s.locked);
    CHECK(Rest(s, kRestLockAfterMs + 100, kHome, 80, kNever, true, true, kSnapGapMs) == RestStep::Place);
}

static void TestSnapshotMustBeReady() {
    RestLock s;
    CHECK(Rest(s, 0, kHome, 80) == RestStep::Steer);
    CHECK(Rest(s, kRestLockAfterMs, kHome, 80, kNever, true, false, kNever) == RestStep::Steer);
    CHECK(!s.locked);
}

static void TestCensusCopyIsNeverPlaced() {
    RestLock s;
    CHECK(Rest(s, 0, kHome, 80) == RestStep::Steer);
    CHECK(Rest(s, kRestLockAfterMs, kHome, 80, kNever, false, true, kNever) == RestStep::Steer);
    CHECK(Rest(s, 60000, kHome, 80, kNever, false, true, kNever) == RestStep::Steer);
    CHECK(!s.locked);
}

static void TestHostMoveUnlocksAndRestartsTheClock() {
    RestLock s;
    CHECK(Rest(s, 0, kHome, 4) == RestStep::Steer);
    CHECK(Rest(s, kRestLockAfterMs, kHome, 4) == RestStep::Hold);
    // The host's root moved 5 cm: still inside the band, so it stays locked.
    CHECK(Rest(s, 2000, V3{0, 0, 5}, 4) == RestStep::Hold);
    CHECK(s.locked);
    // Moved 6 cm: unlocked, and the rest clock restarts at 2500.
    CHECK(Rest(s, 2500, V3{0, 0, 6}, 4) == RestStep::Steer);
    CHECK(!s.locked);
    CHECK(Rest(s, 2500 + kRestLockAfterMs - 1, V3{0, 0, 6}, 4) == RestStep::Steer);
    CHECK(Rest(s, 2500 + kRestLockAfterMs, V3{0, 0, 6}, 4) == RestStep::Hold);
    CHECK(s.locked);
}

static void TestTouchPausesTheLockAndTheRestClockKeepsRunning() {
    RestLock s;
    CHECK(Rest(s, 0, kHome, 4) == RestStep::Steer);
    CHECK(Rest(s, kRestLockAfterMs, kHome, 4) == RestStep::Hold);
    // Touched here at 5000: free to move, no hold.
    CHECK(Rest(s, 5000, kHome, 4, 0) == RestStep::Steer);
    CHECK(!s.locked);
    CHECK(Rest(s, 5000 + kRestTouchQuietMs - 1, kHome, 4, kRestTouchQuietMs - 1) == RestStep::Steer);
    // Quiet again: the rest clock never restarted, so the copy locks at once.
    CHECK(Rest(s, 5000 + kRestTouchQuietMs, kHome, 4, kRestTouchQuietMs) == RestStep::Hold);
    CHECK(s.locked);
}

static void TestDriftWhileLockedStaysHeld() {
    RestLock s;
    CHECK(Rest(s, 0, kHome, 4) == RestStep::Steer);
    CHECK(Rest(s, kRestLockAfterMs, kHome, 4) == RestStep::Hold);
    // The copy drifts 50 cm with the host still: it stays held and is not placed again.
    CHECK(Rest(s, 2000, kHome, 50, kNever, true, true, kNever) == RestStep::Hold);
    CHECK(s.locked);
}

// A hinged piece swings slowly on the host while the root stays: the lock ends, or the copy stays behind.
static void TestOtherBodyMovingUnlocks() {
    RestLock s;
    CHECK(Rest(s, 0, kHome, 4) == RestStep::Steer);
    CHECK(Rest(s, kRestLockAfterMs, kHome, 4) == RestStep::Hold);
    CHECK(DecideRestStep(s, 2000, true, kHome, 4, kNever, true, true, kNever, kRestMovedCm) == RestStep::Hold);
    CHECK(DecideRestStep(s, 3000, true, kHome, 4, kNever, true, true, kNever, kRestMovedCm + 1) == RestStep::Steer);
    CHECK(!s.locked);
}

static void TestLeavingRestResetsTheLock() {
    RestLock s;
    CHECK(Rest(s, 0, kHome, 4) == RestStep::Steer);
    CHECK(Rest(s, kRestLockAfterMs, kHome, 4) == RestStep::Hold);
    CHECK(DecideRestStep(s, 2000, false, kHome, 4, kNever, true, true, kNever) == RestStep::Steer);
    CHECK(!s.locked);
    // At rest again from 3000: the clock starts over.
    CHECK(Rest(s, 3000, kHome, 4) == RestStep::Steer);
    CHECK(Rest(s, 3000 + kRestLockAfterMs - 1, kHome, 4) == RestStep::Steer);
    CHECK(Rest(s, 3000 + kRestLockAfterMs, kHome, 4) == RestStep::Hold);
}

static void TestConstants() {
    // The host's rest state comes about once a second, so a copy waits past one before it locks.
    CHECK(kRestLockAfterMs > 1000.0);
    CHECK(kRestPlaceCm > 0.0 && kRestPlaceCm < kSnapCm);
    CHECK(kRestMovedCm > 0.0);
    CHECK(kRestTouchQuietMs > 0.0);
}

int main() {
    TestMovingHostIsSteered();
    TestLockWaitsForTheRestClock();
    TestCloseCopyLocksWithoutPlacing();
    TestFarCopyIsPlacedOnce();
    TestFarCopyWaitsForTheSnapGap();
    TestSnapshotMustBeReady();
    TestCensusCopyIsNeverPlaced();
    TestHostMoveUnlocksAndRestartsTheClock();
    TestTouchPausesTheLockAndTheRestClockKeepsRunning();
    TestDriftWhileLockedStaysHeld();
    TestOtherBodyMovingUnlocks();
    TestLeavingRestResetsTheLock();
    TestConstants();
    if (g_fail) { std::printf("test_hsm_rest_lock: %d check(s) failed\n", g_fail); return 1; }
    std::printf("test_hsm_rest_lock: all checks passed\n");
    return 0;
}
