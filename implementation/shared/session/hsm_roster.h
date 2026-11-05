// Session roster: who occupies which slot/entity. Game-independent, unit-tested.
// Slot 0 is the host; slots 1..8 are clients in join order (8 players at most: a host with a fighter seats 7 clients,
// role=server seats 8); freed slots are reused (lowest first).
// Entity ids are slot + 1 for fighters, so they stay stable for a member's whole session.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hsm {

constexpr int kMaxPlayers = 8;    // fighters per session; role=server has none of its own, so it seats 8 clients
constexpr int kMaxFighters = kMaxPlayers + 1;   // slots: 0 (the host, empty on a server) and 1..8

struct RosterMember {
    uint64_t key = 0;      // stable identity (SteamID, or transport peer id for direct-IP)
    uint8_t slot = 0;
    uint8_t entity = 0;
    std::string name;
};

class Roster {
public:
    static uint8_t EntityForSlot(uint8_t slot) { return uint8_t(slot + 1); }

    // Adds a member or returns the existing slot for a known key. Fails when kMaxPlayers are in: with the host in slot 0
    // the clients get 1..7, on a server (slot 0 reserved) 1..8.
    std::optional<uint8_t> Add(uint64_t key, const std::string& name) {
        if (const RosterMember* m = Find(key)) return m->slot;
        if (members_.size() >= size_t(kMaxPlayers)) return std::nullopt;
        for (uint8_t slot = firstSlot_; slot < kMaxFighters; ++slot) {
            if (BySlot(slot)) continue;
            members_.push_back({key, slot, EntityForSlot(slot), name});
            return slot;
        }
        return std::nullopt;
    }

    bool Remove(uint64_t key) {
        for (size_t i = 0; i < members_.size(); ++i)
            if (members_[i].key == key) { members_.erase(members_.begin() + long(i)); return true; }
        return false;
    }
    bool Rename(uint64_t key, const std::string& name) {
        for(auto& m:members_) if(m.key==key && m.name!=name) { m.name=name; return true; }
        return false;
    }

    const RosterMember* Find(uint64_t key) const {
        for (const RosterMember& m : members_) if (m.key == key) return &m;
        return nullptr;
    }
    const RosterMember* BySlot(uint8_t slot) const {
        for (const RosterMember& m : members_) if (m.slot == slot) return &m;
        return nullptr;
    }
    const RosterMember* ByEntity(uint8_t entity) const {
        for (const RosterMember& m : members_) if (m.entity == entity) return &m;
        return nullptr;
    }
    const std::vector<RosterMember>& members() const { return members_; }
    size_t size() const { return members_.size(); }
    void Clear() { members_.clear(); }
    // role=server: the host has no fighter, so slot 0 (entity 1) is never given to anyone. Survives Clear().
    void ReserveHostSlot(bool on) { firstSlot_ = on ? 1 : 0; }

private:
    std::vector<RosterMember> members_;
    uint8_t firstSlot_ = 0;
};

}  // namespace hsm
