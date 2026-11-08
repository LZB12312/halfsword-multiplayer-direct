// Soft correction of a body that is driven by someone else's stream (0.8.0-pre18 structures, #230 / #85). A structure (a hanging
// bridge, a chain: bodies joined by physics constraints, session/hsm_structure.h) is simulated by the host and streamed to the
// others, but a received state must not be teleported onto a body: the joints do not follow a teleport, take huge impulses and
// break. So the receiving copy is steered instead. Each body gets the host's velocity plus a push toward the host's pose
// (extrapolated by its velocity to now), and the physics engine moves it there, joints and all:
//
//     v = v_host + clamp((posErr - dead) * gain, kSoftMaxPosCorr)         (cm/s)
//     w = w_host + clamp((rotErr - dead) * gain, kSoftMaxAngCorr)         (deg/s)
//
// posErr is target - current; rotErr is the shortest-arc rotation that takes the current orientation to the target's, as a
// rotation vector. Under the dead band (2 cm, 2 deg) the body gets the host's velocity alone. The gain is limited by the time since the
// body was last steered (it can never ask for more than kSoftMaxStep of the error in one interval), so the error shrinks
// every update and never swings past the target, however the stream's rate or jitter changes.
//
// When the whole structure is badly off (any body further than kSnapCm from its target, or any body further than kStuckCm
// for kStuckMs: a copy that sleeps or hangs wrongly), ShouldSnap lets all its bodies be placed at once from one snapshot
// (SnapReady), at most once every kSnapGapMs. World space, centimetres, degrees. Game-independent.
#pragma once
#include "sync/hsm_pose.h"
#include <algorithm>
#include <cmath>

namespace hsm::sync {

constexpr double kSoftGain = 8.0;            // 1/s: about 40% of the error removed per 50 ms update (the structure stream's rate)
constexpr double kSoftMaxStep = 0.9;         // never ask to remove more than this fraction of the error in one update interval
constexpr double kSoftMaxPosCorr = 400.0;    // cm/s; the most a correction adds to the host's velocity
constexpr double kSoftMaxAngCorr = 360.0;    // deg/s
constexpr double kSoftDeadCm = 2.0;          // under this position error a body gets the host's velocity alone
constexpr double kSoftDeadDeg = 2.0;
constexpr double kSoftMinDtSec = 0.016;      // the interval since the last steer is taken as at least this ...
constexpr double kSoftMaxDtSec = 0.5;        // ... and at most this
constexpr double kSoftMaxExtrapMs = 250.0;   // a state is extrapolated by its velocity for at most this long

constexpr double kSnapCm = 150.0;            // any body this far from its target: the whole structure is placed at once
constexpr double kStuckCm = 60.0;            // any body this far for kStuckMs on end: the copy hangs or sleeps wrongly
constexpr double kStuckMs = 2000.0;
constexpr double kSnapGapMs = 5000.0;        // a structure is snapped at most this often
constexpr double kSnapCoherentMs = 300.0;    // the snapshot's states arrived within this of each other ...
constexpr double kSnapFreshMs = 500.0;       // ... and the newest is no older than this
constexpr double kSnapSettleMs = 6000.0;     // no snap this soon after the structure was bound here (it settles)
constexpr double kRestCmS = 3.0;             // a body slower than this (and turning slower than kRestDegS) is at rest
constexpr double kRestDegS = 3.0;

// ---------------------------------------------------------------- quaternion helpers (x, y, z, w; Q4 is unit)
inline Q4 QMul(const Q4& a, const Q4& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Q4 QConj(const Q4& q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Q4 QNormalized(const Q4& q) {
    const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(n > 1e-9) || !std::isfinite(n)) return {};
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}
inline double VLen(const V3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
inline V3 VAdd(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 VScale(const V3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }

// The shortest-arc rotation from `from` to `to` as a rotation vector (axis * angle) in degrees, world frame. q and -q are the
// same orientation, so the result never exceeds 180 degrees.
inline V3 RotationErrorDeg(const Q4& from, const Q4& to) {
    Q4 d = QNormalized(QMul(to, QConj(from)));
    if (d.w < 0) d = {-d.x, -d.y, -d.z, -d.w};
    const double s = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (s < 1e-12) return {};
    const double angle = 2.0 * std::atan2(s, d.w) * 180.0 / 3.14159265358979323846;
    return {d.x / s * angle, d.y / s * angle, d.z / s * angle};
}

// `rot` turned by angVelDeg (deg/s, world frame) for ageMs, the age capped at kSoftMaxExtrapMs.
inline Q4 ExtrapolateRot(const Q4& rot, const V3& angVelDeg, double ageMs) {
    const double t = std::clamp(ageMs, 0.0, kSoftMaxExtrapMs) / 1000.0;
    const double rate = VLen(angVelDeg), deg = rate * t;
    if (deg < 1e-9) return rot;
    const double half = deg * 3.14159265358979323846 / 360.0;
    const double s = std::sin(half) / rate;
    return QNormalized(QMul(Q4{angVelDeg.x * s, angVelDeg.y * s, angVelDeg.z * s, std::cos(half)}, rot));
}
inline V3 ExtrapolatePos(const V3& pos, const V3& vel, double ageMs) {
    return VAdd(pos, VScale(vel, std::clamp(ageMs, 0.0, kSoftMaxExtrapMs) / 1000.0));
}

// ---------------------------------------------------------------- the correction
struct SoftTarget {   // the host's state of one body, already extrapolated to now
    V3 pos, vel;      // cm, cm/s
    Q4 rot;
    V3 angVel;        // deg/s
};
struct SoftSteer {
    V3 vel, angVel;           // what to set on the body
    double posErrCm = 0;      // before the dead band
    double rotErrDeg = 0;
    bool corrected = false;   // either error was past its dead band
};

// A vector of length `err` past the dead band, pushed at `gain`/s and capped: the extra velocity along dir.
inline V3 CorrectionAlong(const V3& err, double dead, double gain, double cap) {
    const double len = VLen(err);
    if (!(len > dead)) return {};
    const double speed = std::min((len - dead) * gain, cap);
    return VScale(err, speed / len);
}

// dtSec: the time since this body was steered last (the interval the correction acts over).
inline SoftSteer Steer(const V3& curPos, const Q4& curRot, const SoftTarget& t, double dtSec) {
    const double dt = std::clamp(std::isfinite(dtSec) ? dtSec : kSoftMinDtSec, kSoftMinDtSec, kSoftMaxDtSec);
    const double gain = std::min(kSoftGain, kSoftMaxStep / dt);
    SoftSteer out;
    const V3 posErr{t.pos.x - curPos.x, t.pos.y - curPos.y, t.pos.z - curPos.z};
    const V3 rotErr = RotationErrorDeg(curRot, t.rot);
    out.posErrCm = VLen(posErr);
    out.rotErrDeg = VLen(rotErr);
    out.vel = VAdd(t.vel, CorrectionAlong(posErr, kSoftDeadCm, gain, kSoftMaxPosCorr));
    out.angVel = VAdd(t.angVel, CorrectionAlong(rotErr, kSoftDeadDeg, gain, kSoftMaxAngCorr));
    out.corrected = out.posErrCm > kSoftDeadCm || out.rotErrDeg > kSoftDeadDeg;
    return out;
}

// A body inside the dead band whose host state and local state are both at rest keeps its own velocity: writing the host's
// (near zero) velocity on every state wakes a body the physics engine put to sleep, and a woken pile of planks jiggles. Until
// 0.8.0-pre29 every received state wrote it.
inline bool KeepLocalVelocity(const SoftSteer& st, const V3& hostVel, const V3& hostAngVel, const V3& localVel,
                              const V3& localAngVel) {
    return !st.corrected && VLen(hostVel) < kRestCmS && VLen(hostAngVel) < kRestDegS && VLen(localVel) < kRestCmS &&
           VLen(localAngVel) < kRestDegS;
}

// ---------------------------------------------------------------- when the whole structure is placed at once
// Tracks how long some body of a structure has been further than kStuckCm from its target.
struct StuckTimer {
    double sinceMs = -1;   // when the worst error first went past kStuckCm; < 0: it is not past it
    double Update(double nowMs, double worstErrCm) {
        if (!(worstErrCm > kStuckCm)) { sinceMs = -1; return 0; }
        if (sinceMs < 0 || nowMs < sinceMs) sinceMs = nowMs;
        return nowMs - sinceMs;
    }
};

// The states of all the structure's bodies make one usable snapshot: they arrived close together (ages in ms since each
// arrived: the oldest and the newest) and the newest is recent.
inline bool SnapReady(double oldestAgeMs, double newestAgeMs) {
    return newestAgeMs <= kSnapFreshMs && oldestAgeMs - newestAgeMs <= kSnapCoherentMs;
}

// worstErrCm: the largest position error of any body; stuckMs: StuckTimer::Update; sinceSnapMs: since this structure was last
// snapped (huge when never). Snap when the snapshot is usable, the gap has passed and the structure is badly off.
inline bool ShouldSnap(bool snapshotReady, double worstErrCm, double stuckMs, double sinceSnapMs) {
    return snapshotReady && sinceSnapMs >= kSnapGapMs && (worstErrCm > kSnapCm || stuckMs >= kStuckMs);
}

// A snap needs a structure that had time to settle since it was bound here.
inline bool SnapAllowed(double sinceSettledMs) { return sinceSettledMs >= kSnapSettleMs; }

}  // namespace hsm::sync
