// Issue #1679. A stand-in has no tick and no capsule collision, so the game's own check never clears Box1 (the invisible
// box behind a fighter's back) between it and anyone else, and arms stop in mid-air behind other players. The same call
// the game makes, for every pair with a stand-in (combat/hsm_fighter_box.h), as often as the game. Game thread only.
#pragma once
#include "ue/engine.h"
#include <cstdint>
#include <utility>
#include <vector>

namespace hsm::game {

class FighterBoxes {
public:
    // Every tick: the stand-ins here, and the fighters this machine runs.
    void Tick(double nowMs, const std::vector<ue::UObject*>& standIns, const std::vector<std::pair<uint8_t, ue::UObject*>>& owned);
    void Clear();   // level change: the fighters are gone
private:
    void ClearPair(ue::UObject* bodyOf, ue::UObject* boxOf);
    ue::UObject* lib_ = nullptr;
    double nextMs_ = 0;
    uint64_t cleared_ = 0;
};

// standin_box_fix under [net] in HSMMod.ini (config.h). Set once at start.
void SetFighterBoxFix(bool on);

}  // namespace hsm::game
