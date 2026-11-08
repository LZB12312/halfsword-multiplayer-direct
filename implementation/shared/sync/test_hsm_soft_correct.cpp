// Soft correction of a streamed structure body (sync/hsm_soft_correct.h): the receiving copy is steered, never teleported.
// Converges on a swinging target without overshoot, holds the host's velocity inside the dead band, clamps a big error,
// takes the shortest arc, and snaps the whole structure only when it is badly off, usable and not snapped lately.
#include "sync/hsm_soft_correct.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm::sync;

static const double kPi = 3.14159265358979323846;

static Q4 AboutZ(double deg) { const double h = deg * kPi / 360.0; return {0, 0, std::sin(h), std::cos(h)}; }
static double YawDeg(const Q4& q) { return 2.0 * std::atan2(q.z, q.w) * 180.0 / kPi; }

static void TestPendulumConvergesWithoutOvershoot() {
    // A bridge plank swinging like a pendulum: x = 80 sin(2 pi 0.5 t) cm, host velocity exact. The copy starts 100 cm off, is
    // steered every 50 ms (the structure stream) and moves by its commanded velocity. While the error is well past the dead band
    // it only shrinks and never changes sign; the copy settles within a few cm (the dead band plus the integration step's drift).
    const double w = 2 * kPi * 0.5, A = 80.0, dt = 0.05;
    V3 cur{100.0, 0, 0};
    bool grew = false, crossed = false;
    double lastErr = 0, t = 0;
    for (int i = 0; i < 200; ++i) {
        SoftTarget tg;
        tg.pos = {A * std::sin(w * t), 0, 0};
        tg.vel = {A * w * std::cos(w * t), 0, 0};
        tg.rot = AboutZ(0);
        const double errBefore = tg.pos.x - cur.x;
        const SoftSteer s = Steer(cur, AboutZ(0), tg, dt);
        cur.x += s.vel.x * dt;                                   // the physics step moves the body by its velocity
        t += dt;
        const double errAfter = A * std::sin(w * t) - cur.x;
        if (std::fabs(errBefore) > 6.0) {
            if (std::fabs(errAfter) > std::fabs(errBefore)) grew = true;
            if (errBefore * errAfter < 0) crossed = true;
        }
        lastErr = std::fabs(errAfter);
    }
    CHECK(!grew);
    CHECK(!crossed);
    CHECK(lastErr <= 5.0);
}

static void TestIrregularIntervalsStillNoOvershoot() {
    // A late packet (dt up to half a second) must not ask for more than the error.
    V3 cur{0, 0, 0};
    SoftTarget tg;
    tg.pos = {50, 0, 0};
    tg.rot = AboutZ(0);
    for (double dt : {0.016, 0.05, 0.1, 0.25, 0.5, 2.0}) {
        const SoftSteer s = Steer(cur, AboutZ(0), tg, dt);
        const double d = std::min(dt, kSoftMaxDtSec);
        CHECK(s.vel.x * d <= 50.0 - kSoftDeadCm + 1e-9);   // never past the target
        CHECK(s.vel.x > 0);
    }
}

static void TestDeadBandKeepsHostVelocity() {
    SoftTarget tg;
    tg.pos = {1.5, 0, 0};            // inside 2 cm
    tg.vel = {10, -20, 30};
    tg.rot = AboutZ(1.5);            // inside 2 deg
    tg.angVel = {1, 2, 3};
    const SoftSteer s = Steer({0, 0, 0}, AboutZ(0), tg, 0.05);
    CHECK(!s.corrected);
    CHECK(s.vel.x == 10 && s.vel.y == -20 && s.vel.z == 30);
    CHECK(s.angVel.x == 1 && s.angVel.y == 2 && s.angVel.z == 3);
    tg.pos = {10, 0, 0};             // past it: steered
    const SoftSteer b = Steer({0, 0, 0}, AboutZ(0), tg, 0.05);
    CHECK(b.corrected);
    CHECK(b.vel.x > 10);
    CHECK(std::fabs(b.vel.y + 20) < 1e-9);   // only along the error
}

static void TestCorrectionIsClamped() {
    SoftTarget tg;
    tg.pos = {0, 1000, 0};
    tg.vel = {0, 5, 0};
    tg.rot = AboutZ(170);
    const SoftSteer s = Steer({0, 0, 0}, AboutZ(0), tg, 0.05);
    CHECK(std::fabs((s.vel.y - 5.0) - kSoftMaxPosCorr) < 1e-6);
    CHECK(std::fabs(s.angVel.z - kSoftMaxAngCorr) < 1e-6);
}

static void TestAngularConvergesLikeAPendulum() {
    // Swinging about z by 40 deg at 0.5 Hz, the copy starts 90 deg off; same rules as the position.
    const double w = 2 * kPi * 0.5, A = 40.0, dt = 0.05;
    Q4 cur = AboutZ(90);
    double t = 0, lastErr = 0;
    bool grew = false, crossed = false;
    for (int i = 0; i < 200; ++i) {
        SoftTarget tg;
        tg.rot = AboutZ(A * std::sin(w * t));
        tg.angVel = {0, 0, A * w * std::cos(w * t)};
        const double errBefore = RotationErrorDeg(cur, tg.rot).z;
        const SoftSteer s = Steer({0, 0, 0}, cur, tg, dt);
        cur = ExtrapolateRot(cur, s.angVel, dt * 1000.0);        // the physics step turns the body by its angular velocity
        t += dt;
        const double errAfter = RotationErrorDeg(cur, AboutZ(A * std::sin(w * t))).z;
        if (std::fabs(errBefore) > 5.0) {
            if (std::fabs(errAfter) > std::fabs(errBefore)) grew = true;
            if (errBefore * errAfter < 0) crossed = true;
        }
        lastErr = std::fabs(errAfter);
    }
    CHECK(!grew);
    CHECK(!crossed);
    CHECK(lastErr <= 4.0);
}

static void TestShortestArc() {
    // 350 deg is -10 deg: from 10 deg the turn is -20, not +340.
    V3 e = RotationErrorDeg(AboutZ(10), AboutZ(350));
    CHECK(std::fabs(e.z + 20.0) < 1e-6 && std::fabs(e.x) < 1e-9 && std::fabs(e.y) < 1e-9);
    // The same orientation by the other sign of the quaternion is no error at all.
    const Q4 q = AboutZ(77);
    e = RotationErrorDeg(q, Q4{-q.x, -q.y, -q.z, -q.w});
    CHECK(VLen(e) < 1e-6);
    // Never more than half a turn.
    e = RotationErrorDeg(AboutZ(0), AboutZ(179));
    CHECK(e.z > 0 && e.z < 180.0);
    e = RotationErrorDeg(AboutZ(0), AboutZ(181));
    CHECK(e.z < 0 && e.z > -180.0);
    // Steering across the wrap goes the short way.
    SoftTarget tg;
    tg.rot = AboutZ(-170);
    const SoftSteer s = Steer({0, 0, 0}, AboutZ(170), tg, 0.05);
    CHECK(s.angVel.z > 0 && std::fabs(s.rotErrDeg - 20.0) < 1e-6);
}

static void TestExtrapolation() {
    const V3 p = ExtrapolatePos({0, 0, 0}, {100, 0, 0}, 100.0);
    CHECK(std::fabs(p.x - 10.0) < 1e-9);
    CHECK(std::fabs(ExtrapolatePos({0, 0, 0}, {100, 0, 0}, 5000.0).x - 25.0) < 1e-9);   // capped at 250 ms
    CHECK(std::fabs(ExtrapolatePos({0, 0, 0}, {100, 0, 0}, -50.0).x) < 1e-9);          // never backwards
    const Q4 r = ExtrapolateRot(AboutZ(10), {0, 0, 90}, 100.0);                          // 9 deg more
    CHECK(std::fabs(YawDeg(r) - 19.0) < 1e-6);
    const Q4 still = ExtrapolateRot(AboutZ(10), {0, 0, 0}, 100.0);
    CHECK(std::fabs(YawDeg(still) - 10.0) < 1e-9);
}

static void TestSnapRule() {
    const bool ok = SnapReady(40, 10);
    CHECK(ok);
    CHECK(!ShouldSnap(ok, 100, 0, 1e9));                    // within 150 cm and not stuck: stay soft
    CHECK(ShouldSnap(ok, kSnapCm + 1, 0, 1e9));             // any body far off: place all
    CHECK(!ShouldSnap(ok, kSnapCm + 1, 0, 1000));           // ... but not twice within 5 s
    CHECK(ShouldSnap(ok, kSnapCm + 1, 0, kSnapGapMs));
    CHECK(!ShouldSnap(false, 500, 9999, 1e9));              // no usable snapshot: never
    CHECK(ShouldSnap(ok, 70, kStuckMs, 1e9));               // stuck far off for 2 s
    CHECK(!ShouldSnap(ok, 70, kStuckMs - 1, 1e9));
}

static void TestSnapshotReady() {
    CHECK(SnapReady(20, 5));
    CHECK(SnapReady(305, 10));                  // 295 apart
    CHECK(!SnapReady(400, 10));                 // 390 apart: one body's state is from an older tick
    CHECK(!SnapReady(900, 800));                // nothing has arrived for 800 ms: stale
    CHECK(SnapReady(0, 0));
}

static void TestStuckTimer() {
    StuckTimer s;
    CHECK(s.Update(0, 30) == 0);
    CHECK(s.Update(100, 80) == 0);              // starts now
    CHECK(s.Update(1100, 80) == 1000);
    CHECK(s.Update(2100, 61) >= kStuckMs);
    CHECK(s.Update(2200, 10) == 0);             // back near: the clock resets
    CHECK(s.Update(2300, 80) == 0);
    CHECK(s.Update(2400, 80) == 100);
}

static void TestRestingBodiesKeepTheirVelocity() {
    SoftTarget t{};
    t.rot = Q4{};
    const V3 still{}, slow{1, 0, 0}, moving{50, 0, 0}, spin{0, 0, 20};
    SoftSteer in = Steer(V3{0.5, 0, 0}, Q4{}, t, 0.05);              // 0.5 cm off: inside the dead band
    CHECK(!in.corrected);
    CHECK(KeepLocalVelocity(in, still, still, still, still));      // asleep on both machines: not woken
    CHECK(KeepLocalVelocity(in, slow, still, slow, still));        // drifting under kRestCmS is still at rest
    CHECK(!KeepLocalVelocity(in, moving, still, still, still));    // the host's copy moves: follow it
    CHECK(!KeepLocalVelocity(in, still, still, moving, still));    // ours moves where the host's rests: stop it
    CHECK(!KeepLocalVelocity(in, still, spin, still, still));      // turning counts too
    SoftSteer off = Steer(V3{30, 0, 0}, Q4{}, t, 0.05);              // 30 cm off: corrected, whatever the speeds
    CHECK(off.corrected);
    CHECK(!KeepLocalVelocity(off, still, still, still, still));
}

static void TestSnapNeedsBodiesAndASettledCopy() {
    CHECK(!SnapAllowed(kSnapSettleMs - 1));                        // just bound: its joints and anchors still settle
    CHECK(SnapAllowed(kSnapSettleMs));
    CHECK(SnapAllowed(1e18));
}

int main() {
    TestRestingBodiesKeepTheirVelocity();
    TestSnapNeedsBodiesAndASettledCopy();
    TestPendulumConvergesWithoutOvershoot();
    TestIrregularIntervalsStillNoOvershoot();
    TestDeadBandKeepsHostVelocity();
    TestCorrectionIsClamped();
    TestAngularConvergesLikeAPendulum();
    TestShortestArc();
    TestExtrapolation();
    TestSnapRule();
    TestSnapshotReady();
    TestStuckTimer();
    if (g_fail) { std::printf("test_hsm_soft_correct: %d check(s) failed\n", g_fail); return 1; }
    std::printf("test_hsm_soft_correct: all checks passed\n");
    return 0;
}
