// Moving follow of a late-found soft structure (0.8.0-pre35, object_sync.cpp SoftDrive). A barrel, crate or plank that is
// knocked on the host moves on the host's copy and is streamed there as one snapshot. Until 0.8.0-pre34 every other machine
// only steered its own copy toward that snapshot by velocity, and a barrel ended metres from the host's on some screens (9 of
// 16 barrels were more than 1 m off for over a fifth of a match on pre30). A rock, which has one simulator and takes exact
// poses, did not drift. So while the streamer's copy moves, the copy here is placed from the snapshot as a rock is: every body
// at the host's pose, extrapolated to now, in one frame. Between states the local physics runs as before.
//
// Moving: a host body faster than kFollowMoveCmS or turning faster than kFollowMoveDegS. It stays moving for kFollowStopMs after
// the last such state, so a slowing barrel does not flicker between the two modes. A snapshot is usable when its states arrived
// within kFollowCoherentMs of each other and the newest is no older than kFollowFreshMs. A census structure (a bridge) follows only
// with structure_bridges_follow: teleports of single bodies brought one down (#230), this places all of them from one snapshot.
// Game-independent.
#pragma once
#include "sync/hsm_soft_correct.h"

namespace hsm::sync {

constexpr double kFollowMoveCmS = 30.0;      // a host body faster than this (cm/s) is moving ...
constexpr double kFollowMoveDegS = 30.0;     // ... or turning faster than this (deg/s)
constexpr double kFollowStopMs = 300.0;      // keeps following this long after the last fast state
constexpr double kFollowCoherentMs = 120.0;  // the states of one snapshot arrived within this of each other ...
constexpr double kFollowFreshMs = 200.0;     // ... and the newest is no older than this

// Whether the streamer's copy is moving, with the hold after its last fast state.
struct MoveFollow {
    double lastFastMs = -1e18;   // when a host body was last past a moving speed; far in the past: never
    bool moving = false;         // the last Update's answer
    // Called at each state of the copy with the host's fastest body (cm/s, deg/s). True while the copy is moving.
    bool Update(double nowMs, double fastestCmS, double fastestDegS) {
        if (fastestCmS > kFollowMoveCmS || fastestDegS > kFollowMoveDegS) lastFastMs = nowMs;
        moving = nowMs - lastFastMs <= kFollowStopMs;
        return moving;
    }
};

// The states of all the structure's bodies make one usable snapshot (ages in ms since each arrived: the oldest and the newest).
inline bool SnapshotFollowable(double oldestAgeMs, double newestAgeMs) {
    return newestAgeMs <= kFollowFreshMs && oldestAgeMs - newestAgeMs <= kFollowCoherentMs;
}

// Whether this copy follows its streamer's moving copy this frame: the setting is on, the copy is a late-found structure (a census
// one only with bridges), its body count is the host's, it has settled since it was bound (SnapAllowed), the host's copy moves, and
// the snapshot is usable. sinceBornMs: since the copy was first tracked here.
inline bool CopyFollows(bool setting, bool found, bool shapeAgrees, double sinceBornMs, bool moving, bool snapshotFollowable,
                        bool bridges = false) {
    return setting && (found || bridges) && shapeAgrees && SnapAllowed(sinceBornMs) && moving && snapshotFollowable;
}

}  // namespace hsm::sync
