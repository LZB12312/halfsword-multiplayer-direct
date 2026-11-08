// Allocation budgets for the hot sync codecs. Packet contents and malformed inputs are covered by the owner,
// pose-codec and lease suites; this catches accidental payload copies and per-byte vector growth returning.
#include "hsm_owner.h"
#include "session/hsm_lease.h"
#include <cstdio>
#include <cstdlib>
#include <new>

static bool g_count = false;
static size_t g_allocs = 0;
void* operator new(std::size_t n) {
    if (g_count) ++g_allocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
static void Begin() { g_allocs = 0; g_count = true; }
static size_t End() { g_count = false; return g_allocs; }

int main() {
    using namespace hsm;
    sync::PoseFrame frame; frame.tsMs = 1000; frame.stepUs = 16667;
    Begin();
    PoseMsg pose; pose.entity = 1; pose.pose = sync::EncodePose(frame);
    const auto packet = Encode(pose);
    const size_t encodeAllocs = End();
    CHECK(encodeAllocs <= 2);   // one payload buffer, one envelope; no bit-writer buffer or return-value copy
    CHECK(packet.size() == kPoseMsgHeaderBytes + pose.pose.size());

    Begin();
    sync::EncodePoseInto(frame, pose.pose);
    CHECK(End() == 0);          // a caller keeping its payload buffer reuses the complete allocation

    PoseMsg envelope;
    sync::PoseFrame decoded;
    Begin();
    const bool admitted = DecodePoseEnvelope(packet.data(), packet.size(), envelope);
    const bool accepted = DecodePoseFrame(packet.data(), packet.size(), envelope, decoded);
    const size_t decodeAllocs = End();
    CHECK(admitted && accepted && envelope.pose.empty());
    CHECK(decoded.tsMs == frame.tsMs && decoded.stepUs == frame.stepUs && decodeAllocs == 0);

    BodyStateMsg body; body.objId = MakeLevelPropId(1);
    Begin();
    const auto state = Encode(body);
    const size_t bodyAllocs = End();
    CHECK(state.size() == 62 && bodyAllocs <= 1);
    BodyStateMsg read;
    CHECK(Decode(state.data(), state.size(), read) && read.objId == body.objId);
    std::printf("sync allocations: pose+envelope %zu, admission+decode %zu, body %zu\n", encodeAllocs, decodeAllocs, bodyAllocs);
    return g_fail ? 1 : 0;
}
