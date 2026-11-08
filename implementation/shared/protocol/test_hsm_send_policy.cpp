// SendFlushes: unreliable streams and a blow's reliable messages go now; other reliable traffic keeps Steam's batching.
#include "protocol/hsm_send_policy.h"
#include <cstdio>
#include <vector>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static bool Flushes(MsgType t, bool reliable) {
    Writer w;
    w.header(t);
    return SendFlushes(w.buf.data(), w.buf.size(), reliable);
}

static void TestBlowsFlush() {
    for (MsgType t : {MsgType::HitClaim, MsgType::HitFx, MsgType::SeverClaim, MsgType::SeverNotice, MsgType::StuckBlade})
        CHECK(Flushes(t, true));
}

static void TestOtherReliableBatches() {
    for (MsgType t : {MsgType::BlobChunk, MsgType::PropLease, MsgType::PropCensus, MsgType::PaintFx, MsgType::MatchInfo,
                      MsgType::Chat, MsgType::Equip, MsgType::GrabState, MsgType::ObjectEvent})
        CHECK(!Flushes(t, true));
}

static void TestStreamsFlush() {
    for (MsgType t : {MsgType::Pose, MsgType::Vitals, MsgType::BodyState, MsgType::Input, MsgType::Ping})
        CHECK(Flushes(t, false));
}

static void TestMalformed() {
    const std::vector<uint8_t> bad{0, 0, 0, 0};
    CHECK(!SendFlushes(bad.data(), bad.size(), true));
    CHECK(!SendFlushes(nullptr, 0, true));
    CHECK(SendFlushes(nullptr, 0, false));
}

int main() {
    TestBlowsFlush();
    TestOtherReliableBatches();
    TestStreamsFlush();
    TestMalformed();
    if (g_fail) { std::printf("test_hsm_send_policy: %d failures\n", g_fail); return 1; }
    std::printf("test_hsm_send_policy: ok\n");
    return 0;
}
