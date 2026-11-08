// Which fighters' bodies must stop colliding with which fighters' Box1 (#1679).
//
// Box1 is an invisible box behind a Willie's back: it stops that fighter's own forearms and hands going through it, and
// it blocks every other fighter's Mesh too. In a solo game each fighter's tick clears this every 1 to 3 s: a Pawn trace,
// then the game's DisableCollision_SkeletalVsSingleBody of its own Mesh against every Willie it found (Willie_BP Lc591).
// A stand-in has no tick, and its capsule has no collision, so no trace finds it: neither side of a pair with a stand-in
// is ever cleared, and a player's arms stop in mid-air behind the other player. A pair of live fighters is the game's own.
// Pure, no engine headers.
#pragma once
#include <cstddef>
#include <vector>

namespace hsm::combat {

inline constexpr double kFighterBoxEveryMs = 2000.0;   // the game's own 1 to 3 s: also finds new fighters and rebuilt bodies

struct FighterBoxPair {
    size_t body;   // this fighter's Mesh ...
    size_t box;    // ... against this fighter's Box1
};

// Fighters [0, standIns) are stand-ins, the next `live` run their own tick.
inline std::vector<FighterBoxPair> FighterBoxPairs(size_t standIns, size_t live) {
    std::vector<FighterBoxPair> pairs;
    const size_t all = standIns + live;
    for (size_t body = 0; body < all; ++body)
        for (size_t box = 0; box < all; ++box)
            if (body != box && (body < standIns || box < standIns)) pairs.push_back({body, box});
    return pairs;
}

}  // namespace hsm::combat
