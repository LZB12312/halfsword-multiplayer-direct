// What the held G key means for this machine's fighter, from the format, the phase and the fighter's state. One rule
// for the key handler and the HUD hint. Game-independent.
//   Free-for-all, teams, Duel (round in play)      GiveUp       the fighter dies, the last attacker is credited
//   Survival and Zombies, a wave being fought      GiveUp       the fighter dies; the format's own rules go on (a fighter
//                                                               down comes back when the wave is cleared; the run is lost
//                                                               only when nobody is left standing)
//   Progression fight (Survival, one fight)        GiveUpFight  the fight is lost
//   Survival and Zombies, the break between waves  SkipBreak    this player's vote to skip the break
//   dead, queued for a respawn, Duel round gap     Waiting      nothing to give up; the HUD says so
//   Progression fight won, waiting (CoopFightWon)  ClaimWin     go on: the next round or the Tavern, as the stock Talk key
//                                                               does after a win (UI_WIN); alive or not
//   anything else (countdown, results, idle)       None
#pragma once
#include "match/hsm_hud_text.h"

namespace hsm {

enum class GiveUpAction : uint8_t { None = 0, GiveUp, GiveUpFight, SkipBreak, Waiting, ClaimWin };

// `alive`: this fighter is in the fight (its body is alive here and the match counts it). `bodyGone`: its body is dead,
// missing or waiting to respawn here. `oneFight`: a Progression fight, the host plays it as one fight with no waves.
inline GiveUpAction GiveUpActionFor(const MatchInfo& m, bool alive, bool bodyGone, bool oneFight) {
    const Format f = Format(m.format);
    const bool fought = m.phase == uint8_t(MatchPhase::Live) || m.phase == uint8_t(MatchPhase::SuddenDeath);
    if (CoopFightWon(m, oneFight)) return GiveUpAction::ClaimWin;           // dead or alive: the side won
    if (InWaveBreak(m)) return GiveUpAction::SkipBreak;                      // dead or alive: the vote counts either way
    if (!fought) return GiveUpAction::None;
    const bool duelGap = IsRoundFormat(f) && InRoundGap(m);
    if (alive && !duelGap) {
        if (!IsBotFormat(f)) return GiveUpAction::GiveUp;                    // free-for-all, teams, Duel
        if (oneFight && f == Format::PlayersVsBots) return GiveUpAction::GiveUpFight;
        return GiveUpAction::GiveUp;                                         // a wave of Survival or Zombies
    }
    if (IsRoundFormat(f) ? duelGap : bodyGone) return GiveUpAction::Waiting;
    return GiveUpAction::None;
}

}  // namespace hsm
