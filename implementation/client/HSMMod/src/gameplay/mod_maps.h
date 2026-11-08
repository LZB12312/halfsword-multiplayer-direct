// Modded maps (HSMMod 0.8.0-pre2): finds the custom maps installed as .pak files under Content/Paks (and ~mods and any
// subfolder) and the paks they come from, for the host's map picker and for a joining client's check against the host's
// pak. Nothing is loaded, sent or downloaded here. Pure rules (stock filter, catalogue, compatibility): shared
// session/hsm_modmaps.h.
//
// Two sources, merged:
//   files     a worker thread lists the *.pak files (stock ones skipped), hashes each one with SHA-256 (cached by path +
//             size + mtime in HSMMod-modpaks.cache next to HSMMod.log) and reads its own index to learn which maps it holds
//   registry  the game thread asks the engine's asset registry (UAssetRegistry::GetAssetsByClass for /Script/Engine.World)
//             which maps it knows; a map the pak index did not name is listed without a pak
// Game thread only, except the worker, which touches only its own state and the mutex-guarded results.
#pragma once
#include "session/hsm_mod_rules.h"
#include "session/hsm_modmaps.h"
#include "session/hsm_modpack.h"
#include <string>
#include <vector>

namespace hsm::game::modmaps {

void Tick();      // each frame: starts the first scan, takes the worker's results, runs the registry query when due
void Rescan();    // files may have been dropped in since the start: list again (a hash already known is not recomputed)

const std::vector<ModMapEntry>& Maps();   // the custom maps, sorted; ModMapEntry::pak indexes Paks()
std::vector<std::string> Unchecked();   // what the last scan couldn't vouch for, for the online check
const std::vector<ModPak>& Paks();        // the mod paks found, hashed or not yet
const std::vector<LocalPack>& Packs();    // the packs installed under Paks/~mods/<pack>/ (HSMPack.ini), valid or not
const std::vector<ModKind>& PakKinds();   // per Paks()[i]: MapPak or ContentPak (session/hsm_mod_rules.h ClassifyPak)
uint32_t ScanVersion();                   // changes whenever Paks(), PakKinds() or the hashes change
bool Listed();                            // the file listing is complete (hashing may still run)
bool AllHashed();                         // every pak listed has its SHA-256

// The host's pick is a custom map whose pak is still being hashed: the lobby holds START until it is.
bool PickPending(const std::string& pick);
// What the host tells clients about its pick (Stock for a stock map), and what this machine makes of the host's message.
HostModMapResult HostMessage(const std::string& pick);
ModMapVerdict Decide(const ModMapMsg& host);

}  // namespace hsm::game::modmaps
