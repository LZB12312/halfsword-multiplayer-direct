// The build card's widgets: a small terminal-style panel in the top-left corner of UI_HUD (match/hsm_build_card.h decides
// what it says and when). It never takes input. Game thread only.
#pragma once
#include "match/hsm_build_card.h"

namespace hsm::ui {

void BuildCardPush(BuildCard card);   // shows when the match HUD can; a player's newer card replaces a waiting one
void BuildCardToggle();               // the class checker key
void BuildCardClear();                // a new lobby: the old lobby's cards go
void BuildCardTick(bool hudVisible);  // once per frame in a match

}  // namespace hsm::ui
