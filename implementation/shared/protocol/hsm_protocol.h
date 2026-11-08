// HalfSwordMP wire protocol (revision 5). Game-independent: no Unreal or generated SDK types.
// All integers are little-endian; floats are IEEE-754 binary32. Every decoder validates sizes,
// counts and finiteness and returns false on malformed input instead of trusting the sender.
#pragma once
#include "match/hsm_match.h"
#include "match/hsm_objective.h"
#include "match/hsm_roles.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hsm {

constexpr uint16_t kMagic = 0x4D48;          // "HM"
#ifdef HSM_TESTBENCH
constexpr uint8_t kProtocolRevision = 141;   // test builds only: real players neither list nor join these lobbies
#else
constexpr uint8_t kProtocolRevision = 41;         // 41: ProgressionResult 69 (host -> client, once per Progression co-op fight, ahead of the Travel: won, Tourney, the purse, and kResultNextRound 4 / kResultTimeStep 8 for a contest round won with more to come; each player's own career takes the fight's end, session/hsm_career.h), and a Travel's settings carry Day Count (every career keeps the host's calendar); 40: lobby mod rules and HSMMod integrity (docs/PLAN_MOD_RULES.md): ModList 48 (client -> host: the mods this machine runs, kind + name + SHA-256, and its official-build status; session/hsm_mod_rules.h), SessionMods 49 (host -> all: the union with a slot mask per mod), ModChallenge 67 / ModAnswer 68 (host -> client: a nonce and ranges of HSMMod's code section; client -> host: their relocation-normalised SHA-256; session/hsm_mod_challenge.h), Reject carries a trailing detail line (120 bytes at most, printable ASCII), RejectReason ModBuild 9, ModsNotAllowed 10 and ModifiedClient 11; 39: Sanitize rules both ends apply: Tag is bare-fisted (gear Fists, match/hsm_match.h IsBare) and a Tournament fight is one round (scoreLimit 1); 38: Spawn.flags bit 1 kSpawnZombie (the body is the zombie Willie: Zombies' bots, Infection's infected; match/hsm_fighter_class.h), HitClaim source Ammo 3 (a fired bolt lodged in the victim, Stuck claims only); 37: Wave 3/4 wire: Format CaptureTheFlag 10, Tag 11, Infection 12, CrossbowTDM 13, Tournament 14 and PitBridgeDuel 15 (kFormatCount 16; match/hsm_ctf.h, hsm_modes.h, hsm_bracket.h, hsm_kingqueue.h, hsm_pitbridge.h), CtfEvent 62 (host -> client: a flag taken, dropped, returned, home or captured), ModeEvent 63 (host -> client: a Tag pass, a fuse burned, an infection), ProgressionVote 66 (the Tavern's next-fight vote; session/hsm_progvote.h; 64 and 65 stay unused), MatchInfo.tagIt (u8) and MatchInfo.fuseLeftMs (u16) right after crossbows, and trailing after fighters: the objectives (u8 count at most 8, 18 bytes each: kind, owner, state, carrier, x, y, z, timer; match/hsm_objective.h), then queueOrder (u8 count, entities), bracketWinners (u8 count, entities), fightWinsA, fightWinsB and streak (Tournament and Pit Bridge Duels; empty or 0 in every other format); 36: roles and team rounds (FighterEntry.role, a trailing u8 after steamId; Format TeamDuel 7, Riot 8 and Juggernaut 9, kFormatCount 10; match/hsm_roles.h), MatchInfo.teamKit, MatchInfo.freeCam and MatchInfo.crossbows (three u8, in that order, after forceStrength: the Team colours row, the dead players' free camera, the Crossbows row), Spawn.flags (a trailing u8, bit 0 kSpawnCrossbow), LoadoutChange 61 (CHANGE LOADOUT: client -> host the new passport as BlobChunks, host -> client one verdict byte; session/hsm_loadout_change.h); 35: GroundClear 59 (host -> client: the host's /clear, a mask of weapons, armour and moved props each machine clears on its own ground; session/hsm_ground_clear.h), LobbySecret 60 (host -> client: the lobby's password, handed to every player let in so a new host keeps the lobby locked; session/hsm_password.h), Hello.password (a trailing string, 32 bytes at most), RejectReason WrongPassword 7 and TooManyTries 8, MatchInfo.teams (the team count 2-4, right after format; Teams 2-4), Format::LastManStanding 6 (rounds, the last one alive takes the round; a Duel is for 2 players); 34: MatchInfo.forceStrength (the host's force_strength: every player's build is forced to the lobby tier's, match/hsm_match.h ForcedWeight); 33: Input 4 and RelayInput 19 carry a toggle count per action (a press or release edge survives a lost frame and a tap shorter than a frame; protocol/hsm_input.h) and the lock-on target (host physics: the client locks on itself and the host logs it; sync/hsm_lockon.h); 32: hand-offs (sync/hsm_handoff.h): HandOffAsk 58 (asker -> host -> victim's owner: my fighter grabbed or stabbed yours), HandOff 57 (writer -> host -> everyone: lease entries, the machine of the fighter that grabbed or stabbed simulates the other while they are coupled), Pose and Vitals carry the sender's lease epoch, clients send Input 4 under owner sync too, RelayInput 19 (host -> holder: a handed fighter's input); 31: MatchInfo.physics (the host's Gore Rate, Bone Snapping and Dead Weight Scale, pinned on every machine; -1 = each its own), RemoteWrite 56 (sender -> host -> target's owner: a teammate helped up, a shove's push, a hold broken free; sync/hsm_remote.h); 30: Vitals carries the fields other fighters read on a stand-in (state bits: downed, guarding, guard sides, crouched, thrusting, threatening, give-up; Weapon Skill, tonus, noise, light, movement dislocation, exhaustion); 29: host physics (0.9.0): Welcome.syncMode 0 is back (the host simulates every fighter from Input 4, clients show its Pose stream for every fighter, their own included), MatchInfo.syncMode (the mode the next map runs, the lobby's Sync row); 28: MapReset 54 (host -> client: the map goes back to the round start at a Duel round reset and a rematch; session/hsm_map_reset.h); MatchInfo.waveBreakS (the host's pause between waves), clearGroundMin and clearCount (the ground-clear timer; clients clear when the count changes); LevelSeed 55 (host -> client: the seed of the travel's shared dice, so the map's Blueprint random numbers agree on every machine; session/hsm_dice.h); 27: ModMap 53 (host -> client: the picked modded map's package path and the pak it comes from, name + size + SHA-256; session/hsm_modmaps.h), a client without that pak leaves with a line saying what to install; 26: generic object and event sync (M0): ObjectAnnounce 45, BodyState 46 (per body, replaces PropState 28), ObjectEvent 47; WorldState 17 and PropState 28 are retired (numbers stay reserved), spawned object ids (bit 62), Welcome.syncMode stays on the wire; 25: HitClaim striker kind Stuck + rawDamage (a lodged blade's Get Damage, replayed by the victim; stabs), the claim wire grows 8 bytes; 24: voice is each player's own (Voice flags kVoiceNoCut, kVoiceEveryone; VoiceMode and kWelcomeVoice* unused), MatchInfo.damagePct (the host's player damage), FighterEntry.steamId (a host migration rebuilds the scores by player); 23: Voice flags kVoiceAdpcm (a microphone the mod records itself), VoiceMode and Welcome.flags kWelcomeVoiceAll (the host's voice setting: Off / Proximity / Everyone), RouteTable and Forward (detours: a client relays another pair's Pose/Vitals), VoteState (the running votekick or voteban, for the in-match menu); 22: 8 players (fighter slots are entities 1..9: host + 8 clients, bots 10..19), Voice (proximity voice chat), PingTable (each client's ping to the host and to every other client), Welcome.flags kWelcomePeerPing; 21: MatchInfo.gear may be Fists (3, the Brawl mode: free-for-all with bare fists); 20: Reject reasons Kicked and Banned, chat lines from the server (kChatSystemSlot); 19: Spawn.life and victimLife (HitClaim, SeverClaim, DeathReport), PeerMap and MeshHeard (direct client-to-client Pose/Vitals), Welcome.flags; 18: ReportShare (in-game bug reports); 17: Kill flags 16/32, a drawn Duel round (both down / no hits); the pose carries each weapon's flail head (pose flags bits 2, 3, codec still v2); Hello and Welcome carry the mod build (session/hsm_build.h); MatchInfo marks fighters down (alive byte bit 1); 16: GiveUp (hold G concedes a Duel round); 15: PaintFx kinds Voice, HitSound, Armor, ArmorOn (voice lines, hit sounds, armour off/on, strip requests); 14: the pose codec is version 2, its header carries the hands' grip state (GripState); 13: a HitClaim may be struck by a prop; MatchInfo carries gear, tier, the Duel round and gap, fighters' ping and alive; 12: SeverNotice, PropCensus; 11: MatchInfo accepts every match format (Duel, Zombies); 10: PaintFx, Equip
#endif
constexpr uint16_t kDefaultPort = 27777;
constexpr size_t kMaxPlayerName = 32;
constexpr size_t kMaxModBuild = 64;   // Hello/Welcome modBuild bytes (session/hsm_build.h)
constexpr size_t kMaxWillies = 19;           // 9 fighter slots (host + 8 clients) + up to 10 bots
constexpr size_t kMaxBodies = 64;
constexpr size_t kMaxPacket = 16 * 1024;

// Channels: reliable-ordered control/events, unreliable-sequenced input/state.
enum Channel : uint8_t { kChanReliable = 0, kChanUnreliable = 1, kChanCount = 2 };

enum class MsgType : uint8_t {
    Hello = 1,       // client -> host
    Welcome = 2,     // host -> client
    Reject = 3,      // host -> client
    Input = 4,       // client -> host, unreliable
    Snapshot = 5,    // host -> client, unreliable
    Event = 6,       // host -> client, reliable
    Ping = 7,
    Pong = 8,
    Travel = 9,      // host -> client, reliable: follow the host to a level
    Ready = 10,      // client -> host, reliable: level loaded
    Spawn = 11,      // host -> client, reliable: an entity exists (fighter/bot) with its loadout blob
    Despawn = 12,    // host -> client, reliable
    BlobChunk = 13,  // both ways, reliable: one piece of a loadout blob
    MatchInfo = 14,  // host -> client, reliable: match settings, phase, scores, teams
    Kill = 15,       // host -> client, reliable: kill feed entry
    Chat = 16,        // both ways, reliable: chat line (protocol/hsm_chat.h); host relays
    // 17 was WorldState (host sync, removed in rev 26): reserved, never reuse.
    CombatEvent = 18, // host -> client, reliable: dismemberment / blood (session/hsm_combat.h)
    RelayInput = 19,  // host -> client, unreliable: a handed fighter's input frame, for the machine that holds it (rev 32)
    OwnerState = 20,  // client -> host, unreliable: the sender's own fighter as it simulates it (it is the authority)
    DeathReport = 21, // client -> host, reliable: the sender's fighter died, and who it says killed it
    Pose = 22,        // owner -> host -> others, unreliable: an owned fighter's pose (protocol/hsm_owner.h)
    Vitals = 23,      // owner -> host -> others, unreliable: an owned fighter's health, parts, dead/fallen/KO
    HitClaim = 24,    // attacker -> host -> victim's owner, reliable: a blow on a stand-in, to replay natively
    HitFx = 25,       // host -> everyone else, reliable: the same blow, shown cosmetically on their stand-in
    SeverClaim = 26,  // attacker -> host -> everyone, reliable: a sever started on a stand-in (combat/hsm_sever.h)
    GrabState = 27,   // grabber's owner -> host -> others: what its hands hold (reliable on change, unreliable refresh; sync/hsm_grab.h)
    // 28 was PropState (replaced by BodyState 46 in rev 26): reserved, never reuse.
    StuckBlade = 29,  // attacker -> host -> victim's owner, reliable: my blade stuck in / pulled out of a stand-in
    PropLease = 30,   // both ways, reliable: a lease request/release, or the host's grant (session/hsm_lease.h)
    PaintFx = 31,     // owner -> host -> others, reliable: blood or a wound painted on an owned fighter (combat/hsm_fx.h)
    Equip = 32,       // owner -> host -> others, reliable: which sheathed weapons an owned fighter has drawn (combat/hsm_equip.h)
    SeverNotice = 33, // owner -> host -> others, reliable: a sever the owner's own fighter took natively (combat/hsm_sever.h)
    PropCensus = 34,  // host -> client, reliable: the level's physics props and their indices (session/hsm_prop_census.h)
    GiveUp = 35,      // client -> host, reliable: the sender's fighter concedes the Duel round (held G)
    ReportShare = 36, // reporter -> host -> others, reliable: a bug report's id and attach token (session/hsm_report.h)
    PeerMap = 37,     // host -> client, reliable: the other clients' slots and SteamIDs (mesh, session/hsm_mesh.h)
    MeshHeard = 38,   // client -> host, reliable: which other slots this client hears directly (mesh fallback)
    PingTable = 39,   // client -> host, reliable: this client's measured ping to the host and to each other client
    Voice = 40,       // both ways, unreliable: one chunk of a fighter's microphone (Steam-compressed), proximity voice
    VoiceMode = 41,   // host -> client, reliable: the host changed its voice setting (Off / Proximity / Everyone)
    RouteTable = 42,  // host -> client, reliable: this client's detour routes and pair keys, and the pairs it relays
    Forward = 43,     // client -> client, unreliable: another client's Pose or Vitals, passed on by a relay (detour)
    VoteState = 44,   // host -> client, reliable: the votekick running now (or none), and this client's own vote
    ObjectAnnounce = 45, // spawner -> host -> all, reliable: an object appeared (a client's is a proposal; only the host's relay is canonical; session/hsm_object.h)
    BodyState = 46,   // streamer -> host -> others, unreliable: one body of an object (replaces PropState; session/hsm_lease.h)
    ObjectEvent = 47, // owner -> host -> all, reliable: a joint broke, a body detached/attached, an object destroyed, a mod event (session/hsm_object_event.h)
    // Reserved for later 0.8.0 mod support. No decoder yet: every validator rejects them (the default
    // case) until the milestone that owns them adds the layout. Do not renumber.
    ModList = 48,     // client -> host, reliable (rev 40): the mods this player runs (kind, name, SHA-256) and its official-build status (session/hsm_mod_rules.h)
    SessionMods = 49, // host -> all, reliable (rev 40): the session's mods, each with the slots that run it (session/hsm_mod_rules.h)
    ModMsg = 50,      // any -> host -> targets, per flags (M5): a message between mods, relayed
    PackList = 51,    // host -> client, reliable (M6): the .pak packs the host lists (SHA-256, size)
    PackStatus = 52,  // client -> host, reliable (M6): download, cache and mount progress
    ModMap = 53,      // host -> client, reliable (rev 27): the modded map the host picked and the pak it comes from (name, size, SHA-256; session/hsm_modmaps.h); empty path = a stock map. Not 48-52: those belong to the mod relay
    MapReset = 54,    // host -> client, reliable (rev 28): props back to their start, round-spawned objects gone, level weapons back (session/hsm_map_reset.h); not sent when the host's map_reset is off
    LevelSeed = 55,   // host -> client, reliable (rev 28): the seed of the level's shared dice, sent ahead of each Travel (session/hsm_dice.h)
    RemoteWrite = 56, // sender -> host -> target's owner, reliable (rev 31): a write the game made onto a stand-in, for its owner to make (sync/hsm_remote.h)
    HandOff = 57,     // writer -> host -> everyone, reliable (rev 32): lease entries, who simulates a fighter coupled to another (sync/hsm_handoff.h)
    HandOffAsk = 58,  // asker -> host -> victim's owner, reliable (rev 32): my fighter grabbed or stabbed yours (sync/hsm_handoff.h)
    GroundClear = 59, // host -> client, reliable (rev 35): the host's /clear: each machine clears its own weapons, armour and moved props (session/hsm_ground_clear.h)
    LobbySecret = 60, // host -> client, reliable (rev 35): the lobby's password, handed to every player let in so a new host keeps the lobby locked (session/hsm_password.h)
    LoadoutChange = 61, // client -> host, reliable (rev 36): the Esc menu's CHANGE LOADOUT: the new passport as BlobChunks (session/hsm_loadout_change.h)
    CtfEvent = 62,    // host -> client, reliable (rev 37): a flag was taken, dropped, returned, went home or was captured (match/hsm_ctf.h)
    ModeEvent = 63,   // host -> client, reliable (rev 37): a Tag pass, a fuse that ran out, an infection: the kill feed line, and for a fuse the owner's fighter dies (match/hsm_modes.h)
    // 64 and 65 are reserved (skipped on purpose, never reuse them).
    ProgressionVote = 66, // both ways, reliable (rev 37): the Tavern's next-fight vote: the host's options and tally, a client's pick, the result (session/hsm_progvote.h)
    ModChallenge = 67, // host -> client, reliable (rev 40): a nonce and ranges of HSMMod's code section to hash (session/hsm_mod_challenge.h)
    ModAnswer = 68,    // client -> host, reliable (rev 40): SHA-256(nonce || id || the relocation-normalised ranges)
    ProgressionResult = 69, // host -> client, reliable (rev 41): a Progression co-op fight's end, once, ahead of the Travel (session/hsm_career.h)
};
// The highest type that has a decoder (ProgressionResult); 50-52, 64 and 65 are reserved with none (64 and 65 are skipped on purpose,
// never reuse them). Fuzzers and tests bound valid types by these.
constexpr uint8_t kLastDecodedMsgType = uint8_t(MsgType::ProgressionResult);
constexpr uint8_t kLastReservedMsgType = uint8_t(MsgType::ProgressionResult);

// Analog inputs, mirroring DefaultInput.ini axis mappings.
enum class Axis : uint8_t {
    MoveForward, MoveRight, TurnMouse, LookMouse, TurnGamepad, LookGamepad,
    RightArm, LeftArm, RightGuard, LeftGuard, Count
};
// Digital inputs: DefaultInput.ini action mappings plus the raw RMB/MMB key events Willie binds
// (held state; press/release edges are derived on the host).
enum class Action : uint8_t {
    GrabLeft, GrabRight, GuardLeft, GuardRight, Thrust, ThrustGamepad, Jump, CrouchKey, CrouchHold,
    Run, SwapHands, ToggleCameraLock, KeyForward, KeyBack, KeyLeft, KeyRight, Talk,
    RightMouse, MiddleMouse, Count
};
constexpr size_t kAxisCount = static_cast<size_t>(Axis::Count);
constexpr size_t kActionCount = static_cast<size_t>(Action::Count);
static_assert(kActionCount <= 32, "action bitmask is 32 bits");

enum class RejectReason : uint8_t { BadRevision = 1, BuildMismatch = 2, Full = 3, Malformed = 4, Kicked = 5, Banned = 6,
                                     WrongPassword = 7, TooManyTries = 8,   // rev 35: a locked lobby (session/hsm_password.h)
                                     ModBuild = 9, ModsNotAllowed = 10, ModifiedClient = 11 };   // rev 40: session/hsm_mod_rules.h, hsm_mod_challenge.h
constexpr uint8_t kMaxRejectReason = 11;
constexpr size_t kMaxRejectDetail = 120;   // Reject.detail bytes: what to remove or install
constexpr size_t kMaxLobbyPassword = 32;   // Hello.password / LobbySecret bytes

enum class EventKind : uint8_t {
    Spawned = 1,       // a = entity, b = slot
    Despawned = 2,     // a = entity
    Dismembered = 3,   // a = entity, b = part enum value
    Died = 4,          // a = entity
    RoundReset = 5,
    MapChange = 6,     // text = map path
};

struct Hello {
    uint8_t revision = kProtocolRevision;
    uint64_t buildKey = 0;   // PE TimeDateStamp << 32 | SizeOfImage of the game exe
    std::string name;
    std::string modBuild;    // rev 17: the client's mod build (session/hsm_build.h ModBuildString)
    std::string password;    // rev 35: the lobby password the player typed (empty: none); the host compares it when the lobby is locked
};
struct Welcome {
    uint32_t sessionId = 0;
    uint8_t slot = 0;        // local-player index the host assigned to this client
    uint8_t entity = 0;      // entity id of the client's Willie
    uint16_t snapshotHz = 30;
    std::string map;
    uint8_t syncMode = 0;    // session/hsm_authority.h SyncMode: 0 the host simulates every fighter (host physics), 1 each owner its own; the mode of the map the host is in
    std::string modBuild;    // rev 17: the host's mod build
    uint8_t flags = 0;       // rev 19: kWelcomeNoHostFighter | kWelcomeMesh; rev 22: kWelcomePeerPing | kWelcomeVoice; rev 23: kWelcomeVoiceAll
};
constexpr uint8_t kWelcomeNoHostFighter = 1;   // the host plays no fighter of its own
constexpr uint8_t kWelcomeMesh = 2;            // mesh offered: clients send Pose/Vitals to each other too (PeerMap follows)
constexpr uint8_t kWelcomePeerPing = 4;        // rev 22: the PeerMap follows so clients can ping each other (mesh on or off)
constexpr uint8_t kWelcomeVoice = 8;           // rev 22: the host relays proximity voice (voice=1 in its HSMMod.ini)
constexpr uint8_t kWelcomeVoiceAll = 16;       // rev 23: with kWelcomeVoice: every living player hears every living one (voice_reach=everyone)
// kWelcomeMods = 32 is reserved for M4 (the host runs the mod relay and sends SessionMods); Decode(Welcome) rejects it until then.
struct Reject { RejectReason reason = RejectReason::Malformed; std::string detail; };   // detail: printable ASCII, may be empty

struct InputFrame {
    uint32_t seq = 0;
    uint32_t clientTimeMs = 0;
    std::array<float, kAxisCount> axes{};   // each clamped to [-kAxisLimit, kAxisLimit]
    uint32_t buttons = 0;                   // bit i = Action i held
    std::array<uint8_t, kActionCount> toggles{};   // rev 33: per action, how often it changed state, mod 16 (never reset): the edges a lost frame or a short tap hides
    uint8_t lockTarget = 0;                 // rev 33: the entity the player's camera is locked on (0: none; sync/hsm_lockon.h)
    float controlYaw = 0, controlPitch = 0; // degrees
};
constexpr float kAxisLimit = 512.f;         // mouse deltas can exceed 1 (a flick is hundreds, #148); anything beyond is rejected

struct Quat16 { int16_t x = 0, y = 0, z = 0, w = 32767; };
struct BodyState { float px = 0, py = 0, pz = 0; Quat16 rot; };
struct WillieState {
    uint8_t entity = 0;
    uint8_t flags = 0;           // bit0 dead, bit1 fallen, bit2 headless
    float health = 0, consciousness = 0, stamina = 0, bleeding = 0;
    std::array<float, 7> partHealth{};   // head, upper body, lower body, arm R, arm L, leg R, leg L
    uint32_t dismembered = 0;    // bit per Enum_DismembermentPart value
    std::vector<BodyState> bodies;       // order agreed by bone list in Welcome-time config
};
struct Snapshot {
    uint32_t tick = 0;
    uint32_t ackInputSeq = 0;
    std::vector<WillieState> willies;
};
struct Event {
    uint32_t seq = 0;
    EventKind kind = EventKind::RoundReset;
    uint32_t a = 0, b = 0;
    std::string text;
};
struct Ping { uint32_t timeMs = 0; };

// One game setting copied from host to client before travel (e.g. a GI_Settings field by name).
// Values are the raw little-endian bytes of a plain (bool/byte/enum/int/float/double) property.
constexpr size_t kMaxSettingName = 64, kMaxSettingBytes = 16, kMaxSettings = 64, kMaxLevelName = 256;
struct Setting { std::string name; std::vector<uint8_t> value; };
struct Travel {
    uint32_t seq = 0;           // increments per host level change
    std::string level;          // level name/path as passed to OpenLevel
    bool absolute = true;
    std::string options;
    std::vector<Setting> settings;
};
struct Ready { uint32_t travelSeq = 0; std::string level; };

enum class EntityKind : uint8_t { Fighter = 1, Bot = 2 };
struct Spawn {
    uint8_t entity = 0;
    EntityKind kind = EntityKind::Fighter;
    uint8_t ownerSlot = 0;      // roster slot of the controlling player (0xFF for host-owned bots)
    int8_t team = -1;           // -1 = free-for-all
    uint32_t blobId = 0;        // loadout blob carrying this entity's passport (0 = none)
    float x = 0, y = 0, z = 0, yaw = 0;   // spawn location (cm, degrees)
    uint8_t life = 0;           // rev 19: the host's count of this entity's bodies; a claim names the life it was made against (0 = unknown)
    uint8_t flags = 0;          // rev 36: kSpawnCrossbow = the body is given a crossbow (the lobby's Crossbows row; match/hsm_crossbow.h)
};
constexpr uint8_t kSpawnCrossbow = 1, kSpawnZombie = 2;   // rev 38: kSpawnZombie = the host built it as the zombie Willie
struct Despawn { uint8_t entity = 0; };

// Loadout blobs (SaveGame bytes holding one Str_Passport_Character1) travel as reliable chunks.
constexpr size_t kMaxBlobBytes = 256 * 1024, kBlobChunkBytes = 8 * 1024;
struct BlobChunk {
    uint32_t blobId = 0;
    uint32_t total = 0;          // whole blob size in bytes
    uint16_t index = 0, count = 0;
    uint32_t crc = 0;            // CRC-32 of the whole blob
    std::vector<uint8_t> data;
};

// Match (rev 5). Field ranges mirror hsm::Sanitize in match/hsm_match.h.
struct ScoreEntry { int16_t key = 0; int16_t score = 0; };          // key: team, or entity in free-for-all
struct FighterEntry {
    uint8_t entity = 0; int8_t team = -1; int16_t kills = 0, deaths = 0; std::string name;
    uint16_t pingMs = 0;                // round trip to the host, 0 = unknown (the host itself, bots)
    bool alive = true;
    bool down = false;                  // rev 17: Duel, the host's draw check has it down (match/hsm_draw.h DrawCheck::Down)
    uint64_t steamId = 0;               // rev 24: the player's SteamID, 0 = bot / not a Steam player (the scores' identity, session/hsm_migration.h)
    uint8_t role = 0;                   // rev 36: the fighter's role id (match/hsm_roles.h; 0 = the lobby's role, bots too), 0..kRoleCount-1
};
// GI_Settings doubles that change bodies and gore (Willie_BP.cpp:7678 Apply Gameplay Settings copies them onto each
// fighter): the host's are every machine's in a session (sync audit items 10, 13; gameplay/damage_pin.cpp).
constexpr size_t kPinnedPhysics = 3;
constexpr const char* kPinnedPhysicsNames[kPinnedPhysics] = { "Gore Rate", "Bone Snapping", "Dead Weight Scale" };
constexpr float kPinnedPhysicsMax = 10.f;
inline bool ValidPinnedPhysics(float v) { return v == -1.f || (v >= 0.f && v <= kPinnedPhysicsMax); }
struct MatchInfo {
    uint8_t format = 0;                 // hsm::Format
    uint8_t teams = 2;                  // rev 35: the Teams format's team count, 2..4 (always 2..4 on the wire; other formats ignore it: hsm::TeamCount)
    uint16_t scoreLimit = 5, timeLimitS = 0, respawnDelayS = 5;
    bool friendlyFire = false;
    uint8_t phase = 0;                  // hsm::MatchPhase
    uint32_t timeLeftMs = 0;
    int16_t winner = -1000;             // score key, -1000 = undecided
    uint8_t gear = 0, tier = 2;         // hsm::Gear, Enum_Ranks tier
    uint16_t round = 0;                 // Duel: the round being fought, or just won in the gap; 0 in other formats
    uint32_t roundGapLeftMs = 0;        // Duel: time before the next round, 0 outside the gap
    uint8_t roundWinner = 0;            // Duel's gap: the entity that took the round, 0 = drawn
    uint16_t waveBreakS = 0;            // rev 28: bot formats, the pause between waves in seconds (10/30/60/120), 0 = the format's own
    uint8_t clearGroundMin = 0;         // rev 28: the ground-clear timer in minutes (0, 2, 3, 5), 0 = off
    uint8_t clearCount = 0;             // rev 28: the host's ground clears so far (wraps); a change makes every machine clear its dropped weapons and loose armour
    uint8_t syncMode = 1;               // rev 29: SyncMode the next map runs (0 host physics, 1 per player); each machine takes it at a level change
    uint8_t forceStrength = 1;          // rev 34: the host's force_strength (0/1): 1 forces every player's passport Weight to the tier's (ForcedWeight); 0 leaves their own (clamped to 0..1)
    uint8_t teamKit = uint8_t(TeamKit::Full);   // rev 36: the host's Team colours row (hsm::TeamKit 0..2): every machine tints its own copies of the fighters that way
    uint8_t freeCam = 1;                // rev 36: the host allows the dead a free camera (MatchConfig::freeCam): 0/1
    uint8_t crossbows = 0;              // rev 36: the host's Crossbows row (hsm::Crossbows, 0..4): how many random players get a crossbow (match/hsm_crossbow.h)
    uint8_t tagIt = 0;                  // Wave 3, Tag: the entity that is "it" (a fighter in the roster; 0 = nobody, and every other format)
    uint16_t fuseLeftMs = 0;            // Wave 3, Tag: the milliseconds left on "it"'s fuse (0 without one); the HUD runs it on between MatchInfos
    uint16_t damagePct = 0xFFFF;        // rev 24: the Player Damage Rate everyone runs, percent (0..1000); 0xFFFF the stock one (match/hsm_damage.h)
    std::array<float, kPinnedPhysics> physics{ -1.f, -1.f, -1.f };   // rev 31: kPinnedPhysicsNames' values everyone runs, -1 = each its own
    std::vector<ScoreEntry> scores;
    std::vector<FighterEntry> fighters;
    std::vector<Objective> objectives;   // rev 37 (trailing): the match's objectives; Capture the flag's two flags (team 0's, then team 1's), else none (match/hsm_objective.h)
    // Tournament and Pit Bridge Duels (match/hsm_bracket.h, hsm_kingqueue.h), empty/0 in every other format. Tournament: queueOrder the
    // entities in seed order (0: left before his first game), bracketWinners the winner of every game in play order (0 undecided,
    // 0xFF empty), fightWinsA/B the rounds the pairing's two fighters (the bracket's current game, a then b) have won. Pit Bridge
    // Duels: queueOrder the queue (the first two fight), streak the wins in a row of the one who stays. Decode checks they hold together.
    std::vector<uint8_t> queueOrder, bracketWinners;
    uint8_t fightWinsA = 0, fightWinsB = 0, streak = 0;
};
struct Kill { uint8_t killer = 0, victim = 0; uint8_t flags = 0; };   // flags: 1 teamkill, 2 suicide, 4 environment, 8 gave up (Duel); 16 the round drawn with both down, 32 drawn with no hits (Duel: victim and killer are the two fighters)

// ---------------------------------------------------------------- byte stream helpers
class Writer {
public:
    std::vector<uint8_t> buf;
    void u8(uint8_t v) { buf.push_back(v); }
    void u16(uint16_t v) { for (int i = 0; i < 2; ++i) buf.push_back(uint8_t(v >> (8 * i))); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) buf.push_back(uint8_t(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) buf.push_back(uint8_t(v >> (8 * i))); }
    void i16(int16_t v) { u16(uint16_t(v)); }
    void f32(float v) { uint32_t u; std::memcpy(&u, &v, 4); u32(u); }
    void str(const std::string& s, size_t max) { size_t n = s.size() < max ? s.size() : max; u8(uint8_t(n)); buf.insert(buf.end(), s.begin(), s.begin() + long(n)); }
    void header(MsgType t) { u16(kMagic); u8(kProtocolRevision); u8(uint8_t(t)); }
};

class Reader {
public:
    Reader(const uint8_t* d, size_t n) : d_(d), n_(n) {}
    bool ok() const { return ok_; }
    bool done() const { return ok_ && p_ == n_; }
    uint8_t u8() { if (!need(1)) return 0; return d_[p_++]; }
    uint16_t u16() { if (!need(2)) return 0; uint16_t v = uint16_t(d_[p_] | d_[p_ + 1] << 8); p_ += 2; return v; }
    uint32_t u32() { if (!need(4)) return 0; uint32_t v = 0; for (int i = 3; i >= 0; --i) v = v << 8 | d_[p_ + size_t(i)]; p_ += 4; return v; }
    uint64_t u64() { uint64_t lo = u32(); uint64_t hi = u32(); return hi << 32 | lo; }
    int16_t i16() { return int16_t(u16()); }
    float f32() { uint32_t u = u32(); float v; std::memcpy(&v, &u, 4); if (!std::isfinite(v)) ok_ = false; return v; }
    std::string str(size_t max) { size_t len = u8(); if (len > max || !need(len)) { ok_ = false; return {}; } std::string s(reinterpret_cast<const char*>(d_ + p_), len); p_ += len; return s; }
    bool header(MsgType& t) { if (u16() != kMagic) ok_ = false; if (u8() != kProtocolRevision) ok_ = false; t = MsgType(u8()); return ok_; }
private:
    bool need(size_t k) { if (!ok_ || n_ - p_ < k) { ok_ = false; return false; } return true; }
    const uint8_t* d_; size_t n_; size_t p_ = 0; bool ok_ = true;
};

// Peek the message type without decoding the body. Returns false on bad magic/revision.
inline bool PeekType(const uint8_t* d, size_t n, MsgType& t) { Reader r(d, n); return r.header(t); }

// ---------------------------------------------------------------- encoders / decoders
inline std::vector<uint8_t> Encode(const Hello& m) { Writer w; w.header(MsgType::Hello); w.u8(m.revision); w.u64(m.buildKey); w.str(m.name, kMaxPlayerName); w.str(m.modBuild, kMaxModBuild); w.str(m.password, kMaxLobbyPassword); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, Hello& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Hello) return false;
    m.revision = r.u8(); m.buildKey = r.u64(); m.name = r.str(kMaxPlayerName); m.modBuild = r.str(kMaxModBuild); m.password = r.str(kMaxLobbyPassword); return r.done();
}

inline std::vector<uint8_t> Encode(const Welcome& m) { Writer w; w.header(MsgType::Welcome); w.u32(m.sessionId); w.u8(m.slot); w.u8(m.entity); w.u16(m.snapshotHz); w.str(m.map, 200); w.u8(m.syncMode); w.str(m.modBuild, kMaxModBuild); w.u8(m.flags); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, Welcome& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Welcome) return false;
    m.sessionId = r.u32(); m.slot = r.u8(); m.entity = r.u8(); m.snapshotHz = r.u16(); m.map = r.str(200); m.syncMode = r.u8(); m.modBuild = r.str(kMaxModBuild); m.flags = r.u8();
    return r.done() && m.snapshotHz >= 1 && m.snapshotHz <= 120 && m.syncMode <= 1 && (m.flags & ~(kWelcomeNoHostFighter | kWelcomeMesh | kWelcomePeerPing | kWelcomeVoice | kWelcomeVoiceAll)) == 0;
}

inline std::vector<uint8_t> Encode(const Reject& m) { Writer w; w.header(MsgType::Reject); w.u8(uint8_t(m.reason)); w.str(m.detail, kMaxRejectDetail); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, Reject& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Reject) return false;
    uint8_t v = r.u8(); if (v < 1 || v > kMaxRejectReason) return false; m.reason = RejectReason(v);
    m.detail = r.str(kMaxRejectDetail);
    for (char c : m.detail) { const unsigned char u = static_cast<unsigned char>(c); if (u < 0x20 || u >= 0x7F) return false; }
    return r.done();
}

inline void PutInputBody(Writer& w, const InputFrame& m) {
    w.u32(m.seq); w.u32(m.clientTimeMs);
    for (float a : m.axes) w.f32(a);
    w.u32(m.buttons);
    for (size_t i = 0; i < kActionCount; i += 2) w.u8(uint8_t((m.toggles[i] & 15) | ((i + 1 < kActionCount ? m.toggles[i + 1] & 15 : 0) << 4)));   // two actions a byte
    w.u8(m.lockTarget); w.f32(m.controlYaw); w.f32(m.controlPitch);
}
inline bool GetInputBody(Reader& r, InputFrame& m) {
    m.seq = r.u32(); m.clientTimeMs = r.u32();
    for (float& a : m.axes) { a = r.f32(); if (std::fabs(a) > kAxisLimit) return false; }
    m.buttons = r.u32();
    for (size_t i = 0; i < kActionCount; i += 2) {
        const uint8_t b = r.u8();
        m.toggles[i] = b & 15;
        if (i + 1 < kActionCount) m.toggles[i + 1] = b >> 4; else if (b >> 4) return false;   // the spare nibble stays zero
    }
    m.lockTarget = r.u8(); m.controlYaw = r.f32(); m.controlPitch = r.f32();
    if (m.buttons >> kActionCount) return false;
    if (m.lockTarget > kMaxWillies) return false;
    if (std::fabs(m.controlYaw) > 720.f || std::fabs(m.controlPitch) > 720.f) return false;
    return r.done();
}
inline std::vector<uint8_t> Encode(const InputFrame& m) { Writer w; w.header(MsgType::Input); PutInputBody(w, m); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, InputFrame& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Input) return false;
    return GetInputBody(r, m);
}

struct RelayInput { uint8_t entity = 0; InputFrame frame; };
inline std::vector<uint8_t> Encode(const RelayInput& m) { Writer w; w.header(MsgType::RelayInput); w.u8(m.entity); PutInputBody(w, m.frame); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, RelayInput& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::RelayInput) return false;
    m.entity = r.u8(); if (m.entity == 0 || m.entity > kMaxWillies) return false;
    return GetInputBody(r, m.frame);
}


inline void PutWillie(Writer& w, const WillieState& s) {
    w.u8(s.entity); w.u8(s.flags); w.f32(s.health); w.f32(s.consciousness); w.f32(s.stamina); w.f32(s.bleeding);
    for (float h : s.partHealth) w.f32(h);
    w.u32(s.dismembered); w.u8(uint8_t(s.bodies.size()));
    for (const BodyState& b : s.bodies) { w.f32(b.px); w.f32(b.py); w.f32(b.pz); w.i16(b.rot.x); w.i16(b.rot.y); w.i16(b.rot.z); w.i16(b.rot.w); }
}
inline bool GetWillie(Reader& r, WillieState& s) {
    s.entity = r.u8(); s.flags = r.u8(); s.health = r.f32(); s.consciousness = r.f32(); s.stamina = r.f32(); s.bleeding = r.f32();
    for (float& h : s.partHealth) h = r.f32();
    s.dismembered = r.u32(); size_t nb = r.u8(); if (nb > kMaxBodies) return false;
    s.bodies.resize(nb);
    for (BodyState& b : s.bodies) { b.px = r.f32(); b.py = r.f32(); b.pz = r.f32(); b.rot.x = r.i16(); b.rot.y = r.i16(); b.rot.z = r.i16(); b.rot.w = r.i16(); }
    return r.ok();
}

struct OwnerState { WillieState state; };
inline std::vector<uint8_t> Encode(const OwnerState& m) { Writer w; w.header(MsgType::OwnerState); PutWillie(w, m.state); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, OwnerState& m) {
    if (n > kMaxPacket) return false;
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::OwnerState) return false;
    if (!GetWillie(r, m.state) || !r.done()) return false;
    return m.state.entity >= 1 && m.state.entity <= kMaxWillies;
}

struct DeathReport { uint8_t victim = 0, killer = 0; uint8_t victimLife = 0; };   // killer 0 = environment / unknown; victimLife: the Spawn.life of the body that died (rev 19)
inline std::vector<uint8_t> Encode(const DeathReport& m) { Writer w; w.header(MsgType::DeathReport); w.u8(m.victim); w.u8(m.killer); w.u8(m.victimLife); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, DeathReport& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::DeathReport) return false;
    m.victim = r.u8(); m.killer = r.u8(); m.victimLife = r.u8();
    return r.done() && m.victim >= 1 && m.victim <= kMaxWillies && m.killer <= kMaxWillies;
}
struct GiveUpMsg { uint8_t entity = 0; };
inline std::vector<uint8_t> Encode(const GiveUpMsg& m) { Writer w; w.header(MsgType::GiveUp); w.u8(m.entity); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, GiveUpMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::GiveUp) return false;
    m.entity = r.u8();
    return r.done() && m.entity >= 1 && m.entity <= kMaxWillies;
}
inline std::vector<uint8_t> Encode(const Snapshot& m) {
    Writer w; w.header(MsgType::Snapshot); w.u32(m.tick); w.u32(m.ackInputSeq); w.u8(uint8_t(m.willies.size()));
    for (const WillieState& s : m.willies) PutWillie(w, s);
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, Snapshot& m) {
    if (n > kMaxPacket) return false;
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Snapshot) return false;
    m.tick = r.u32(); m.ackInputSeq = r.u32(); size_t nw = r.u8(); if (nw > kMaxWillies) return false;
    m.willies.resize(nw);
    for (WillieState& s : m.willies) if (!GetWillie(r, s)) return false;
    return r.done();
}

inline std::vector<uint8_t> Encode(const Event& m) { Writer w; w.header(MsgType::Event); w.u32(m.seq); w.u8(uint8_t(m.kind)); w.u32(m.a); w.u32(m.b); w.str(m.text, 200); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, Event& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Event) return false;
    m.seq = r.u32(); uint8_t k = r.u8(); if (k < 1 || k > 6) return false; m.kind = EventKind(k);
    m.a = r.u32(); m.b = r.u32(); m.text = r.str(200); return r.done();
}

inline std::vector<uint8_t> EncodePing(uint32_t ms, bool pong) { Writer w; w.header(pong ? MsgType::Pong : MsgType::Ping); w.u32(ms); return w.buf; }
inline bool DecodePing(const uint8_t* d, size_t n, Ping& m, bool& pong) {
    Reader r(d, n); MsgType t; if (!r.header(t) || (t != MsgType::Ping && t != MsgType::Pong)) return false;
    pong = t == MsgType::Pong; m.timeMs = r.u32(); return r.done();
}

inline std::vector<uint8_t> Encode(const Travel& m) {
    Writer w; w.header(MsgType::Travel); w.u32(m.seq); w.str(m.level, 255); w.u8(m.absolute ? 1 : 0); w.str(m.options, 255);
    size_t n = m.settings.size() < kMaxSettings ? m.settings.size() : kMaxSettings; w.u8(uint8_t(n));
    for (size_t i = 0; i < n; ++i) {
        const Setting& st = m.settings[i]; w.str(st.name, kMaxSettingName);
        size_t vb = st.value.size() < kMaxSettingBytes ? st.value.size() : kMaxSettingBytes; w.u8(uint8_t(vb));
        w.buf.insert(w.buf.end(), st.value.begin(), st.value.begin() + long(vb));
    }
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, Travel& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Travel) return false;
    m.seq = r.u32(); m.level = r.str(255); uint8_t abs = r.u8(); if (abs > 1) return false; m.absolute = abs == 1;
    m.options = r.str(255); size_t cnt = r.u8(); if (cnt > kMaxSettings || m.level.empty()) return false;
    m.settings.resize(cnt);
    for (Setting& st : m.settings) {
        st.name = r.str(kMaxSettingName); size_t vb = r.u8(); if (vb > kMaxSettingBytes || st.name.empty()) return false;
        st.value.resize(vb); for (uint8_t& b : st.value) b = r.u8();
    }
    return r.done();
}
inline std::vector<uint8_t> Encode(const Ready& m) { Writer w; w.header(MsgType::Ready); w.u32(m.travelSeq); w.str(m.level, 255); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, Ready& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Ready) return false;
    m.travelSeq = r.u32(); m.level = r.str(255); return r.done();
}

inline std::vector<uint8_t> Encode(const Spawn& m) {
    Writer w; w.header(MsgType::Spawn); w.u8(m.entity); w.u8(uint8_t(m.kind)); w.u8(m.ownerSlot); w.u8(uint8_t(m.team)); w.u32(m.blobId);
    w.f32(m.x); w.f32(m.y); w.f32(m.z); w.f32(m.yaw); w.u8(m.life); w.u8(m.flags); return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, Spawn& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Spawn) return false;
    m.entity = r.u8(); uint8_t k = r.u8(); m.ownerSlot = r.u8(); m.team = int8_t(r.u8()); m.blobId = r.u32();
    m.x = r.f32(); m.y = r.f32(); m.z = r.f32(); m.yaw = r.f32(); m.life = r.u8(); m.flags = r.u8();
    if (m.flags & ~(kSpawnCrossbow | kSpawnZombie)) return false;
    if (k < 1 || k > 2 || m.entity == 0 || m.team < -1 || m.team > 7) return false;
    if (std::fabs(m.x) > 1e7f || std::fabs(m.y) > 1e7f || std::fabs(m.z) > 1e7f || std::fabs(m.yaw) > 720.f) return false;
    m.kind = EntityKind(k); return r.done();
}
inline std::vector<uint8_t> Encode(const Despawn& m) { Writer w; w.header(MsgType::Despawn); w.u8(m.entity); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, Despawn& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Despawn) return false;
    m.entity = r.u8(); return r.done() && m.entity != 0;
}
inline std::vector<uint8_t> Encode(const BlobChunk& m) {
    Writer w; w.header(MsgType::BlobChunk); w.u32(m.blobId); w.u32(m.total); w.u16(m.index); w.u16(m.count); w.u32(m.crc);
    w.u16(uint16_t(m.data.size())); w.buf.insert(w.buf.end(), m.data.begin(), m.data.end()); return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, BlobChunk& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::BlobChunk) return false;
    m.blobId = r.u32(); m.total = r.u32(); m.index = r.u16(); m.count = r.u16(); m.crc = r.u32();
    size_t len = r.u16();
    if (m.blobId == 0 || m.total == 0 || m.total > kMaxBlobBytes || len == 0 || len > kBlobChunkBytes) return false;
    if (m.count != (m.total + kBlobChunkBytes - 1) / kBlobChunkBytes || m.index >= m.count) return false;
    size_t expect = m.index + 1u == m.count ? m.total - size_t(m.index) * kBlobChunkBytes : kBlobChunkBytes;
    if (len != expect) return false;
    m.data.resize(len); for (uint8_t& b : m.data) b = r.u8();
    return r.done();
}

inline std::vector<uint8_t> Encode(const MatchInfo& m) {
    Writer w; w.header(MsgType::MatchInfo);
    w.u8(m.format); w.u8(m.teams); w.u16(m.scoreLimit); w.u16(m.timeLimitS); w.u16(m.respawnDelayS); w.u8(m.friendlyFire ? 1 : 0);
    w.u8(m.phase); w.u32(m.timeLeftMs); w.i16(m.winner);
    w.u8(m.gear); w.u8(m.tier); w.u16(m.round); w.u32(m.roundGapLeftMs); w.u8(m.roundWinner); w.u16(m.damagePct);
    w.u16(m.waveBreakS); w.u8(m.clearGroundMin); w.u8(m.clearCount); w.u8(m.syncMode); w.u8(m.forceStrength); w.u8(m.teamKit); w.u8(m.freeCam); w.u8(m.crossbows);
    w.u8(m.tagIt); w.u16(m.fuseLeftMs);
    for (float v : m.physics) w.f32(v);
    size_t ns = m.scores.size() < 255 ? m.scores.size() : 255; w.u8(uint8_t(ns));
    for (size_t i = 0; i < ns; ++i) { w.i16(m.scores[i].key); w.i16(m.scores[i].score); }
    size_t nf = m.fighters.size() < 255 ? m.fighters.size() : 255; w.u8(uint8_t(nf));
    for (size_t i = 0; i < nf; ++i) {
        const FighterEntry& f = m.fighters[i];
        w.u8(f.entity); w.u8(uint8_t(f.team)); w.i16(f.kills); w.i16(f.deaths); w.str(f.name, kMaxPlayerName); w.u16(f.pingMs);
        w.u8(uint8_t((f.alive ? 1 : 0) | (f.down ? 2 : 0)));   // bit 0 alive, bit 1 down
        w.u64(f.steamId);
        w.u8(f.role);
    }
    size_t no = m.objectives.size() < kMaxObjectives ? m.objectives.size() : kMaxObjectives; w.u8(uint8_t(no));
    for (size_t i = 0; i < no; ++i) {
        const Objective& o = m.objectives[i];
        w.u8(uint8_t(o.kind)); w.u8(uint8_t(o.owner)); w.u8(uint8_t(o.state)); w.u8(o.carrier);
        w.f32(o.x); w.f32(o.y); w.f32(o.z); w.u16(uint16_t(o.timerMs));
    }
    const size_t nq = m.queueOrder.size() < kMaxBracketPlayers ? m.queueOrder.size() : kMaxBracketPlayers; w.u8(uint8_t(nq));
    for (size_t i = 0; i < nq; ++i) w.u8(m.queueOrder[i]);
    const size_t nw = m.bracketWinners.size() < kMaxBracketPlayers ? m.bracketWinners.size() : kMaxBracketPlayers; w.u8(uint8_t(nw));
    for (size_t i = 0; i < nw; ++i) w.u8(m.bracketWinners[i]);
    w.u8(m.fightWinsA); w.u8(m.fightWinsB); w.u8(m.streak);
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, MatchInfo& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::MatchInfo) return false;
    m.format = r.u8(); m.teams = r.u8(); m.scoreLimit = r.u16(); m.timeLimitS = r.u16(); m.respawnDelayS = r.u16();
    uint8_t ff = r.u8(); m.phase = r.u8(); m.timeLeftMs = r.u32(); m.winner = r.i16();
    m.gear = r.u8(); m.tier = r.u8(); m.round = r.u16(); m.roundGapLeftMs = r.u32(); m.roundWinner = r.u8(); m.damagePct = r.u16();
    m.waveBreakS = r.u16(); m.clearGroundMin = r.u8(); m.clearCount = r.u8(); m.syncMode = r.u8(); m.forceStrength = r.u8(); m.teamKit = r.u8(); m.freeCam = r.u8(); m.crossbows = r.u8();
    m.tagIt = r.u8(); m.fuseLeftMs = r.u16();
    for (float& v : m.physics) { v = r.f32(); if (!ValidPinnedPhysics(v)) return false; }
    if (m.syncMode > 1 || m.forceStrength > 1 || m.teamKit >= kTeamKitCount || m.freeCam > 1 || !ValidCrossbows(m.crossbows)) return false;
    if (m.format >= kFormatCount || m.teams < kMinTeams || m.teams > kMaxTeams || ff > 1 || m.phase > 4 || m.scoreLimit < 1 || m.scoreLimit > 100 || m.timeLimitS > 3600 || m.respawnDelayS > 60) return false;
    if (m.tagIt > kMaxWillies || m.fuseLeftMs > uint16_t(kFuseMaxS) * 1000u || (!m.tagIt && m.fuseLeftMs)) return false;   // a fuse needs its "it", and runs the Fuse row's 60 s at most
    if (m.gear >= kGearCount || m.tier > 7 || (m.damagePct != 0xFFFF && m.damagePct > 1000)) return false;
    if (!ValidWaveBreakS(m.waveBreakS) || !ValidClearGroundMin(m.clearGroundMin)) return false;
    m.friendlyFire = ff == 1;
    size_t ns = r.u8(); if (ns > kMaxWillies) return false;
    m.scores.resize(ns); for (ScoreEntry& e : m.scores) { e.key = r.i16(); e.score = r.i16(); }
    size_t nf = r.u8(); if (nf > kMaxWillies) return false;
    m.fighters.resize(nf);
    for (FighterEntry& f : m.fighters) {
        f.entity = r.u8(); f.team = int8_t(r.u8()); f.kills = r.i16(); f.deaths = r.i16(); f.name = r.str(kMaxPlayerName);
        f.pingMs = r.u16(); uint8_t alive = r.u8(); f.alive = (alive & 1) != 0; f.down = (alive & 2) != 0;
        f.steamId = r.u64(); f.role = r.u8();
        if (!ValidRoleId(f.role)) return false;
        if (f.entity == 0 || f.team < -1 || f.team > 7) return false;
        if (alive > 3) return false;
    }
    size_t no = r.u8(); if (no > kMaxObjectives) return false;
    m.objectives.assign(no, Objective{});
    for (Objective& o : m.objectives) {
        o.kind = ObjectiveKind(r.u8()); o.owner = int8_t(r.u8()); o.state = ObjectiveState(r.u8()); o.carrier = r.u8();
        o.x = r.f32(); o.y = r.f32(); o.z = r.f32(); o.timerMs = r.u16();
        if (!ValidObjective(o)) return false;
    }
    size_t nq = r.u8(); if (nq > kMaxBracketPlayers) return false;
    m.queueOrder.resize(nq); for (uint8_t& e : m.queueOrder) e = r.u8();
    size_t nw = r.u8(); if (nw > kMaxBracketPlayers) return false;
    m.bracketWinners.resize(nw); for (uint8_t& e : m.bracketWinners) e = r.u8();
    m.fightWinsA = r.u8(); m.fightWinsB = r.u8(); m.streak = r.u8();
    if (!r.done()) return false;
    const bool tournament = m.format == uint8_t(Format::Tournament), pit = m.format == uint8_t(Format::PitBridgeDuel);
    for (uint8_t e : m.queueOrder) if (e > kMaxWillies) return false;   // 0 is a seed who left, in a bracket only (the validators say)
    if (tournament) return ValidBracketWire(m.queueOrder, m.bracketWinners) && m.streak == 0 && m.fightWinsA <= 3 && m.fightWinsB <= 3;
    if (pit) return ValidQueueWire(m.queueOrder, m.streak) && m.bracketWinners.empty() && m.fightWinsA == 0 && m.fightWinsB == 0;
    return m.queueOrder.empty() && m.bracketWinners.empty() && m.fightWinsA == 0 && m.fightWinsB == 0 && m.streak == 0;
}
inline std::vector<uint8_t> Encode(const Kill& m) { Writer w; w.header(MsgType::Kill); w.u8(m.killer); w.u8(m.victim); w.u8(m.flags); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, Kill& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Kill) return false;
    m.killer = r.u8(); m.victim = r.u8(); m.flags = r.u8();
    return r.done() && m.victim != 0 && m.flags < 64;
}

// Wave 3: a mode's one-off event, for the kill feed (and for a fuse, to the fighter it burns). a and b are entities.
//   kTagPass    a tagged b (the fuse is b's now)
//   kTagBurn    a's fuse ran out: a is out for the round (its owner's fighter dies)
//   kInfected   a infected b (a = 0: the world or nobody); b is infected and respawns
enum class ModeEventKind : uint8_t { TagPass = 1, TagBurn = 2, Infected = 3 };
struct ModeEventMsg { ModeEventKind kind = ModeEventKind::TagPass; uint8_t a = 0, b = 0; };
inline std::vector<uint8_t> Encode(const ModeEventMsg& m) { Writer w; w.header(MsgType::ModeEvent); w.u8(uint8_t(m.kind)); w.u8(m.a); w.u8(m.b); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, ModeEventMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::ModeEvent) return false;
    const uint8_t k = r.u8(); m.a = r.u8(); m.b = r.u8();
    if (!r.done() || k < uint8_t(ModeEventKind::TagPass) || k > uint8_t(ModeEventKind::Infected)) return false;
    m.kind = ModeEventKind(k);
    if (m.a > kMaxWillies || m.b > kMaxWillies) return false;
    if (m.kind == ModeEventKind::TagPass) return m.a != 0 && m.b != 0 && m.a != m.b;
    if (m.kind == ModeEventKind::TagBurn) return m.a != 0;
    return m.b != 0;                                  // Infected: a may be 0
}

// Mesh (rev 19). The host tells each client who else is in the session so clients can stream Pose/Vitals to each other;
// a client tells the host which slots it hears directly so the host stops relaying those to it.
constexpr size_t kMaxPeerMap = 9;   // == kMaxFighters (session/hsm_roster.h; static_assert in session/hsm_authority.h)
struct PeerMapEntry { uint8_t slot = 0; uint64_t steamId = 0; };
struct PeerMapMsg { std::vector<PeerMapEntry> peers; };
inline std::vector<uint8_t> Encode(const PeerMapMsg& m) {
    Writer w; w.header(MsgType::PeerMap);
    const size_t n = m.peers.size() < kMaxPeerMap ? m.peers.size() : kMaxPeerMap; w.u8(uint8_t(n));
    for (size_t i = 0; i < n; ++i) { w.u8(m.peers[i].slot); w.u64(m.peers[i].steamId); }
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, PeerMapMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::PeerMap) return false;
    const size_t cnt = r.u8(); if (cnt > kMaxPeerMap) return false;
    m.peers.assign(cnt, PeerMapEntry{});
    uint32_t seen = 0;
    for (PeerMapEntry& e : m.peers) {
        e.slot = r.u8(); e.steamId = r.u64();
        if (e.slot >= kMaxPeerMap || e.steamId == 0 || (seen >> e.slot & 1)) return false;
        seen |= 1u << e.slot;
    }
    return r.done();
}
struct MeshHeardMsg { uint16_t mask = 0; };   // bit s = this client received slot s's Pose or Vitals directly lately
inline std::vector<uint8_t> Encode(const MeshHeardMsg& m) { Writer w; w.header(MsgType::MeshHeard); w.u16(m.mask); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, MeshHeardMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::MeshHeard) return false;
    m.mask = r.u16(); return r.done() && (m.mask >> kMaxPeerMap) == 0;
}

// Ping table (rev 22): what one client measured to the host (slot 0) and to the other clients (session/hsm_peer_ping.h).
// rttMs and steamMs 0xFFFF: not measured. One entry per slot at most, never the sender's own.
struct PingTableEntry { uint8_t slot = 0; uint16_t rttMs = 0xFFFF; uint16_t steamMs = 0xFFFF; uint8_t lossPct = 0; };
struct PingTableMsg { std::vector<PingTableEntry> entries; };
inline std::vector<uint8_t> Encode(const PingTableMsg& m) {
    Writer w; w.header(MsgType::PingTable);
    const size_t n = m.entries.size() < kMaxPeerMap ? m.entries.size() : kMaxPeerMap; w.u8(uint8_t(n));
    for (size_t i = 0; i < n; ++i) {
        const PingTableEntry& e = m.entries[i];
        w.u8(e.slot); w.u16(e.rttMs); w.u16(e.steamMs); w.u8(e.lossPct);
    }
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, PingTableMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::PingTable) return false;
    const size_t cnt = r.u8(); if (cnt > kMaxPeerMap) return false;
    m.entries.assign(cnt, PingTableEntry{});
    uint32_t seen = 0;
    for (PingTableEntry& e : m.entries) {
        e.slot = r.u8(); e.rttMs = r.u16(); e.steamMs = r.u16(); e.lossPct = r.u8();
        if (e.slot >= kMaxPeerMap || e.lossPct > 100 || (seen >> e.slot & 1)) return false;
        seen |= 1u << e.slot;
    }
    return r.done();
}

// Voice (rev 22): one Steam-compressed microphone chunk. A client sends its own (entity = its fighter); the host
// relays it to the players who may hear it (voice/hsm_voice.h) with the entity it checked. flags: kVoiceSpurt, the
// first chunk of a talk spurt; kVoiceAdpcm (rev 23), the data is IMA ADPCM at 16 kHz (voice/hsm_adpcm.h), recorded
// by the mod from a microphone the player picked, instead of Steam-compressed.
// Rev 24: voice is each player's own setting, carried by the speaker's chunks: kVoiceEveryone, the speaker chose
// Everyone (else Proximity; Off sends nothing); kVoiceNoCut, the speaker keeps talking when knocked out or low on
// consciousness (voice_cut=0; the dead still talk only to the dead).
constexpr uint8_t kVoiceSpurt = 1, kVoiceAdpcm = 2, kVoiceNoCut = 4, kVoiceEveryone = 8;
constexpr size_t kMaxVoiceBytes = 4096;   // over a second of speech: only a long hitch makes a chunk this big
struct VoiceMsg { uint8_t entity = 0; uint16_t seq = 0; uint8_t flags = 0; std::vector<uint8_t> data; };
inline std::vector<uint8_t> Encode(const VoiceMsg& m) {
    Writer w; w.header(MsgType::Voice); w.u8(m.entity); w.u16(m.seq); w.u8(m.flags);
    const size_t len = m.data.size() < kMaxVoiceBytes ? m.data.size() : kMaxVoiceBytes;
    w.u16(uint16_t(len)); w.buf.insert(w.buf.end(), m.data.begin(), m.data.begin() + long(len));
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, VoiceMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Voice) return false;
    m.entity = r.u8(); m.seq = r.u16(); m.flags = r.u8();
    const size_t len = r.u16();
    if (!r.ok() || m.entity == 0 || (m.flags & ~(kVoiceSpurt | kVoiceAdpcm | kVoiceNoCut | kVoiceEveryone)) != 0 || len == 0 || len > kMaxVoiceBytes || n < 10 || n - 10 != len) return false;   // 4 header + 6 fields
    m.data.assign(d + 10, d + n);
    return true;
}

// VoiceMode (rev 23): the host's voice setting changed mid-session (voice/hsm_voice.h Reach: 0 Off, 1 Proximity,
// 2 Everyone). Unused since rev 24 (voice is each player's own; kWelcomeVoice* too); still decoded.
struct VoiceModeMsg { uint8_t reach = 0; };
inline std::vector<uint8_t> Encode(const VoiceModeMsg& m) { Writer w; w.header(MsgType::VoiceMode); w.u8(m.reach); return w.buf; }
inline bool Decode(const uint8_t* d, size_t n, VoiceModeMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::VoiceMode) return false;
    m.reach = r.u8(); return r.done() && m.reach <= 2;
}

// Detours (rev 23, session/hsm_route.h, session/hsm_detour.h). The host plans each client pair's path and sends every
// client its RouteTable: for each other client, the path (direct, through the host, or through a relay client) and the
// pair's key; and the pairs this client relays for. A Forward carries one Pose or Vitals message from `from` to `to`
// through the relay, with a tag only the pair's two ends can make (the relay never gets the key). epoch: the table's
// number; a Forward from another epoch is dropped.
constexpr uint8_t kRouteViaDirect = 0xFE;   // RouteKey.via: straight to the peer (session/hsm_route.h kRouteDirect)
constexpr uint8_t kRouteViaHost = 0;        // RouteKey.via: through the host (slot 0), as before detours
constexpr size_t kRouteKeyBytes = 16;
constexpr size_t kMaxRoutePairs = kMaxPeerMap * (kMaxPeerMap - 1) / 2;
struct RouteKey { uint8_t peer = 0; uint8_t via = kRouteViaHost; std::array<uint8_t, kRouteKeyBytes> key{}; };
struct RoutePair { uint8_t a = 0, b = 0; };   // a < b; the relay passes on both directions
struct RouteTableMsg { uint8_t epoch = 0; std::vector<RouteKey> keys; std::vector<RoutePair> relays; };
inline std::vector<uint8_t> Encode(const RouteTableMsg& m) {
    Writer w; w.header(MsgType::RouteTable); w.u8(m.epoch);
    const size_t k = m.keys.size() < kMaxPeerMap ? m.keys.size() : kMaxPeerMap; w.u8(uint8_t(k));
    for (size_t i = 0; i < k; ++i) { w.u8(m.keys[i].peer); w.u8(m.keys[i].via); w.buf.insert(w.buf.end(), m.keys[i].key.begin(), m.keys[i].key.end()); }
    const size_t p = m.relays.size() < kMaxRoutePairs ? m.relays.size() : kMaxRoutePairs; w.u8(uint8_t(p));
    for (size_t i = 0; i < p; ++i) { w.u8(m.relays[i].a); w.u8(m.relays[i].b); }
    return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, RouteTableMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::RouteTable) return false;
    m.epoch = r.u8();
    const size_t k = r.u8(); if (k > kMaxPeerMap) return false;
    m.keys.assign(k, RouteKey{});
    uint32_t seen = 0;
    for (RouteKey& e : m.keys) {
        e.peer = r.u8(); e.via = r.u8();
        for (uint8_t& b : e.key) b = r.u8();
        if (e.peer == 0 || e.peer >= kMaxPeerMap || (seen >> e.peer & 1)) return false;   // clients only, once each
        if (e.via != kRouteViaDirect && (e.via >= kMaxPeerMap || e.via == e.peer)) return false;
        seen |= 1u << e.peer;
    }
    const size_t p = r.u8(); if (p > kMaxRoutePairs) return false;
    m.relays.assign(p, RoutePair{});
    for (size_t i = 0; i < p; ++i) {
        RoutePair& e = m.relays[i];
        e.a = r.u8(); e.b = r.u8();
        if (e.a == 0 || e.a >= e.b || e.b >= kMaxPeerMap) return false;
        for (size_t j = 0; j < i; ++j) if (m.relays[j].a == e.a && m.relays[j].b == e.b) return false;
    }
    return r.done();
}

constexpr size_t kForwardHeader = 4 + 1 + 1 + 1 + 2 + 8 + 2;   // header, from, to, epoch, seq, tag, length
constexpr size_t kMaxForwardBytes = kMaxPacket - kForwardHeader;
struct ForwardMsg { uint8_t from = 0, to = 0, epoch = 0; uint16_t seq = 0; uint64_t tag = 0; std::vector<uint8_t> payload; };
inline std::vector<uint8_t> Encode(const ForwardMsg& m) {
    Writer w; w.header(MsgType::Forward); w.u8(m.from); w.u8(m.to); w.u8(m.epoch); w.u16(m.seq); w.u64(m.tag);
    const size_t len = m.payload.size() < kMaxForwardBytes ? m.payload.size() : kMaxForwardBytes;
    w.u16(uint16_t(len)); w.buf.insert(w.buf.end(), m.payload.begin(), m.payload.begin() + long(len));
    return w.buf;
}
// The payload must be a whole message of this revision, a Pose or a Vitals (never another Forward); the receiver
// decodes it after the tag checks.
inline bool Decode(const uint8_t* d, size_t n, ForwardMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::Forward) return false;
    m.from = r.u8(); m.to = r.u8(); m.epoch = r.u8(); m.seq = r.u16(); m.tag = r.u64();
    const size_t len = r.u16();
    if (!r.ok() || m.from == 0 || m.to == 0 || m.from >= kMaxPeerMap || m.to >= kMaxPeerMap || m.from == m.to) return false;
    if (len == 0 || len > kMaxForwardBytes || n < kForwardHeader || n - kForwardHeader != len) return false;
    MsgType inner; if (!PeekType(d + kForwardHeader, len, inner) || (inner != MsgType::Pose && inner != MsgType::Vitals)) return false;
    m.payload.assign(d + kForwardHeader, d + n);
    return true;
}

// VoteState (rev 23): the votekick as the host counts it, sent to each client when it changes and each second while it
// runs. target: the slot being voted on (kVoteNone: no vote). mine: this client's vote (1 yes, -1 no, 0 not yet,
// kVoteNotVoter: not a voter, e.g. the target). ban: 1 for a voteban (role=server, 7 days), 0 for a votekick. Votes are
// cast as chat lines (/votekick #slot, /voteban #slot, /yes, /no).
constexpr uint8_t kVoteNone = 0xFF;
constexpr int8_t kVoteNotVoter = -2;
struct VoteStateMsg { uint8_t target = kVoteNone; uint8_t yes = 0, no = 0, needed = 0, voters = 0, secondsLeft = 0; int8_t mine = kVoteNotVoter; uint8_t ban = 0; };
inline std::vector<uint8_t> Encode(const VoteStateMsg& m) {
    Writer w; w.header(MsgType::VoteState); w.u8(m.target); w.u8(m.yes); w.u8(m.no); w.u8(m.needed); w.u8(m.voters); w.u8(m.secondsLeft);
    w.u8(uint8_t(m.mine)); w.u8(m.ban); return w.buf;
}
inline bool Decode(const uint8_t* d, size_t n, VoteStateMsg& m) {
    Reader r(d, n); MsgType t; if (!r.header(t) || t != MsgType::VoteState) return false;
    m.target = r.u8(); m.yes = r.u8(); m.no = r.u8(); m.needed = r.u8(); m.voters = r.u8(); m.secondsLeft = r.u8(); m.mine = int8_t(r.u8()); m.ban = r.u8();
    if (!r.done()) return false;
    if (m.ban > 1 || (m.target == kVoteNone && m.ban)) return false;
    if (m.target == kVoteNone) return m.yes == 0 && m.no == 0 && m.needed == 0 && m.voters == 0 && m.secondsLeft == 0 && m.mine == kVoteNotVoter;
    return m.target < kMaxPeerMap && m.voters <= kMaxPeerMap && m.yes + m.no <= m.voters && m.needed <= m.voters && m.secondsLeft <= 60 &&
           m.mine >= kVoteNotVoter && m.mine <= 1;
}

// ---------------------------------------------------------------- quantization
inline Quat16 PackQuat(float x, float y, float z, float w) {
    float len = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(len > 1e-6f) || !std::isfinite(len)) return {};
    if (w < 0) { x = -x; y = -y; z = -z; w = -w; }   // canonical hemisphere
    auto q = [len](float v) { float s = v / len * 32767.f; return int16_t(s > 32767.f ? 32767 : s < -32767.f ? -32767 : std::lround(s)); };
    return { q(x), q(y), q(z), q(w) };
}
inline void UnpackQuat(const Quat16& p, float& x, float& y, float& z, float& w) {
    x = p.x / 32767.f; y = p.y / 32767.f; z = p.z / 32767.f; w = p.w / 32767.f;
    float len = std::sqrt(x * x + y * y + z * z + w * w);
    if (len > 1e-6f) { x /= len; y /= len; z /= len; w /= len; } else { x = y = z = 0; w = 1; }
}

// ---------------------------------------------------------------- sequence helpers
// True if a is newer than b with 32-bit wraparound.
inline bool SeqNewer(uint32_t a, uint32_t b) { return int32_t(a - b) > 0; }

}  // namespace hsm
