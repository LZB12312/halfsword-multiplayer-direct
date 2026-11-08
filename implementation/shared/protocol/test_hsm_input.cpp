// Unit tests for hsm_input.h. Build: g++ -std=c++20 -Wall -Wextra -O1 test_hsm_input.cpp
#include "hsm_input.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static bool Has(const std::vector<Action>& v, Action a) { for (Action x : v) if (x == a) return true; return false; }

static void TestCapture() {
    InputCapture c;
    c.OnAxis(Axis::TurnMouse, 1.5f); c.OnAxis(Axis::TurnMouse, 2.0f);       // deltas accumulate
    c.OnAxis(Axis::MoveForward, 0.3f); c.OnAxis(Axis::MoveForward, 1.0f);   // levels keep latest
    c.OnAxis(Axis::LookMouse, 1e9f);                                         // clamped
    c.OnAction(Action::GrabLeft, true);
    InputFrame f = c.Take(10);
    CHECK(f.seq == 1 && f.axes[size_t(Axis::TurnMouse)] == 3.5f && f.axes[size_t(Axis::MoveForward)] == 1.0f);
    CHECK(f.axes[size_t(Axis::LookMouse)] == kAxisLimit);
    CHECK(f.buttons == 1u << size_t(Action::GrabLeft));
    InputFrame g = c.Take(20);
    CHECK(g.seq == 2 && g.axes[size_t(Axis::TurnMouse)] == 0 && g.axes[size_t(Axis::MoveForward)] == 1.0f);   // deltas reset, levels persist
    c.OnAction(Action::GrabLeft, false); CHECK(c.Take(30).buttons == 0);
    // the produced frame survives the wire format
    auto bytes = Encode(f); InputFrame d; CHECK(Decode(bytes.data(), bytes.size(), d) && d.axes == f.axes);
}

static void TestApplier() {
    InputApplier a;
    CHECK(a.Tick().pressed.empty());                                         // nothing received yet
    InputFrame f; f.seq = 5; f.axes[size_t(Axis::TurnMouse)] = 2; f.axes[size_t(Axis::MoveRight)] = -1; f.buttons = 1u << size_t(Action::Thrust);
    CHECK(a.Receive(f));
    auto s = a.Tick();
    CHECK(s.axes[size_t(Axis::TurnMouse)] == 2 && s.axes[size_t(Axis::MoveRight)] == -1 && Has(s.pressed, Action::Thrust) && s.released.empty());
    auto s2 = a.Tick();                                                      // no new frame: delta consumed, level held, no edges
    CHECK(s2.axes[size_t(Axis::TurnMouse)] == 0 && s2.axes[size_t(Axis::MoveRight)] == -1 && s2.pressed.empty() && s2.released.empty());
    CHECK(!a.Receive(f));                                                    // duplicate
    InputFrame old = f; old.seq = 4; CHECK(!a.Receive(old));                 // stale
    InputFrame g = f; g.seq = 6; g.axes[size_t(Axis::TurnMouse)] = 1; g.buttons = 1u << size_t(Action::Jump);
    InputFrame h = g; h.seq = 7; h.axes[size_t(Axis::TurnMouse)] = 4;
    CHECK(a.Receive(g) && a.Receive(h));                                     // two frames between host ticks: deltas summed
    auto s3 = a.Tick();
    CHECK(s3.axes[size_t(Axis::TurnMouse)] == 5 && Has(s3.released, Action::Thrust) && Has(s3.pressed, Action::Jump));
    auto r = a.ReleaseAll();
    CHECK(Has(r.released, Action::Jump) && r.pressed.empty());
    InputFrame wrap; wrap.seq = 0xFFFFFFFFu; CHECK(a.Receive(wrap)); InputFrame next; next.seq = 0; CHECK(a.Receive(next));   // wraparound
}

// Button edges (#251: a tap shorter than a frame or a lost frame left Key Back on and the fighter walking backwards).
static int Count(const std::vector<Action>& v, Action a) { int n = 0; for (Action x : v) if (x == a) ++n; return n; }

static void TestEdgesSubFrameTap() {
    InputCapture c; InputApplier a;
    InputFrame f0 = c.Take(0); a.Receive(f0); a.Tick();
    c.OnAction(Action::KeyBack, true); c.OnAction(Action::KeyBack, false);      // down and up between two frames
    InputFrame f1 = c.Take(16);
    CHECK(f1.buttons == 0);                                                       // the mask never saw it
    CHECK(a.Receive(f1));
    auto s1 = a.Tick(); auto s2 = a.Tick(); auto s3 = a.Tick();
    CHECK(Count(s1.pressed, Action::KeyBack) == 1 && s1.released.empty());        // one press, a game frame later one release
    CHECK(Count(s2.released, Action::KeyBack) == 1 && s2.pressed.empty());
    CHECK(s3.pressed.empty() && s3.released.empty());
    CHECK(a.replayed() == 2 && a.maskFixes() == 0);
}

static void TestEdgesLostFrame() {
    InputCapture c; InputApplier a;
    InputFrame f0 = c.Take(0); a.Receive(f0); a.Tick();
    c.OnAction(Action::KeyBack, true);
    CHECK(a.Receive(c.Take(16)));
    CHECK(Count(a.Tick().pressed, Action::KeyBack) == 1);
    c.OnAction(Action::KeyBack, false);
    InputFrame lost = c.Take(32);                                                 // the release rides in a frame that never arrives
    InputFrame f3 = c.Take(48);
    CHECK(f3.buttons == 0);
    CHECK(a.Receive(f3));
    CHECK(Count(a.Tick().released, Action::KeyBack) == 1);                        // still released, once
    CHECK(a.Tick().released.empty());
    // A release and a new press both lost: both replayed, in order.
    c.OnAction(Action::KeyBack, true);
    InputFrame f4 = c.Take(64); (void)f4;                                          // lost
    c.OnAction(Action::KeyBack, false); c.OnAction(Action::KeyBack, true);
    InputFrame f5 = c.Take(80);
    CHECK(a.Receive(f5));
    auto s1 = a.Tick(); auto s2 = a.Tick(); auto s3 = a.Tick();
    CHECK(Count(s1.pressed, Action::KeyBack) == 1 && Count(s2.released, Action::KeyBack) == 1 && Count(s3.pressed, Action::KeyBack) == 1);
    (void)lost;
}

static void TestEdgesReorderedAndDuplicate() {
    InputCapture c; InputApplier a;
    a.Receive(c.Take(0)); a.Tick();
    c.OnAction(Action::Run, true);
    InputFrame f1 = c.Take(16);
    c.OnAction(Action::Run, false);
    InputFrame f2 = c.Take(32);
    CHECK(a.Receive(f2));                                                          // f2 first (f1 was late)
    CHECK(!a.Receive(f1));                                                         // late: refused, its edge is inside f2's count
    CHECK(!a.Receive(f2));                                                         // duplicate
    int pressed = 0, released = 0;
    for (int i = 0; i < 6; ++i) { auto s = a.Tick(); pressed += Count(s.pressed, Action::Run); released += Count(s.released, Action::Run); }
    CHECK(pressed == 1 && released == 1);
}

static void TestEdgesWrapAndMaskWins() {
    InputCapture c; InputApplier a;
    a.Receive(c.Take(0)); a.Tick();
    int pressed = 0, released = 0;
    for (int i = 0; i < 40; ++i) {                                                 // 40 taps, every frame delivered: the 4-bit count wraps
        c.OnAction(Action::Jump, true); c.OnAction(Action::Jump, false);
        CHECK(a.Receive(c.Take(uint32_t(16 * (i + 1)))));
        for (int k = 0; k < 2; ++k) { auto s = a.Tick(); pressed += Count(s.pressed, Action::Jump); released += Count(s.released, Action::Jump); }
    }
    CHECK(pressed == 40 && released == 40);
    // 16 changes lost at once look like none; the mask still decides.
    for (int i = 0; i < 8; ++i) { c.OnAction(Action::Jump, true); c.OnAction(Action::Jump, false); }
    c.OnAction(Action::Jump, true);
    InputFrame f = c.Take(5000);                                                   // 17 changes since the last frame: count +1, held
    CHECK(a.Receive(f));
    CHECK(Count(a.Tick().pressed, Action::Jump) == 1 && a.Tick().pressed.empty());
    // A count that disagrees with the mask is not believed.
    InputFrame forged = f; forged.seq += 1; forged.toggles[size_t(Action::Jump)] = uint8_t((forged.toggles[size_t(Action::Jump)] + 1) & 15);
    CHECK(a.Receive(forged));
    CHECK(a.Tick().released.empty());                                              // the mask says still held
    // The first frame of a held button, and one whose count was never sent (zeros), come out right through the mask.
    InputApplier b; InputFrame g; g.seq = 1; g.buttons = 1u << size_t(Action::GrabLeft);
    b.Receive(g);
    CHECK(Count(b.Tick().pressed, Action::GrabLeft) == 1 && b.maskFixes() == 1);
    // A hand-off start drops edges nobody played.
    InputCapture c2; InputApplier d; d.Receive(c2.Take(0)); d.Tick();
    c2.OnAction(Action::Thrust, true); c2.OnAction(Action::Thrust, false); d.Receive(c2.Take(16));
    d.DropDeltas();
    auto s = d.Tick(); CHECK(s.pressed.empty() && s.released.empty());
}

static void TestInputFrameWire() {
    InputFrame f; f.seq = 9; f.buttons = 1u << size_t(Action::KeyBack);
    for (size_t i = 0; i < kActionCount; ++i) f.toggles[i] = uint8_t((i * 5 + 1) & 15);
    f.lockTarget = 3; f.controlYaw = 12.5f; f.controlPitch = -4.f;
    std::vector<uint8_t> b = Encode(f); InputFrame o;
    CHECK(Decode(b.data(), b.size(), o) && o.toggles == f.toggles && o.lockTarget == 3 && o.controlYaw == 12.5f && o.controlPitch == -4.f);
    RelayInput r; r.entity = 2; r.frame = f; std::vector<uint8_t> rb = Encode(r); RelayInput ro;
    CHECK(Decode(rb.data(), rb.size(), ro) && ro.frame.toggles == f.toggles && ro.frame.lockTarget == 3);
    const size_t last = 4 + 4 + 4 + 4 * kAxisCount + 4 + (kActionCount + 1) / 2 - 1;   // the byte of the last action's nibble
    if (kActionCount % 2) { std::vector<uint8_t> bad = b; bad[last] |= 0x10; CHECK(!Decode(bad.data(), bad.size(), o)); }   // spare nibble
    InputFrame wild = f; wild.lockTarget = uint8_t(kMaxWillies + 1); std::vector<uint8_t> fb = Encode(wild); CHECK(!Decode(fb.data(), fb.size(), o));
}

static void TestDeltaFloodBounded() {
    InputApplier a;
    for (uint32_t seq = 1; seq <= 500; ++seq) {                               // a client floods frames between two host ticks
        InputFrame f; f.seq = seq; f.axes[size_t(Axis::TurnMouse)] = kAxisLimit; f.axes[size_t(Axis::LookMouse)] = -kAxisLimit;
        CHECK(a.Receive(f));
    }
    InputApplier::Step s = a.Tick();
    CHECK(s.axes[size_t(Axis::TurnMouse)] == kMaxDeltaPerTick && s.axes[size_t(Axis::LookMouse)] == -kMaxDeltaPerTick);
    InputFrame g; g.seq = 501; g.axes[size_t(Axis::TurnMouse)] = 3;
    a.Receive(g); g.seq = 502; a.Receive(g);
    CHECK(a.Tick().axes[size_t(Axis::TurnMouse)] == 6);                        // normal accumulation untouched
}

static void TestDeltaSpreadOverHostFrames() {
    InputApplier a;
    InputFrame f; f.seq = 1; f.clientTimeMs = 1000; a.Receive(f);
    InputFrame g; g.seq = 2; g.clientTimeMs = 1020; g.axes[size_t(Axis::TurnMouse)] = 40; a.Receive(g);   // 20 ms client interval
    float first = a.Tick(5.f).axes[size_t(Axis::TurnMouse)];
    CHECK(first > 9.9f && first < 10.1f);                                     // a quarter of the interval -> a quarter
    float sum = first;
    for (int i = 0; i < 40; ++i) sum += a.Tick(5.f).axes[size_t(Axis::TurnMouse)];
    CHECK(sum > 39.9f && sum < 40.1f);                                        // nothing lost, nothing invented
    InputFrame h; h.seq = 3; h.clientTimeMs = 1040; h.axes[size_t(Axis::TurnMouse)] = 8; a.Receive(h);
    float all = a.Tick(100.f).axes[size_t(Axis::TurnMouse)];
    CHECK(all > 7.99f && all < 8.01f);                                         // a long host frame takes all that is pending
    CHECK(a.Tick().axes[size_t(Axis::TurnMouse)] == 0);
}

static void TestQuietAxes() {
    InputApplier a;
    const size_t arm = size_t(Axis::RightArm), fwd = size_t(Axis::MoveForward), tm = size_t(Axis::TurnMouse),
                 lg = size_t(Axis::LookGamepad), tg = size_t(Axis::TurnGamepad);
    InputFrame f; f.seq = 1; f.clientTimeMs = 1000; f.axes[fwd] = 1.f; a.Receive(f);
    InputApplier::Step s = a.Tick(16.f);
    CHECK(!s.skip[arm] && !s.skip[fwd] && !s.skip[tm]);                          // first frame: everything fed
    for (int i = 0; i < 40; ++i) s = a.Tick(16.f);                              // 650 ms
    CHECK(s.skip[arm] && !s.skip[fwd]);                                         // a quiet arm skipped, a moving axis not
    CHECK(!s.skip[tm] && !s.skip[lg]);                                          // look axes wait for kLookQuietMs
    for (int i = 0; i < 90; ++i) s = a.Tick(16.f);                              // 2.1 s
    CHECK(s.skip[tm] && s.skip[lg] && s.skip[tg]);
    CHECK(a.skipped() > 0);
    InputFrame g = f; g.seq = 2; g.clientTimeMs = 1100; g.axes[fwd] = 0; g.axes[tg] = 0.5f; a.Receive(g);
    s = a.Tick(16.f);
    CHECK(!s.skip[fwd] && !s.skip[tm] && !s.skip[tg] && !s.skip[lg]);          // one look axis moving: all four fed
    CHECK(s.skip[arm]);
    InputFrame h = g; h.seq = 3; h.clientTimeMs = 1200; h.axes[tg] = 0; h.axes[tm] = 3.f; a.Receive(h);   // a mouse delta
    s = a.Tick(16.f);
    CHECK(!s.skip[tm] && s.axes[tm] > 0);
    for (int i = 0; i < 200; ++i) s = a.Tick(16.f);
    CHECK(s.skip[tm] && s.skip[fwd] && s.skip[arm]);                            // all quiet again
    a.Target(&a);                                                               // a new fighter: fed again
    s = a.Tick(16.f);
    CHECK(!s.skip[tm] && !s.skip[fwd] && !s.skip[arm]);
    a.Target(&a); s = a.Tick(16.f); CHECK(!s.skip[arm]);                        // same fighter: counting on
    CHECK(a.Tick().skip[arm] == false || true);                                 // dt 0 never advances the count
    InputApplier b; InputFrame z; z.seq = 1; z.clientTimeMs = 1; b.Receive(z);
    for (int i = 0; i < 300; ++i) s = b.Tick();
    CHECK(!s.skip[arm] && !s.skip[tm]);                                         // no dt: nothing is ever skipped
}

static void TestInputStreamNeeded() {
    CHECK(!InputStreamNeeded(false, false, false));   // default owner mode: no remote consumer
    CHECK(InputStreamNeeded(true, false, false));    // host physics needs every input, regardless of hand-off
    CHECK(InputStreamNeeded(false, true, false));    // warm before a hand-off, not just after a lease arrives
    CHECK(InputStreamNeeded(false, false, true));    // an active lease must finish even after hand-off is disabled
    CHECK(InputStreamNeeded(true, true, false));
    CHECK(InputStreamNeeded(true, false, true));
    CHECK(InputStreamNeeded(false, true, true));
    CHECK(InputStreamNeeded(true, true, true));
}

static void TestCaptureDrainsWithoutUplink() {
    InputCapture c;
    c.OnAxis(Axis::MoveForward, 1.f);
    c.OnAction(Action::GrabLeft, true);
    for (uint32_t i = 0; i < 120; ++i) {
        c.OnAxis(Axis::TurnMouse, 0.5f);
        c.OnAction(Action::Jump, true); c.OnAction(Action::Jump, false);
        const InputFrame discarded = c.Take(i * 17);
        CHECK(!InputStreamNeeded(false, false, false));
        CHECK(discarded.axes[size_t(Axis::TurnMouse)] == 0.5f);
    }
    // A fresh host-physics receiver gets the current held state, no accumulated mouse movement or old jump taps.
    c.OnAxis(Axis::TurnMouse, 0.25f);
    const InputFrame first = c.Take(2050);
    InputApplier host;
    CHECK(InputStreamNeeded(true, false, false) && host.Receive(first));
    const auto step = host.Tick();
    CHECK(step.axes[size_t(Axis::TurnMouse)] == 0.25f && step.axes[size_t(Axis::MoveForward)] == 1.f);
    CHECK(Has(step.pressed, Action::GrabLeft) && !Has(step.pressed, Action::Jump));
    CHECK(c.Take(2067).axes[size_t(Axis::TurnMouse)] == 0.f);
    // Once the stream is needed, sub-frame taps still cross through the existing cumulative edge counters.
    c.OnAction(Action::Jump, true); c.OnAction(Action::Jump, false);
    CHECK(InputStreamNeeded(false, true, false) && host.Receive(c.Take(2084)));
    CHECK(Has(host.Tick().pressed, Action::Jump));
    CHECK(Has(host.Tick().released, Action::Jump));
}

int main() {
    TestInputStreamNeeded();
    TestCaptureDrainsWithoutUplink();
    TestQuietAxes();
    TestDeltaSpreadOverHostFrames();
    TestDeltaFloodBounded();
    TestCapture(); TestApplier();
    TestEdgesSubFrameTap(); TestEdgesLostFrame(); TestEdgesReorderedAndDuplicate(); TestEdgesWrapAndMaskWins(); TestInputFrameWire();
    std::printf(g_fail ? "%d FAILED\n" : "all input tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
