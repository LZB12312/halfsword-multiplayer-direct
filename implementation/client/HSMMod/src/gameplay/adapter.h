// Half Sword game adapter: the only place that knows Half Sword class, function, property and bone
// names. Translates between protocol types (hsm_protocol.h) and the running game. Game thread only.
#pragma once
#include "match/hsm_class.h"
#include "hsm_drivelean.h"
#include "hsm_protocol.h"
#include "match/hsm_scenarios.h"
#include "match/hsm_team_kit.h"
#include "session/hsm_career.h"
#include "ue/engine.h"
#include <array>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

namespace hsm { struct VitalsMsg; }

namespace hsm::game {

// What one TintKit pass set: material parameters per slot kind (cloth includes the tabard, grip is a weapon's handle),
// and the weapon mesh parts it left alone as blades.
struct KitTally {
    int plate = 0, cloth = 0, shield = 0, grip = 0, skippedBlade = 0;
    int total() const { return plate + cloth + shield + grip; }
};

struct InputBinding { bool isAxis = false; size_t index = 0; bool pressed = false; };

class Adapter {
public:
    bool Resolve(ue::UObject* worldContext);       // per tick; cheap after the first success
    ue::UObject* World() const { return world_; }

    // Players
    ue::UObject* PlayerWillie(int localIndex);     // GameplayStatics.GetPlayerCharacter
    bool ControlRotation(int localIndex, float& yaw, float& pitch);   // the local player's camera/aim rotation
    bool SetControlRotation(int localIndex, float yaw, float pitch);  // turns the local player's camera (host physics lock-on, sync/hsm_lockon.h)

    // Loadouts: the local player's passport (Free Mode or Progression fighter) carried in an SG_GameProgress_C
    // SaveGame serialized to memory, so the engine deep-copies its TMaps/TArrays and reloads classes by path.
    bool ExportPassport(std::vector<uint8_t>& out);
    ue::UObject* LoadPassportSave(const std::vector<uint8_t>& blob);  // nullptr when the bytes are not our SaveGame
    bool SaveToMemory(ue::UObject* saveGame, std::vector<uint8_t>& out);

    // Fighters spawned by the mod (deferred spawn, like BP_SpawnerPoint_Willies).
    struct SpawnOptions {
        const std::vector<uint8_t>* blob = nullptr;   // nullptr = default loadout (GI "Free Mode Player Character 2")
        ue::FVector at; double yawDeg = 0;
        int teamInt = 1;                              // stock: 1 = player side
        bool brain = true;                            // true: plain AIController (host-driven input); false: puppet, no controller
        const void* passport = nullptr;               // raw Str_Passport_Character1 used instead of blob (bots)
        bool stockBrain = false;                      // keep Willie's own NPC brain (bots on the host)
        bool bot = false;                             // a generated bot (SpawnBot): keeps the generator's build, clamped to 0..1, never forced
        double weight = -1.0, height = -1.0;          // a role's build (match/hsm_roles.h; Riot, Juggernaut): the passport Weight / Height this fighter spawns with, 0..1; negative: none (SetBuild's rules)
        const char* fighterClass = nullptr;           // nullptr = the stock Willie_BP_C; else the actor class to spawn as (match/hsm_fighter_class.h: the Zombies format's bots)
        bool careerBody = false;                      // the career's Player Body Condition as the start body (the stock spawner's, session/hsm_career.h CareerBodyAtSpawn)
    };
    ue::UObject* SpawnFighter(const SpawnOptions& o);
    void KeepRemoteFighter(ue::UObject* willie, float yawDeg, float pitchDeg);   // per tick on the host
    // drive_lean (hsm_drivelean.h): the controller is read from the pawn, the rotation is set only when it changed, the
    // game instance is found once. false: as 0.8.0-pre17.
    void SetLean(bool on) { lean_ = on; }
    // Hand-off: a stand-in spawned without a controller gets the plain AIController a host-played fighter has
    // (SpawnFighter's brain), so KeepRemoteFighter can aim it. True when it has one.
    bool EnsureBrain(ue::UObject* willie);
    void RestoreGameInput();                               // game-only input, no cursor (as the stock death screens do)
    // The stock pause menu (a UserWidget whose class name contains "UI_Pause") is on screen. Cheap to call every frame: the
    // widgets are only searched for a moment after Esc or a pause, and then at most four times a second.
    bool PauseMenuOpen();
    // The pause menu widget PauseMenuOpen found (nullptr: none on screen).
    ue::UObject* PauseMenu();
    // Closes the pause menu the way its Continue button does (that button's click handler, found by name on the widget's
    // class); when none is found or the menu stays up, removes it, unpauses and gives the game its input back. True when
    // the menu is gone.
    bool ContinueFromPauseMenu();
    // Game-and-UI input with the mouse cursor shown, so the pause menu can be clicked (the game's own menus do the same).
    void ShowPauseCursor();
    // A menu level loaded after the mod took the cursor: game-and-UI input, cursor shown, as ShowPauseCursor. Leaving a
    // session skips the stock flow that gives the menus their input (#19, #23). Does nothing when the mod never touched it.
    bool MenuInputBack();
    // The chat's keyboard: on, UI-only input with the pressed keys flushed (the game's own UI_DeathDoor on open), so the
    // viewport ignores keys while the player types; off, RestoreGameInput.
    void CaptureInput(bool on);
    // The mod has set a game-only or UI-only input mode this run (RestoreGameInput, CaptureInput): the next menu level
    // needs its cursor and clicks given back (MenuInputBack).
    // First hit straight down from just above `at`, looking through `ignore`; `hitName` gets the hit actor's name.
    bool FloorZ(const ue::FVector& at, const std::vector<ue::UObject*>& ignore, double& z, std::string* hitName = nullptr);
    // Nothing between `from` and `to` (a fence, a wall), looking through `ignore`; `hitName` gets what is in the way.
    bool LineClear(const ue::FVector& from, const ue::FVector& to, const std::vector<ue::UObject*>& ignore, std::string* hitName = nullptr);
    // One visibility trace: true on a hit, with its impact point and the hit actor's name when asked for.
    bool TraceLine(const ue::FVector& start, const ue::FVector& end, const std::vector<ue::UObject*>& ignore, ue::FVector* impact,
                   std::string* hitName);
    // Where the player's camera is (the game's, or our follow camera while spectating); false when there is none.
    bool CameraLocation(ue::FVector& out);
    // The actors a fighter carries or wears: weapons (held and sheathed), quiver, feet and fists, attached armour.
    std::vector<ue::UObject*> CarriedActors(ue::UObject* willie);
    // Where a bone (socket) of the fighter's Mesh is in the world; false without a Mesh or a finite position.
    bool BoneLocation(ue::UObject* willie, const char* bone, ue::FVector& out);
    // A navmesh point within `tolerance` of `aim` that the navmesh connects to `from`; false: none found.
    bool ReachablePointNear(const ue::FVector& from, const ue::FVector& aim, double tolerance, ue::FVector& out);
    void DestroyActor(ue::UObject* actor);
    // Destroys a fighter (when it is still the object spawned at index) with what it carries: its weapons ("Weapon R",
    // "Weapon L") and quiver are actors of their own and its armour pieces are attached actors, none of which the
    // engine or Willie_BP removes with it. Attached fighters are never destroyed.
    void DestroyFighter(ue::UObject* willie, int32_t index);
    // 0.8.0-pre7: a fighter destroyed while the vertex-paint plugin still had a task queued on its mesh (a splash of blood
    // just applied) left that task's worker reading a freed mesh (#72, a crash at a kill on the host). DestroyFighter
    // parks such a fighter out of sight (hidden, no collision) and this destroys it once the plugin is idle, or after
    // kParkMaxMs. Called once a frame.
    void TickDeferredDestroys();
    // The game turns a Willie's hair simulation off before it destroys the groom (Willie_BP.cpp:16487-16488); destroying a
    // live, simulating one left Niagara's hair-strands data interface reading freed buffers on a worker thread (public
    // #28, #39, #44). Turns it off, and with `destroy` also destroys the component. Logged once per fighter.
    void StopHair(ue::UObject* willie, bool destroy);
    bool StillValid(ue::UObject* o, int32_t index) const;  // index from ue::ObjectIndex at spawn time
    // Owner sync: StillValid and AllWillies also treat an object the game is destroying (BeingDestroyed) or has destroyed
    // (IsGarbage) as gone, instead of until garbage collection frees its slot. Off in host sync, which keeps its old behaviour.
    void SetDestroyAware(bool on) { destroyAware_ = on; }
    // Owner sync: IsWeapon (and so FighterOf) also counts every Blueprint derived from ModularWeaponBP_C: the forged and
    // premade weapons, shields, fists and feet. Host sync keeps the old test, ModularWeaponBP_C exactly (the level's and
    // the procedural weapons only).
    void SetSubclassWeapons(bool on) { subclassWeapons_ = on; }
    // An actor the game is destroying (AActor::IsActorBeingDestroyed) or a component (UActorComponent::IsBeingDestroyed);
    // false for any other object. Each is asked its own class's function: the actor's reads garbage on a component.
    static bool BeingDestroyed(ue::UObject* o);
    // BP_SpawnerPoint_Willies actors in the loaded level that the level kept: BP_LevelManager::Spawn_Combatants destroys
    // every spawner not made for the current game, combat and play modes or the number of combatants (the Pit's bridge
    // and outer ring in Free Mode), and a destroyed one reads (0,0,0), the middle of the Pit.
    std::vector<ue::FVector> SpawnCandidates();
    // Pit Bridge Duels: the world bounds (centre, half extents) of every BP_Structure_Board_Destructible_1X6M_C in the level, the
    // Pit's bridge (shared/match/hsm_pitbridge.h works the ends out). Empty when the level has none.
    struct ActorBox { ue::FVector origin, extent; };
    std::vector<ActorBox> BridgeBoards();
    // role=server spawn anchor fallbacks, tried in this order after the spawners (shared/session/hsm_spawn_anchor.h): the
    // level's PlayerStart actors; a navmesh point (the origin or a NavMeshBoundsVolume projected onto the navmesh); the floor
    // a downward trace finds under the static meshes nearest the origin. The last two return false when there is none.
    std::vector<ue::FVector> PlayerStarts();
    bool NavigablePoint(ue::FVector& out);
    bool StaticMeshFloor(const std::vector<ue::UObject*>& ignore, ue::FVector& out);
    ue::FVector ActorLocation(ue::UObject* actor);
    double ActorYaw(ue::UObject* actor);
    double WorldSeconds();                                 // game seconds since this world began play, -1 unreadable
    void Teleport(ue::UObject* actor, const ue::FVector& to);
    ue::UObject* ControllerOf(ue::UObject* pawn);

    // Match support (Phase 3)
    bool IsWillie(ue::UObject* o);
    bool WillieClassKnown() const { return willieClass_ != nullptr; }   // until then IsWillie answers false for everything
    bool FreeModeActive();
    void PrepareFreeModeFight(int foes = -1);
    // Game Mode 2 (the Tavern errands: Alley drunk escort, Cellar barrel) cannot finish in a session (audit item 40):
    // set it to 1, a normal fight. True if it was 2.
    bool EndErrandMode();
    void ClearFreeMode();                       // the stock Progression: Free Mode off
    // Combat scenarios (match/hsm_scenarios.h): the world settings on the game instance, set before a travel (they reach the
    // clients with its settings). The host's own values are kept the first time and put back by RestoreScenario.
    // False when none of the fields it needed could be set.
    bool ApplyScenario(const hsm::ScenarioWorld& w);
    void RestoreScenario();
    // The Fenced-ring scenario's lose condition back on the game instance (hsm::kLoseConditionFence), after the level
    // manager's level-load reset wiped it and before the level script checks it to keep the BP_Fence_Bags (#706).
    // True when the field was set.
    bool KeepFenceLoseCondition();
    // The Fenced ring's BP_Fence_Bags, as the map's own script builds it at Play Mode 0 (match/hsm_scenarios.h
    // FencePlacementFor): spawned with its Radius set before it finishes (its construction lays the bags out). nullptr:
    // the class is not loaded (not a fence map) or the spawn failed.
    ue::UObject* SpawnFenceBags(const ue::FVector& at, double radius);
    bool FenceBagsExist();
    ue::UClass* LoadClassBlocking(const std::string& objectPath);
    bool IsZombieBody(ue::UObject* willie);   // of the zombie Willie class (match/hsm_fighter_class.h)
    // The spike: logs the game's combat-event objects (BP_CombatEvent_*_C) with their Scenario Name, Available Combat Modes
    // and Available Lose Conditions, one line each ("scenario: map <name>: combat event ..."). Returns how many lines.
    int LogCombatEvents(const std::string& map);
    // Progression: the names of the next fights the hub offers (the career's "Available Combat Events"), in list order;
    // empty when the list cannot be found. `freshVisit` (the host's first read of a hub visit) logs one line per holder
    // of the list saying why it reads as it does, and when it is empty refills it once through the level's own game
    // manager (BP_GameManager_C's Generate Combat Events, #621) before reading it again. StartOfferedFight(i) starts the
    // i-th through the hub UI's own function; false (and `how` says why) when that cannot be found or called.
    std::vector<std::string> OfferedFights(bool freshVisit = false);
    bool StartOfferedFight(size_t index, std::string& how);
    int FreeModeFoes();                                    // GI "Free Mode Foes Amount", -1 if unreadable
    void SetFreeModeFoes(int foes);
    // 0.8.0-pre20 (#294 #295 #301): every player fighter spawned from now on (own body, stand-ins) gets exactly `forced` as
    // its passport Weight when `on` (hsm::BuildWeight), the lobby tier's build. Off, and for bots: their own, clamped to 0..1.
    // The Weight drives the body's mass and the grip's hold limit, so players must not differ in it. `tier` is for the log.
    void SetBuild(double forced, bool on, int tier) { buildWeight_ = forced; buildOn_ = on; buildTier_ = tier; }
    bool BuildOn() const { return buildOn_; }
    double ForcedBuild() const { return buildWeight_; }
    // 0.8.0-pre21: the forced build on a fighter already in the world (the player's own, which the game spawned from its
    // save): passport Weight, Muscle Rate and every body's mass scale, rescaled so armour and weapon weights stay.
    void ApplyBuildLive(ue::UObject* willie);
    // A class's stats on this machine's own fighter. Called often: each value keeps the game's own as its base (match/hsm_class.h).
    struct ClassPiece { ue::UObject* what = nullptr; int32_t index = -1; ClassStatTrack power; };   // a foot or a held weapon
    struct ClassTracks {
        ue::UObject* who = nullptr;
        ClassStatTrack run, walk, strength, gripR, gripL, hands, bones, regen;
        double staminaLast = -1.0, weightSet = -1.0, rateWritten = -1.0;
        bool holding = false;   // Grappler: this fighter's hand is on another fighter
        // Under host physics both fighters are simulated here, so a Grappler's hold can lighten what it holds, for as long as it holds it.
        bool lightenHeld = false; ue::UObject* heldMesh = nullptr; int32_t heldIndex = -1; std::vector<std::pair<ue::FName, float>> heldScales;
        int massCheckIn = 0;    // ticks until the log says what the class's weight came to
        ClassPiece footR, footL, handR, handL;
        bool loggedMissing = false;
    };
    void ApplyClassStats(ue::UObject* willie, const ClassSheet& sheet, ClassTracks& tracks);
    void ApplyClassWeight(ue::UObject* willie, double base, const ClassSheet& sheet, ClassTracks& tracks);   // Strength and Muscle
    ue::UObject* HeldFighterMesh(ue::UObject* willie);
    void LightenHeld(ue::UObject* target, ClassTracks& tracks);
    void ReadLiveStats(ue::UObject* willie, double& run, double& strength);   // what's on the fighter now; 0: couldn't read
    bool SetMusclesLook(ue::UObject* willie, double value);   // skin only (match/hsm_class.h ClassLookMuscles); false: no body material yet
    void ClearPain(ue::UObject* willie);   // every frame for a class that feels none: the game flinches the moment a value passes its limit
    // Brawl: every fighter spawned from now on (own body, stand-ins, whatever loadout it was sent) has no armour and
    // holds no weapon. Done on the fighter's own copy of the passport, so no save and no uploaded loadout changes.
    void SetBare(bool on) { bare_ = on; }
    void QuitGame();
    bool GamePaused();
    void SetGamePaused(bool paused);
    // World time back to normal speed (slow motion and photo mode change it on one machine only). True if it was not 1.
    bool ResetTimeDilation();
    void ConsoleCommand(const std::string& command);      // KismetSystemLibrary.ExecuteConsoleCommand                                 // GI_Settings.Free Mode Activated (false = Progression)
    bool IsDead(ue::UObject* willie);
    ue::UObject* FighterOf(ue::UObject* actor);            // the Willie an actor belongs to (itself, a held weapon, worn armor)
    bool IsWeapon(ue::UObject* actor);
    // Fists and feet: weapon actors a fighter spawns as part of its own body ("Weapon R/L" with an empty hand, "Foot R/L
    // Weapon"), never dropped, picked up or synced as props.
    bool IsBodyWeapon(ue::UObject* actor);
    std::vector<ue::UObject*> AllWillies();                // live Willie_BP_C instances in the loaded level
    std::vector<ue::UObject*> WeaponActors();              // live weapon actors (IsWeapon), sheathed, held and dropped
    // Destroys every weapon lying loose: no "Parent Actor" (held and sheathed weapons keep it, Willie_BP.cpp:5987, 6299, 6995).
    // "Last Parent" is not asked: ModularWeaponBP clears it itself 300 uu from the holder (:1152-1157) and on a re-grab
    // (:1021-1023). levelStart says whether a weapon is one the level placed and still lies at its spot (kept); the
    // rule is session/hsm_ground_clear.h. Counts what was destroyed and what stayed.
    struct DropClear { int destroyed = 0, held = 0, sheathed = 0, level = 0; };
    DropClear DestroyDroppedWeapons(const std::function<bool(ue::UObject*)>& levelStart);
    // Weapons the level itself placed: lying loose (no "Parent Actor") and never held ("Last Parent" empty). The map reset
    // records them at the match start (object_sync.h). One pass over the object array.
    std::vector<ue::UObject*> LevelWeapons();
    // A new actor of a class (a Blueprint or native actor class with no setup of its own) at a place, spawned always.
    ue::UObject* SpawnActorAt(ue::UClass* cls, const ue::FVector& at, const ue::FRotator& rot);
    // Crossbows in the lobby (experimental, behind a runtime check): whether this level has the game's crossbow weapon and bolt
    // classes. Logs "crossbow: class <name> found|missing, bolt class <name> found|missing" each time `log` is set. The classes
    // are Blueprint classes, found only while some loaded asset references them; missing ones leave the Crossbows row without effect.
    bool CrossbowAvailable(bool log);
    // Hands a living fighter a crossbow: spawns the weapon actor and puts it in the right hand the way the game's own pick-up
    // does ("Set Up Right Hand Weapon"), then makes sure the quiver holds bolts. False (logged) when any step failed.
    bool GiveCrossbow(ue::UObject* willie);
    ue::FRotator ActorRotation(ue::UObject* actor);
    // Capture the flag (match/hsm_ctf.h): each machine spawns its own two flag actors and moves them to what MatchInfo says.
    // The actor class is the first loaded one of a preference list (a long tool: Rake, Pitchfork, Shovel, Scythe, the lantern);
    // null (logged once per level) when none is loaded, and the flags are markers only. The actor is tinted in the team's colour
    // and frozen (no physics, no collision) until a fighter takes it in hand.
    ue::UObject* SpawnFlagActor(int team, const ue::FVector& at, std::string* className = nullptr);
    void FlagLevelChanged() { flagClass_ = nullptr; flagClassName_.clear(); flagBodies_.clear(); modActors_.clear(); }
    // An actor the mod spawned and keeps (the flags): the ground clears (DestroyDroppedWeapons) and the map reset's level-weapon
    // record (LevelWeapons) leave it alone. Checked by object index, so a recycled address is not mistaken for it.
    bool IsModActor(ue::UObject* a);
    // Lets go of the flag (a hand or the back), then puts it, frozen, at `at`.
    void FlagPlace(ue::UObject* flag, ue::UObject* carrier, const ue::FVector& at, double yawDeg);
    // The fighter takes the flag in a free hand (the game's own pick-up: "Set Up Right/Left Hand Weapon"); never lets go of a
    // weapon for it. False when both hands are full or the pick-up did not take: the caller carries it on the back.
    bool FlagHold(ue::UObject* willie, ue::UObject* flag);
    bool FlagHeld(ue::UObject* willie, ue::UObject* flag);
    // Hangs the flag, frozen, on the fighter's back sheath socket (Sheath_Slot_Back). True when it is attached to that fighter.
    bool FlagCarryOnBack(ue::UObject* carrier, ue::UObject* flag);
    bool FlagAttachedTo(ue::UObject* carrier, ue::UObject* flag);
    // Armour pieces lying loose (no attach parent): what a strip, a pick-up swap or a fallen bot's loot left. Worn and
    // carried pieces are attached. One pass over the object array.
    std::vector<ue::UObject*> LooseArmor();
    // Survival loot: a fallen fighter lets go of what its hands hold and sheds the worn pieces LootPicks(seed) picks
    // through its own Remove Armor (a loose piece each, reported to the other machines like a strip). Returns how many
    // pieces it shed; weapons: how many hands let go.
    int DropLoot(ue::UObject* willie, uint32_t seed, int& weapons);
    std::vector<ue::UObject*> BodiesAndGear();             // every Willie, what is attached to it (armor, weapons) and every weapon
    // Re-tints the fabric of every armor piece attached to the Willie; returns how many pieces changed.
    int TintArmor(ue::UObject* willie, float r, float g, float b);
    // The team kit (match/hsm_team_kit.h): walks the Willie's attached actors (worn armour and clothing, shield, held and
    // sheathed weapons) and sets every tintable colour parameter of the slots `kit` names to `team`'s colour, through dynamic
    // material instances (made once per mesh slot: the component keeps them). Blades are never touched. Cheap to repeat.
    KitTally TintKit(ue::UObject* willie, int team, TeamKit kit);
    // Infection (match/hsm_modes.h): puts the zombie traits on a fighter simulated here: the zombie flag, the pain fields zeroed, and
    // Consciousness kept above half of `baseline` (the value read when the body was first handled; NaN: read it now and return it).
    // Cheap to repeat every frame (the fields are looked up once per class). What it found comes back for the log line.
    struct InfectedTraits { bool zombieFlag = false; int painFields = 0; std::string consciousness; double baseline = 0; };
    InfectedTraits ApplyInfectedTraits(ue::UObject* willie, double baseline);
    // A hash of what the Willie wears and holds (its attached actors and both hand fields): a change means gear came or went.
    uint64_t GearSignature(ue::UObject* willie);
    // Spawns a fresh fighter for local player 0 and possesses it; the old pawn is destroyed.
    ue::UObject* RespawnLocalPlayer(const SpawnOptions& o);
    // Host: a stock NPC fighter with a passport from the game's random generator (Enum_Ranks tier 0..7).
    ue::UObject* SpawnAbyssNpc(const ue::FVector& at);   // the Abyss level's own enemy spawner; null elsewhere
    ue::UObject* SpawnBot(int rank, const ue::FVector& at, double yawDeg, int teamInt);
    int DropHeldWeapons(ue::UObject* willie);   // real weapons out of both hands, as a dropped weapon leaves; fists stay
    // The Willie's own passport serialized like ExportPassport (bots are relayed as blobs too).
    bool ExportPassportOf(ue::UObject* willie, std::vector<uint8_t>& out);
    // Per tick: UI_DED / UI_DeathDoor / UI_WinScreen / UI_WIN never construct while on. ownerMatch (owner sync) also
    // suppresses UI_Lose / UI_GiveUp / UI_NextFIght, the single-player lose flow that pauses the game.
    void SuppressStockScreens(bool on, bool ownerMatch = false);

    // Input (resolved from Willie_BP_C event functions)
    bool BindInputs(ue::UObject* willie);
    const std::unordered_map<ue::UFunction*, InputBinding>& Bindings() const { return bindings_; }
    void ApplyAxis(ue::UObject* willie, Axis a, float value);
    void FireAction(ue::UObject* willie, Action a, bool pressed);
    static float ReadAxisParam(ue::UFunction* fn, void* params);

    // State
    bool ReadState(ue::UObject* willie, uint8_t entity, WillieState& out);
    // Owner-authority vitals (protocol rev 9): dead ("DED"), fallen, knocked out (Consciousness <= 0), the health
    // fields, the severed-part mask; dislocations are not read yet (Phase 4). False without a readable Willie.
    bool ReadVitals(ue::UObject* willie, uint8_t entity, VitalsMsg& out);
    ue::UObject* Mesh(ue::UObject* willie) { return MeshOf(willie); }
    void DriveTowards(ue::UObject* willie, const WillieState& target, float dt);
    bool FirstBodyPosition(ue::UObject* willie, ue::FVector& out);   // world position of the first synced bone (hip)
    void NudgeTowards(ue::UObject* willie, const WillieState& target, float blend);   // blends bone velocities, keeps the local motion
    void ApplyVitals(ue::UObject* willie, const WillieState& s);
    void ApplyStandInVitals(ue::UObject* willie, const VitalsMsg& v);   // owner sync: the owner's state on its stand-in
    void WriteParryTonus(ue::UObject* willie, const VitalsMsg& v);      // ...and its tonus, just before a parry roll
    // Sync audit T3: what another machine's Blueprint wrote onto this fighter's stand-in, made on the real one.
    void Revive(ue::UObject* willie);                                    // Willie_BP Get_Up_Pal (W:4718)
    bool Push(ue::UObject* willie, const std::string& bone, const ue::FVector& impulse);   // a contact's impulse (kg cm/s) on one body
    void TriggerDeath(ue::UObject* willie);
    size_t BoneCount() const { return bones_.size(); }
    ue::UObject* GameInstance();
    bool MouseLook(float& sensitivity, bool& invert);   // GI_Settings' mouse sensitivity and invert
    // My own fighter while its tick is blocked (a replica: host physics, a hand-off): Look Sensiticity moved as its tick
    // would. The look axes multiply by it, and it starts at 0 (#137: no mouse look at all on a replica from its spawn).
    void TickLookSensitivity(ue::UObject* willie, float dt);
    // The actors a blade of this fighter is stuck in now: its held weapons' active Stuck Constraints (hand-offs).
    std::vector<ue::UObject*> StuckTargets(ue::UObject* willie);
    void KeepAudioListener(ue::UObject* mine);          // player 0's audio listener back on my fighter's head
    std::wstring SaveGamesDir();   // <Saved>/SaveGames/ (sweeps temporary slots left by a crash the first time); empty before the engine is up
    // A blank SG_GameProgress_C (CreateSaveGameObject), nullptr when it cannot be made. Unreferenced: use it within the call
    // that made it, before the engine collects garbage (the save guard holds the player's passports in one over a load).
    ue::UObject* NewSaveObject();
    // A Progression fight's end on this machine's career (session/hsm_career.h). Whole-number career fields (Player Funds,
    // Reputation on Rank, Due Reward) are 4 or 8 bytes by the reflected size; false when the field is missing or neither.
    bool GetCareerInt(const char* field, int64_t& out);
    bool SetCareerInt(const char* field, int64_t value);
    bool ReadCareerRank(hsm::CareerRank& out);                // funds, reputation, tier, permanent tier
    bool WriteCareerRank(const hsm::CareerChange& change);    // and on a Tourney won: nemeses cleared, board, merchant and people renewed
    int CareerGameMode();                                     // GI "Current Game Mode enum", -1 unreadable
    int CareerCombatMode();                                   // GI "Current Combat Mode", -1 unreadable
    bool SkipTimeStep();                                      // GI "Skip Time Event" at no price: the time of day one step on
    int CareerArrayNum(const char* field);                    // the length of a GI array (e.g. "Available Combatants"), -1 unreadable
    // A Tourney's final round: the level's game manager generates the champion into Available Combatants (UI_NextFIght NF:97).
    bool GenerateTourneyChampion(int tier);

private:
    struct Parked {
        ue::UObject* willie = nullptr;
        int32_t index = -1;
        std::vector<std::pair<ue::UObject*, int32_t>> carried;
        double sinceMs = 0;
    };
    std::vector<Parked> parked_;
    ue::UClass* paintClass_ = nullptr;
    bool paintClassLooked_ = false;
    bool PaintTasksQueued(ue::UObject* willie);
    void FinishDestroy(Parked& pk, const char* why, double t0Ms);
    double buildWeight_ = 0.5;
    bool buildOn_ = false;
    int buildTier_ = 0;
    bool bare_ = false;
    struct FlagBody { ue::UObject* comp = nullptr; uint8_t collision = 3; bool simulating = false; };
    ue::UClass* flagClass_ = nullptr;
    std::string flagClassName_;
    std::unordered_map<ue::UObject*, std::vector<FlagBody>> flagBodies_;   // per flag actor: each primitive as it was before it was frozen
    std::vector<std::pair<ue::UObject*, int32_t>> modActors_;                // the flags: actor and object index (IsModActor)
    void FlagFreeze(ue::UObject* flag, bool frozen);
    int TintFlag(ue::UObject* flag, int team);
    struct ScenarioSaved { bool saved = false, carnage = false, brawling = false, hasLose = false; uint8_t lose = 0; } scenarioSaved_;   // the GI values ApplyScenario replaced
    bool ResolveBones(ue::UObject* mesh);
    std::string TempSlot();
    void DeleteSlot(const std::string& slot);
    std::wstring saveDir_;
    uint32_t slotCounter_ = 0;
    bool CopyPassport(ue::UObject* willie, const SpawnOptions& o, double& height);
    void ApplyBuild(ue::UObject* willie, ue::FProperty* passport, bool bot, double roleWeight);
    void StripEquipment(ue::UObject* willie, ue::FProperty* passport);
    bool SavePassportFrom(ue::UObject* owner, ue::FProperty* src, std::vector<uint8_t>& out);
    ue::UObject* MeshOf(ue::UObject* willie);
    ue::UObject* world_ = nullptr;
    ue::UObject* statics_ = nullptr;               // GameplayStatics CDO
    ue::UClass* willieClass_ = nullptr;
    bool destroyAware_ = false;
    bool lean_ = true;
    std::unordered_map<ue::UObject*, ControlRotationGate> rotationGate_;   // per remote fighter (KeepRemoteFighter)
    ue::UObject* gi_ = nullptr;            // GameInstance(), found once
    int32_t giIndex_ = -1;
    ue::UClass* giClass_ = nullptr;
    bool pauseOpen_ = false, pauseLogged_ = false;
    ue::UObject* pauseWidget_ = nullptr;           // with its object-array slot: the widget PauseMenuOpen last found on screen
    int32_t pauseWidgetIndex_ = -1;
    uint32_t pauseCheckAt_ = 0, pauseScanAt_ = 0, pauseArmedUntil_ = 0;   // PauseMenuOpen's throttles (GetTickCount ms)
    bool inputTouched_ = false;    // RestoreGameInput or CaptureInput ran since the last MenuInputBack
    bool inputCaptured_ = false;   // CaptureInput(true) holds: a respawn keeps the chat's keyboard
    bool subclassWeapons_ = false;
    ue::FName weaponName_{};                       // ModularWeaponBP_C, once a class chain has shown it
    bool weaponNameKnown_ = false;
    std::array<ue::UFunction*, kAxisCount> axisFn_{};
    std::array<ue::UFunction*, kActionCount> pressFn_{}, releaseFn_{};
    std::unordered_map<ue::UFunction*, InputBinding> bindings_;
    std::vector<ue::FName> bones_;
    std::vector<std::string> boneNames_;
    bool bonesResolved_ = false;
    int TintMesh(ue::UObject* mesh, KitSlot slot, TeamKit kit, int team);
    void NoteKitParam(KitSlot slot, const std::string& name, bool tintable);
    std::array<std::set<std::string>, kKitSlotCount> kitParamsSeen_;   // the parameter names logged so far, per slot kind
    std::set<int> kitPushSeen_;                                        // the teams whose armour-passport push was logged (once each)
    ue::UClass* meshClass_ = nullptr;               // MeshComponent
    bool heightLogged_ = false;                     // the past-1.0 fighter spawn (the Juggernaut's INI height) was logged
};

}  // namespace hsm::game
