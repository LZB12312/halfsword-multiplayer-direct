// Rest lock (0.8.0-pre35). A soft structure (session/hsm_structure.h) that the host has at rest is steered on the copies for
// ever. A copy that never reaches the host's pose keeps moving: it kicks each second and teleports every few seconds.
// Here a copy at rest is placed once to the host's rest pose, then held still.
//
//   Steer  the copy is steered toward the host's pose (sync/hsm_soft_correct.h), as before.
//   Place  every simulating body goes to the host's rest pose from one snapshot, and their velocities go to zero.
//   Hold   nothing is written to the copy. It stays where it lies.
//
// A copy unlocks when the host's root moves. A touch here unlocks it for a short time; the rest clock keeps running.
// A copy the caller marks as not placeable is never placed. Game-independent.
#pragma once
#include "sync/hsm_soft_correct.h"
#include <cstdint>

namespace hsm::sync {

constexpr double kRestLockAfterMs = 1500.0;    // the host's structure at rest this long before a copy locks to it (a rest state comes once a second)
constexpr double kRestPlaceCm = 10.0;          // a copy this close to the host's pose needs no placing: it locks where it is
constexpr double kRestMovedCm = 5.0;           // the host's root this far from where a copy locked: the host moved, so the copy steers again
constexpr double kRestTouchQuietMs = 1500.0;   // a touch here unlocks a copy for this long after it

enum class RestStep : uint8_t { Steer = 0, Place = 1, Hold = 2 };

struct RestLock {
    bool locked = false;
    double restSinceMs = -1e18;   // when the host's structure came to rest (-1e18: it is moving)
    V3 lockedRoot{};              // the host's root where the copy locked
};

// Decides one copy's step on a state from the host.
//   hostAtRest    every body the host streams is at rest (PropAtRest, the test StreamState applies to the stream)
//   hostRoot      the host's root position, cm
//   worstErrCm    the largest position error of any body here
//   sinceTouchMs  since a fighter, a weapon or another prop touched it here (huge when never)
//   mayPlace      a structure that may be placed and has settled here
//   snapReady     the snapshot's states are usable (SnapReady)
//   sinceSnapMs   since the copy was last placed or snapped (huge when never)
//   bodyMovedCm   how far any other host body has gone from where it was at the lock: a hinged piece can swing while the root stays
inline RestStep DecideRestStep(RestLock& s, double nowMs, bool hostAtRest, const V3& hostRoot, double worstErrCm, double sinceTouchMs,
                               bool mayPlace, bool snapReady, double sinceSnapMs, double bodyMovedCm = 0) {
    if (!hostAtRest) {
        s.locked = false;
        s.restSinceMs = -1e18;
        return RestStep::Steer;
    }
    if (!(s.restSinceMs > -1e17)) s.restSinceMs = nowMs;
    if (sinceTouchMs < kRestTouchQuietMs) {   // touched here: free to move until it is quiet again
        s.locked = false;
        return RestStep::Steer;
    }
    if (s.locked) {
        const V3 d{hostRoot.x - s.lockedRoot.x, hostRoot.y - s.lockedRoot.y, hostRoot.z - s.lockedRoot.z};
        if (VLen(d) <= kRestMovedCm && bodyMovedCm <= kRestMovedCm) return RestStep::Hold;   // a copy that drifts here on its own stays locked: no new place
        s.locked = false;
        s.restSinceMs = nowMs;   // the host moved: it has to come to rest again
        return RestStep::Steer;
    }
    if (nowMs - s.restSinceMs < kRestLockAfterMs) return RestStep::Steer;
    if (worstErrCm < kRestPlaceCm) {
        s.locked = true;
        s.lockedRoot = hostRoot;
        return RestStep::Hold;
    }
    if (mayPlace && snapReady && sinceSnapMs >= kSnapGapMs) {
        s.locked = true;
        s.lockedRoot = hostRoot;
        return RestStep::Place;
    }
    return RestStep::Steer;
}

}  // namespace hsm::sync
