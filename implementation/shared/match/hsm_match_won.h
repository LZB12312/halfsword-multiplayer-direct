// What the game mode's "Match Won" and "Player DED" flags should be, from the referee's match state. One rule for every
// format. Game-independent and unit-tested; the client's gameplay layer applies the answer (session.cpp TickStockFlags).
//
// The game's own give-up (the kneel on G, the dropped weapon, the voice lines) does nothing while Match Won is set. The
// first Survival match runs the game's win logic, which sets it after the first round, so its breaks are quiet; a
// rematch never runs that logic and every G hold in a break started the give-up. The Abyss never sets it at all. The
// referee's phase is that logic for every format: Match Won is clear while a round or wave is fought (a new match or round
// starts from the game's fresh state) and set in a bot format's break between waves. A Duel's round gap and the phases
// outside a fight are left as the game has them. Player DED is set when the player's fighter dies and the game never
// clears it for a rematch or a new round: it follows the fighter, so it is clear once the fighter is alive.
#pragma once
#include "match/hsm_hud_text.h"

namespace hsm {

enum class MatchWonFlag : uint8_t { Leave, Clear, Set };

struct StockMatchFlags {
    MatchWonFlag matchWon = MatchWonFlag::Leave;
    bool clearPlayerDed = false;     // the fighter is alive: a Player DED the game left set is stale
};

// `oneFight`: a Progression fight. Won and waiting for G (CoopFightWon), Match Won is set as the stock win sets it
// (BP_HalfSwordGameMode Minus_Enemy, blocked in a session): without it the stock Talk key on G kneels the player in a give-up,
// and the fight's end then keeps no kit (Override Player Character Equipment skips the kit of a fighter who gave up).
inline MatchWonFlag MatchWonFor(const MatchInfo& m, bool oneFight = false) {
    if (CoopFightWon(m, oneFight)) return MatchWonFlag::Set;
    const bool fought = m.phase == uint8_t(MatchPhase::Live) || m.phase == uint8_t(MatchPhase::SuddenDeath);
    if (!fought || InRoundGap(m)) return MatchWonFlag::Leave;
    return InWaveBreak(m) ? MatchWonFlag::Set : MatchWonFlag::Clear;
}

// `playerAlive`: this machine's fighter is in the fight and its body is alive here (not waiting for a respawn).
inline StockMatchFlags StockFlagsFor(const MatchInfo& m, bool playerAlive, bool oneFight = false) {
    StockMatchFlags f;
    f.matchWon = MatchWonFor(m, oneFight);
    f.clearPlayerDed = playerAlive;
    return f;
}

}  // namespace hsm
