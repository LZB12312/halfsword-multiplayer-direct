// SendCadence: the long-run rate matches the configured rate at frame rates above and below it, float noise in dt
// does not halve the rate, and a long frame sends once without a burst after it.
#include "util/hsm_cadence.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using hsm::util::SendCadence;

// Sends over seconds of frames of dtS each.
static int Sends(double fps, double hz, double seconds, double noise = 0.0) {
    SendCadence c;
    int n = 0;
    const int frames = int(fps * seconds);
    for (int i = 0; i < frames; ++i) {
        const double jitter = noise * ((i % 2) ? 1.0 : -1.0);
        if (c.Due(1.0 / fps + jitter, hz)) ++n;
    }
    return n;
}

static void TestRateAboveFrameRate() {
    // 144 fps at 60 Hz: 600 sends in 10 s (the old reset-to-0 gave 480, every third frame).
    const int n = Sends(144, 60, 10);
    CHECK(n >= 598 && n <= 600);
    // 90 fps: 600 (the old rule gave 450).
    const int m = Sends(90, 60, 10);
    CHECK(m >= 598 && m <= 600);
}

static void TestNoiseAtMatchingRate() {
    // 60 fps with +-0.1 ms of frame noise: every frame sends; the old rule dropped to about 30 Hz on the short frames.
    const int n = Sends(60, 60, 10, 0.0001);
    CHECK(n >= 595 && n <= 600);
}

static void TestSlowFramesSendEachFrame() {
    // 30 fps at 60 Hz: one send per frame, never more.
    SendCadence c;
    int n = 0;
    for (int i = 0; i < 300; ++i) n += c.Due(1.0 / 30.0, 60) ? 1 : 0;
    CHECK(n == 300);
}

static void TestNoBurstAfterHitch() {
    SendCadence c;
    CHECK(c.Due(0.5, 60));                 // a 500 ms hitch: one send
    int n = 0;
    for (int i = 0; i < 4; ++i) n += c.Due(1.0 / 240.0, 60) ? 1 : 0;   // the next 4 fast frames (16.7 ms)
    CHECK(n == 1);                         // at most the one carried period, not 30 queued sends
}

static void TestBadInput() {
    SendCadence c;
    CHECK(!c.Due(0.1, 0));
    CHECK(!c.Due(-1.0, 60));
    CHECK(c.Due(1.0 / 60.0, 60));
    c.Reset();
    CHECK(!c.Due(0.001, 60));
}

int main() {
    TestRateAboveFrameRate();
    TestNoiseAtMatchingRate();
    TestSlowFramesSendEachFrame();
    TestNoBurstAfterHitch();
    TestBadInput();
    if (g_fail) { std::printf("test_hsm_cadence: %d failures\n", g_fail); return 1; }
    std::printf("test_hsm_cadence: ok\n");
    return 0;
}
