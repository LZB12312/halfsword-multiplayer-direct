// Limits the receiver holds, whatever the sender runs. Each case is a way one player could crash the others.
#include "session/hsm_incoming_guard.h"
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace hsm;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

int main() {
    // Spawning: a burst gets through, a flood doesn't, and time lets it recover.
    SpawnGate gate;
    int allowed = 0;
    for (int i = 0; i < 1000; ++i) if (gate.Check(1, 0.0) == SpawnGate::Verdict::Allowed) ++allowed;
    CHECK(allowed == int(kSpawnBurst));
    CHECK(gate.Check(1, 0.0) == SpawnGate::Verdict::TooFast);
    CHECK(gate.Check(2, 0.0) == SpawnGate::Verdict::Allowed);       // another player isn't affected
    CHECK(gate.Check(1, 1000.0) == SpawnGate::Verdict::Allowed);
    // Live objects: capped per player, and removing one makes room.
    SpawnGate many;
    for (int i = 0; i < kMaxLiveSpawnsPerSlot; ++i) many.OnCreated(uint64_t(i + 1), 3);
    CHECK(many.Live(3) == kMaxLiveSpawnsPerSlot && many.Check(3, 100000.0) == SpawnGate::Verdict::TooMany);
    many.OnGone(1); many.OnGone(1); many.OnGone(999999);            // twice, and one it never knew: neither miscounts
    CHECK(many.Live(3) == kMaxLiveSpawnsPerSlot - 1 && many.Check(3, 100000.0) == SpawnGate::Verdict::Allowed);
    many.Reset(); CHECK(many.Live(3) == 0);

    // A blow: ordinary numbers pass, and heavy HS Enhancer ones do too.
    combat::HitClaim c;
    CHECK(ClaimValuesSane(c));
    c.cuttingPower = 5000; c.hitImpulse.x = 1e6; c.location.x = 40;
    CHECK(ClaimValuesSane(c));
    const auto with = [](auto set) { combat::HitClaim h; set(h); return ClaimValuesSane(h); };
    CHECK(!with([](combat::HitClaim& h) { h.cuttingPower = 1e30; }));
    CHECK(!with([](combat::HitClaim& h) { h.rawDamage = -1e30; }));
    CHECK(!with([](combat::HitClaim& h) { h.hitImpulse.z = 1e20; }));
    CHECK(!with([](combat::HitClaim& h) { h.hitVelocity.y = std::numeric_limits<double>::quiet_NaN(); }));
    CHECK(!with([](combat::HitClaim& h) { h.location.x = 1e7; }));
    CHECK(!with([](combat::HitClaim& h) { h.propVelocity.x = 1e20; }));
    // 16 shots to the head in a second is nowhere near the limit. A flood is, for that attacker alone.
    BlowGate blows;
    int landed = 0;
    for (int i = 0; i < 16; ++i) if (blows.Allow(1, double(i) * 60.0)) ++landed;
    CHECK(landed == 16);
    BlowGate flood;
    landed = 0;
    for (int i = 0; i < 5000; ++i) if (flood.Allow(1, 0.0)) ++landed;
    CHECK(landed == int(kBlowBurst) && flood.Allow(2, 0.0));
    std::printf("ok\n");
    return 0;
}
