// The per-peer receive budget (M3): classes, burst and refill, a flood is dropped while normal traffic never is, classes
// are independent, free classes are never counted, the client's wider scale, a backwards clock, the log gate, TokenBucket.
#include "session/hsm_budget.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static void TestClasses() {
    CHECK(BudgetClassOf(MsgType::Pose) == BudgetClass::Stream && BudgetClassOf(MsgType::Vitals) == BudgetClass::Stream);
    CHECK(BudgetClassOf(MsgType::BodyState) == BudgetClass::Prop);
    CHECK(BudgetClassOf(MsgType::PropLease) == BudgetClass::Lease);
    CHECK(BudgetClassOf(MsgType::ObjectAnnounce) == BudgetClass::Lease && BudgetClassOf(MsgType::ObjectEvent) == BudgetClass::Lease);
    CHECK(BudgetClassOf(MsgType::PaintFx) == BudgetClass::Fx);
    CHECK(BudgetClassOf(MsgType::HitClaim) == BudgetClass::Combat && BudgetClassOf(MsgType::GrabState) == BudgetClass::Combat);
    CHECK(BudgetClassOf(MsgType::BlobChunk) == BudgetClass::Blob && BudgetClassOf(MsgType::Ready) == BudgetClass::Control);
    CHECK(BudgetClassOf(MsgType::ModMap) == BudgetClass::Control);
    CHECK(BudgetClassOf(MsgType::LevelSeed) == BudgetClass::Control);
    CHECK(BudgetClassOf(MsgType::LobbySecret) == BudgetClass::Control);
    CHECK(BudgetClassOf(MsgType::ModList) == BudgetClass::Control && BudgetClassOf(MsgType::ModAnswer) == BudgetClass::Control);
    CHECK(BudgetClassOf(MsgType::SessionMods) == BudgetClass::Control && BudgetClassOf(MsgType::ModChallenge) == BudgetClass::Control);
    CHECK(BudgetClassOf(MsgType::Forward) == BudgetClass::Detour);
    CHECK(std::string(MsgBudget::ClassName(BudgetClass::Detour)) == "detour");
    // Setup and already-bounded messages are never budgeted here.
    for (MsgType t : {MsgType::Hello, MsgType::Welcome, MsgType::Chat, MsgType::Ping, MsgType::Travel, MsgType::Spawn,
                      MsgType::MatchInfo, MsgType::Event, MsgType::PropCensus})
        CHECK(BudgetClassOf(t) == BudgetClass::Free);
}

static void TestFloodDropped() {
    MsgBudget b;
    int ok = 0;
    for (int i = 0; i < 100000; ++i) ok += b.Allow(MsgType::PaintFx, 1000.0);   // 100k in one instant
    CHECK(ok == 720);                                                           // exactly the burst
    CHECK(b.Dropped(BudgetClass::Fx) == 100000 - 720 && b.DroppedTotal() == b.Dropped(BudgetClass::Fx));
    // A second later the bucket holds a second's worth again.
    ok = 0;
    for (int i = 0; i < 1000; ++i) ok += b.Allow(MsgType::PaintFx, 2000.0);
    CHECK(ok == 360);
}

static void TestNormalPlayNeverDrops() {
    // One peer's legitimate maximum: Pose + Vitals at 60 Hz, 20 leased props at 60 Hz, 6 fx kinds at 30/s, claims 100/s.
    MsgBudget b;
    for (int frame = 0; frame < 60 * 30; ++frame) {   // 30 s at 60 Hz
        const double now = frame * (1000.0 / 60.0);
        CHECK(b.Allow(MsgType::Pose, now));
        CHECK(b.Allow(MsgType::Vitals, now));
        for (int p = 0; p < 20; ++p) CHECK(b.Allow(MsgType::BodyState, now));
        if (frame % 2 == 0) for (int k = 0; k < 6; ++k) CHECK(b.Allow(MsgType::PaintFx, now));   // 180/s
        if (frame % 36 == 0) CHECK(b.Allow(MsgType::HitClaim, now));
    }
    CHECK(b.DroppedTotal() == 0);
}

static void TestClassesIndependent() {
    MsgBudget b;
    for (int i = 0; i < 5000; ++i) b.Allow(MsgType::Pose, 0.0);
    CHECK(b.Dropped(BudgetClass::Stream) > 0);
    CHECK(b.Allow(MsgType::BodyState, 0.0) && b.Allow(MsgType::PaintFx, 0.0) && b.Allow(MsgType::HitClaim, 0.0));
    CHECK(b.Dropped(BudgetClass::Prop) == 0 && b.Dropped(BudgetClass::Fx) == 0);
    for (int i = 0; i < 5000; ++i) CHECK(b.Allow(MsgType::Hello, 0.0) && b.Allow(MsgType::Chat, 0.0));   // free
    CHECK(b.DroppedTotal() == b.Dropped(BudgetClass::Stream));
}

static void TestClientScale() {
    // What the host sends a client carries every fighter's streams: 16 x 60 Hz poses fit, a flood does not.
    MsgBudget c{double(kMaxWillies)};
    for (int frame = 0; frame < 60 * 10; ++frame)
        for (int e = 0; e < int(kMaxWillies); ++e) {
            CHECK(c.Allow(MsgType::Pose, frame * (1000.0 / 60.0)));
            CHECK(c.Allow(MsgType::Vitals, frame * (1000.0 / 60.0)));
        }
    CHECK(c.DroppedTotal() == 0);
    int ok = 0;
    for (int i = 0; i < 50000; ++i) ok += c.Allow(MsgType::Pose, 99999.0);
    CHECK(ok < 50000 && ok <= int(600 * kMaxWillies));
    MsgBudget bad(0.0), neg(-3.0);   // a nonsense scale falls back to 1
    CHECK(bad.Allow(MsgType::Pose, 0.0) && neg.Allow(MsgType::Pose, 0.0));
}

static void TestClockBackwards() {
    MsgBudget b;
    for (int i = 0; i < 700; ++i) b.Allow(MsgType::Ready, 5000.0);
    const uint64_t d = b.Dropped(BudgetClass::Control);
    CHECK(d > 0);
    CHECK(!b.Allow(MsgType::Ready, 1000.0));       // time went back: nothing refills
    CHECK(!b.Allow(MsgType::Ready, 1000.0));
    CHECK(b.Allow(MsgType::Ready, 1000.0 + 1000.0));   // and counts on from the new clock
}

static void TestShouldLog() {
    CHECK(!MsgBudget::ShouldLog(0));
    CHECK(MsgBudget::ShouldLog(1) && MsgBudget::ShouldLog(2) && MsgBudget::ShouldLog(3));
    CHECK(!MsgBudget::ShouldLog(4) && !MsgBudget::ShouldLog(1999));
    CHECK(MsgBudget::ShouldLog(2000) && MsgBudget::ShouldLog(4000));
    CHECK(std::string(MsgBudget::ClassName(BudgetClass::Fx)) == "fx" && std::string(MsgBudget::ClassName(BudgetClass::Count)) == "free");
}

static void TestTokenBucket() {
    TokenBucket t(2.0, 4.0);   // 2 per second, burst 4
    int ok = 0;
    for (int i = 0; i < 10; ++i) ok += t.Allow(0.0);
    CHECK(ok == 4);
    CHECK(!t.Allow(100.0));          // 0.2 of a token
    CHECK(t.Allow(600.0));           // one whole token by now
    CHECK(!t.Allow(600.0));
    ok = 0;
    for (int i = 0; i < 10; ++i) ok += t.Allow(60000.0);   // a long wait refills only the burst
    CHECK(ok == 4);
}

int main() {
    TestClasses();
    TestFloodDropped();
    TestNormalPlayNeverDrops();
    TestClassesIndependent();
    TestClientScale();
    TestClockBackwards();
    TestShouldLog();
    TestTokenBucket();
    if (g_fail) { std::printf("%d failures\n", g_fail); return 1; }
    std::printf("hsm_budget tests passed\n");
    return 0;
}
