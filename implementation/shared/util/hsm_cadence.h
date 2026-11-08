// A fixed send rate driven by frame time. Each frame adds its dt; a send is due once a period has built up, and the
// remainder is carried to the next period, so the long-run rate is the configured rate rather than the frame rate
// rounded down to whole frames. A frame longer than a period sends once and carries at most one period (no burst
// after a hitch).
//
// Before 0.8.0-pre26 the input senders reset the sum to 0 on every send: at 144 fps a 60 Hz rate became about 48 Hz
// (every third frame), and at 60 fps float noise in dt made it flip between 60 and 30 Hz.
#pragma once
#include <algorithm>

namespace hsm::util {

class SendCadence {
public:
    // dtS: this frame's length in seconds; hz: the wanted rate. True when a send is due this frame.
    bool Due(double dtS, double hz) {
        if (!(hz > 0.0) || !(dtS >= 0.0)) return false;
        const double period = 1.0 / hz;
        accS_ += dtS;
        if (accS_ < period) return false;
        accS_ = std::min(accS_ - period, period);
        return true;
    }
    void Reset() { accS_ = 0.0; }

private:
    double accS_ = 0.0;
};

}  // namespace hsm::util
