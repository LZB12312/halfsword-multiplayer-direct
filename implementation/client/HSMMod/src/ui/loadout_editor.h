#pragma once
#include "gameplay/adapter.h"
#include <string>
namespace hsm::ui::loadout {
std::string Open(game::Adapter& adapter, ue::UObject* worldContext);
bool IsOpen();
// True once the editor closed and its in-memory passport can be uploaded.
bool Tick(game::Adapter& adapter);
void Close(game::Adapter& adapter, bool restoreView=true);
}
