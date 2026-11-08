// Input bookkeeping shared by client (capture) and host (apply). Pure logic, unit-tested.
#pragma once
#include "hsm_protocol.h"
#include <algorithm>
#include <vector>

namespace hsm {

// Keep the input stream warm before a possible hand-off, and until an active hand-off has returned even if the
// option was disabled meanwhile. Owner mode without hand-off has no remote consumer; capture still drains locally.
inline bool InputStreamNeeded(bool hostPhysics, bool handOffEnabled, bool handed) {
    return hostPhysics || handOffEnabled || handed;
}

// Mouse axes are per-frame deltas and must be summed between sends; the rest are levels.
inline bool IsDeltaAxis(Axis a) { return a == Axis::TurnMouse || a == Axis::LookMouse; }
constexpr float kMaxDeltaPerTick = 4 * kAxisLimit;   // a few client frames per host frame; bounds a flood
// Quiet axes (0.8.0-pre14): an axis event the game need not see. Willie_BP's axis handlers (decomp Willie_BP.cpp
// 11290-11560) store the value and interpolate towards it (arms FInterpTo 50/s, gamepad sticks 30/s, mouse 25/s),
// the move handlers add it to the movement input (0: nothing), the guard handlers run their release once (a DoOnce;
// the next 0 returns at once). So an axis fed 0 for kQuietMs has nothing left to do. The four look handlers each apply
// the other three's stored values too and run Update Aim, whose aim vector converges at 50 uu/s or more over at most
// 90 uu: they go quiet together, once every look axis has been 0 for kLookQuietMs.
constexpr float kQuietMs = 500.f, kLookQuietMs = 2000.f;
inline bool IsLookAxis(Axis a) { return a == Axis::TurnMouse || a == Axis::LookMouse || a == Axis::TurnGamepad || a == Axis::LookGamepad; }

class InputCapture {
public:
    void OnAxis(Axis a, float v) {
        if (!std::isfinite(v)) return;
        float& slot = axes_[size_t(a)];
        slot = IsDeltaAxis(a) ? Clamp(slot + v) : Clamp(v);
    }
    // Every change of state is an edge, counted per action (mod 16, never reset, sent with each frame): the host replays
    // the edges a lost frame or a tap shorter than a frame would hide from the held mask (InputApplier).
    void OnAction(Action a, bool pressed) {
        uint32_t bit = 1u << size_t(a);
        if (((buttons_ & bit) != 0) == pressed) return;   // already so: no edge
        toggles_[size_t(a)] = uint8_t((toggles_[size_t(a)] + 1) & 15);
        buttons_ = pressed ? (buttons_ | bit) : (buttons_ & ~bit);
    }
    // Builds the next frame and resets accumulated deltas.
    InputFrame Take(uint32_t timeMs) {
        InputFrame f; f.seq = ++seq_; f.clientTimeMs = timeMs; f.axes = axes_; f.buttons = buttons_; f.toggles = toggles_;
        for (size_t i = 0; i < kAxisCount; ++i) if (IsDeltaAxis(Axis(i))) axes_[i] = 0;
        return f;
    }
    uint32_t buttons() const { return buttons_; }
private:
    static float Clamp(float v) { return v > kAxisLimit ? kAxisLimit : v < -kAxisLimit ? -kAxisLimit : v; }
    std::array<float, kAxisCount> axes_{};
    uint32_t buttons_ = 0;
    std::array<uint8_t, kActionCount> toggles_{};
    uint32_t seq_ = 0;
};

class InputApplier {
public:
    struct Step {
        std::array<float, kAxisCount> axes{};   // values to feed this game frame
        std::vector<Action> pressed, released;  // edges to fire this game frame (at most one per action: a tap shows for a frame)
        std::array<bool, kAxisCount> skip{};    // quiet axes: axes[i] is 0, the game has it, do not feed it
    };
    // The fighter the steps go to: a new one (a respawn, a hand-off) starts with every axis handler unrun, so its
    // first zeros are fed (the guard handlers' release branch runs once on the first 0).
    void Target(const void* w) { if (w != target_) { target_ = w; quietMs_.fill(0); } }
    // Accepts a received frame; returns false for stale/duplicate sequence numbers.
    bool Receive(const InputFrame& f) {
        if (have_ && !SeqNewer(f.seq, latest_.seq)) return false;
        if (have_) { uint32_t iv = f.clientTimeMs - latest_.clientTimeMs; if (iv >= 1 && iv <= 200) intervalMs_ = float(iv); }
        for (size_t i = 0; i < kAxisCount; ++i)
            if (IsDeltaAxis(Axis(i))) pendingDelta_[i] = std::clamp(pendingDelta_[i] + f.axes[i], -kMaxDeltaPerTick, kMaxDeltaPerTick);
        // Edges: each frame carries the running count of every action's changes, so what a lost frame held comes with the
        // next one, and a duplicate or late frame (refused above) adds nothing twice.
        if (have_)
            for (size_t i = 0; i < kActionCount; ++i)
                pending_[i] = uint8_t(std::min(15, pending_[i] + ((f.toggles[i] - lastToggles_[i]) & 15)));
        lastToggles_ = f.toggles;
        latest_ = f; have_ = true; return true;
    }
    // Called once per game frame for this player. With dtMs, mouse deltas are spread over the frames until the
    // sender's next frame (the game reads them as per-frame rates: one spike then zeros is a wrist flick, not a
    // swing); without it they are applied at once.
    Step Tick(float dtMs = 0) {
        Step s;
        if (!have_) return s;
        float portion = dtMs > 0 ? std::min(1.f, dtMs / intervalMs_) : 1.f;
        for (size_t i = 0; i < kAxisCount; ++i) {
            if (!IsDeltaAxis(Axis(i))) { s.axes[i] = latest_.axes[i]; continue; }
            // The spread is geometric: the last hundredth of a unit goes in one piece, so the axis reaches 0 (quiet).
            s.axes[i] = portion >= 1.f || std::fabs(pendingDelta_[i]) < 0.01f ? pendingDelta_[i] : pendingDelta_[i] * portion;
            pendingDelta_[i] -= s.axes[i];
        }
        bool lookQuiet = true;
        for (size_t i = 0; i < kAxisCount; ++i) {
            const bool zero = s.axes[i] == 0.f;
            if (IsLookAxis(Axis(i))) lookQuiet = lookQuiet && zero && quietMs_[i] >= kLookQuietMs;
            else s.skip[i] = zero && quietMs_[i] >= kQuietMs;
        }
        for (size_t i = 0; i < kAxisCount; ++i) {
            if (IsLookAxis(Axis(i))) s.skip[i] = lookQuiet;
            if (s.skip[i]) ++skipped_;
            quietMs_[i] = s.axes[i] == 0.f ? std::min(quietMs_[i] + dtMs, 1e6f) : 0.f;
        }
        // One edge a game frame per action, in order (press, release, press...): a tap shorter than a frame still shows for
        // one. The held mask is the truth: edges that would leave an action other than the mask says are dropped, and an
        // action that differs with no edge left is set right at once (the first frame, a flood past the count).
        for (size_t i = 0; i < kActionCount; ++i) {
            const uint32_t bit = 1u << i;
            const bool want = (latest_.buttons & bit) != 0, held = (applied_ & bit) != 0;
            uint8_t& p = pending_[i];
            if (p && (held != ((p & 1) != 0)) != want) p = 0;
            if (p) { --p; ++replayed_; }
            else if (held != want) ++maskFixes_;
            else continue;
            (held ? s.released : s.pressed).push_back(Action(i));
            applied_ ^= bit;
        }
        return s;
    }
    // Mouse movement received while nothing played it (a hand-off starts): dropped, not turned in one frame.
    void DropDeltas() { pendingDelta_.fill(0); pending_.fill(0); }
    // Releases everything that is held (disconnect / despawn), so the Willie does not keep gripping.
    Step ReleaseAll() {
        Step s;
        for (size_t i = 0; i < kActionCount; ++i) if (applied_ & (1u << i)) s.released.push_back(Action(i));
        applied_ = 0; latest_ = InputFrame{}; pendingDelta_.fill(0); pending_.fill(0); have_ = false; quietMs_.fill(0); target_ = nullptr;
        return s;
    }
    uint32_t skipped() const { return skipped_; }   // axis events not fed since the start
    uint32_t replayed() const { return replayed_; }   // button edges played from the counts
    uint32_t maskFixes() const { return maskFixes_; } // button edges played because the held mask differed with none left to play
    uint32_t lastSeq() const { return latest_.seq; }
    const InputFrame& latest() const { return latest_; }   // control rotation of the newest frame
private:
    InputFrame latest_{};
    bool have_ = false;
    uint32_t applied_ = 0;
    std::array<uint8_t, kActionCount> lastToggles_{}, pending_{};   // the sender's counts last seen; edges received, not yet played
    uint32_t replayed_ = 0, maskFixes_ = 0;
    std::array<float, kAxisCount> pendingDelta_{};
    std::array<float, kAxisCount> quietMs_{};   // how long each axis has been fed 0 (skipped frames count)
    const void* target_ = nullptr;
    uint32_t skipped_ = 0;
    float intervalMs_ = 1000.f / 60.f;   // sender's frame spacing, from its clientTimeMs
};

}  // namespace hsm
