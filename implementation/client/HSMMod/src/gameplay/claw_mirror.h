// Draws Half Wolverine's claws on other players' fighters (rules: match/hsm_claw_mirror.h). The blades are cosmetic: the
// hits already reach other players through the claw weapons. Game thread only.
#pragma once
#include "gameplay/adapter.h"
#include "gameplay/lobby.h"
#include <functional>
#include <map>
#include <vector>

namespace hsm::game {

class ClawMirror {
public:
    using ActorOf = std::function<ue::UObject*(direct::UserId)>;
    // Each frame in a fight, in a lobby. bone: this machine's claws are bone rather than adamantium.
    void Tick(Adapter& adapter, Lobby& lobby, ue::UObject* mine, bool bone, const ActorOf& actorOf);
    bool BladesOut() const { return bladesOut_; }   // this machine's claws, as of the last look (twice a second)
    void Clear(Adapter& adapter);   // leaving the fight: every drawn blade goes
private:
    struct Spawned { ue::UObject* actor = nullptr; int32_t index = -1; };
    struct Remote { std::string value; ue::UObject* fighter = nullptr; std::vector<Spawned> blades; bool loggedNoFighter = false; };
    void Publish(Adapter& adapter, Lobby& lobby, ue::UObject* mine, bool bone);
    void Draw(Adapter& adapter, Remote& r, ue::UObject* fighter);
    void Remove(Adapter& adapter, Remote& r);
    std::vector<Spawned> ownBlades_;
    std::string published_;
    std::map<direct::UserId, Remote> remotes_;
    uint32_t nextOwnScan_ = 0, nextPublish_ = 0, nextDraw_ = 0;
    bool loggedNoMesh_ = false;
    bool bladesOut_ = false;
};

}  // namespace hsm::game
