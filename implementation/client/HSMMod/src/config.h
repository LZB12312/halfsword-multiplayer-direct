// HSMMod.ini next to the game exe. Missing file or role=off keeps the game fully vanilla.
#pragma once
#include "match/hsm_class.h"
#include "hsm_chat.h"
#include "hsm_inputdelay.h"
#include "session/hsm_structure.h"
#include "voice/hsm_voice_ui.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace hsm {
// Steam: lobby-driven (Multiplayer button on the main menu; lobby owner hosts). Server: below.
enum class Role { Off, Direct, Server };   // Server: a hosted retail instance with no fighter of its own, auto-starting matches

struct Config {
    Role role = Role::Direct;
    std::string playerName = "Player";
    int snapshotHz = 30;
    int inputHz = 60;
    // 1: send input only for host physics or hand-off; capture still drains in owner mode. 0 restores the always-on uplink.
    bool inputOnDemand = true;
    // role=server only (0.9.0): sync_mode=host runs host physics (the server simulates every fighter, SyncMode::Host);
    // owner (the default) is per player. A Steam lobby host picks it on the lobby page (the Sync row) instead.
    bool hostPhysics = false;
    // true: clients also send Pose and Vitals straight to each other (Steam lobbies only; session/hsm_mesh.h), and the host
    // stops relaying what a client already hears directly (default from 0.7.0-pre6). false (mesh=0): every stream goes
    // through the host, as in 0.6.5. Used where the host and the client both have it on.
    bool mesh = false;
    // false (the default after 0.8.0-pre14): no hand-off (sync/hsm_handoff.h, rev 32). Every fighter stays with its own player,
    // grabs included, as in 0.8.0-pre7. hand_off=1 turns it back on; it happens only where the host and both players have it.
    bool handOff = false;
    // true (default from 0.8.0-pre17): a grab on any bone drags the way a down body's foot did in 0.8.0-pre7: the held
    // limb or torso part left to the hold on the grabber's screen, part of the pull on the held player's pelvis and
    // chest, and no snap of a held stand-in (sync/hsm_grab.h DragYieldBones). grab_drag=0: as 0.8.0-pre16. Per machine.
    bool grabDrag = true;
    // How a level prop built of several bodies joined by physics constraints (the Pit's bridge, a chain) is synced
    // (session/hsm_structure.h). soft (default from 0.8.0-pre18): the host streams all its bodies as one snapshot at 20 Hz and
    // the others steer their copy toward it with velocity, never teleporting a body; the map reset leaves it be. local: every
    // machine simulates its own copy, nothing streams it. full: as 0.8.0-pre17 (streamed and teleported). The old 0 reads as
    // soft, 1 as full. Per machine.
    StructureMode syncStructures = StructureMode::Soft;
    // true (default): under soft, a barrel, crate or plank that broke on this machine alone follows the host's, piece by piece.
    // structure_follow_broken=0: it is left to the physics here, as 0.8.0-pre30. Per machine.
    bool structureFollowBroken = true;
    // A break of a structure that could not go to the host at once (just after a snap, or over the report budget) waits and is
    // passed on, so every copy breaks alike. structure_share_held_breaks=0: it stays on this screen, as 0.8.0-pre30. Per machine.
    bool structureShareHeld = true;
    // true (default from 0.8.0-pre35): under soft, while the host's copy of a late-found barrel, crate or plank moves, this machine's
    // copy is placed from the host's states as a rock is, not only steered by velocity. structure_follow_moving=0: steered, as
    // 0.8.0-pre34. Per machine.
    bool structureFollowMoving = true;
    // true (default from 0.8.0-pre36): a hanging bridge takes the rest lock's placing and the moving follow too, all its bodies at
    // once. structure_bridges_follow=0: it is steered, as 0.8.0-pre35. Per machine.
    bool structureBridgesFollow = true;
    // true (default from 0.8.0-pre36): a barrel plank that came off on another machine comes off here by the barrel's own code, so
    // its collision, sound and Health match. structure_game_break=0: only taken off, as 0.8.0-pre35. Per machine.
    bool structureGameBreak = true;
    // true (default): a client records where each free physics prop lay early in a fight level and matches the census on that
    // place, so a roll before the first match does not cost a home (ObjectSync). prop_census_snapshot=0: the place at the first
    // match, as 0.8.0-pre34. Per machine.
    bool propCensusSnapshot = true;
    // A soft structure at rest (sync/hsm_rest_lock.h): a copy far from the host's rest pose is placed once to it, then held still
    // until something touches it here or the host's root moves. structure_rest_lock=0: steered as 0.8.0-pre34. Per machine.
    bool structureRestLock = true;
    // true (default from 0.8.0-pre19): under host physics a client's camera lock-on works again (sync/hsm_lockon.h): the client
    // aims at its target itself, as the game's lock-on does, and tells the host which fighter it holds. host_lock_on=0: no lock-on
    // for a client under host physics, as up to 0.8.0-pre18. Per machine (the client's).
    bool hostLockOn = true;
    // true (default from 0.8.0-pre19): the host plays other players' input on their fighters with fewer game calls
    // (sync/hsm_drivelean.h: a held guard is fed once, an unchanged aim is not set again, the game instance is found
    // once). drive_lean=0: fed as in 0.8.0-pre17. Host only.
    bool driveLean = true;
    // true (default from 0.8.0-pre20): every player's build (the passport's Weight: body mass and grip strength) is forced to
    // the lobby tier's, as a bot of that tier is built (shared match/hsm_match.h ForcedWeight; Brawl: the default 0.5), so no
    // player is heavier or stronger-gripped than another because of their Free Mode passport (#294 #295 #301). force_strength=0:
    // each keeps their own build, clamped to 0..1. The host's value rules the match (MatchInfo.forceStrength).
    bool forceStrength = true;
    // Host physics, the host's own input (hsm_inputdelay.h): auto (default) delays its fighter's buttons and movement by
    // the clients' median input lag so the fight is even; 0 off; <ms> fixed (0-200). INI only, never in the lobby. Host only.
    HostDelaySetting hostInputDelay;
    // true (default from 0.8.0-pre19): a sheathed weapon that hangs in a scabbard has the scabbard's own joint moved to the
    // Mesh (not the slot constraint) and a watchdog puts a slot weapon that stays 40 cm from its socket back
    // (standin_slots.h). scabbard_fix=0: as 0.8.0-pre18. Per machine; owner sync too.
    bool scabbardFix = true;
    // true (default): collision between a stand-in and other fighters' Box1, the invisible box behind a fighter's back, is
    // turned off as the game does between live fighters (gameplay/fighter_boxes.h). standin_box_fix=0 leaves it on. Per machine.
    bool standInBoxFix = true;
    // A test switch, off by default: every stand-in here has Box1 stop blocking bodies, and its DriverSkeleton and
    // CharacterMesh_Head lose collision, all back when it becomes a live fighter (standin_rig.cpp). standin_parts_off=1.
    // Per machine; owner sync.
    bool standInPartsOff = false;
    // true (default from 0.8.0-pre24): the Fenced-ring scenario keeps its BP_Fence_Bags: the level manager's own
    // level-load reset of "Current Lose Condition" is followed by the mod setting it back to 3 before the level script
    // destroys the bags (#706). fence_fix=0: as 0.8.0-pre23 (the fence never survives a level load). Per machine:
    // every machine's level manager wipes its own game instance, so every player in the fight needs it on.
    bool fenceFix = true;
    // true (default from 0.8.0-pre26): a blade lodged in my fighter on the attacker's screen does no stuck damage here where
    // my own replay of that blow found my armour stopped the whole cut (combat/hsm_armor_gate.h): the attacker's copy of my
    // fighter lags mine, so a blade could lodge there through armour that held here. stuck_armor_gate=0: every stuck
    // blade is replayed, as 0.8.0-pre25. Per machine: the victim's setting applies.
    bool stuckArmorGate = true;
    // true (default from 0.7.0-pre6): with the mesh on, the host also plans each client pair's fastest path from the
    // measured pings and a client may pass another pair's Pose and Vitals on (detours, session/hsm_detour.h). detour=0
    // turns it off; nothing happens with mesh=0.
    bool detour = false;
    // true (default): on a Steam lobby every client pings the host and every other client every 2 s and reports the
    // results to the host every 10 s; both log them in the health window ("ping:" lines). Measures what a mesh would get
    // without turning it on. Host and clients both need it; mesh=1 is not needed.
    bool peerPing = false;
    // true (default): when a Steam lobby's host leaves, the member Steam makes the new lobby owner hosts the same lobby
    // and the others follow it (session/hsm_migration.h) instead of everyone going back to the menu.
    bool hostMigration = false;
    // true (default): the host resets the map at each Duel round and each rematch (props back to their start, objects spawned
    // in the round gone, the level's own weapons back on their spots); 0 sends no MapReset. Read on the host only.
    bool mapReset = true;
    // true (default): in the Tavern hub of a hosted Progression session the host puts the game's next fights to a vote
    // (chat /fight <number>, 30 s) and starts the winner; 0 leaves the choice to the host's own hub UI. Read on the host only.
    bool progressionVote = true;
    // true (default): a hosted Progression fight pays the host's career money in-session (#620): a win adds the purse the
    // suppressed win screen would have paid, a loss keeps the bet lost (stock), and either way the pending reward is spent.
    // 0: money never moves. Read on the host only; it lands in the multiplayer career (progressionSave).
    bool progressionPayout = true;
    // true (default): a session's Progression plays on this machine's multiplayer career, HSMMP_Progression.sav beside the
    // game's GameProgress.sav (session/hsm_mp_career.h): a new one the first time (character creation in the Tavern), and
    // what the session does to it is kept: written in the Tavern after each fight, when the session leaves Progression and
    // when it ends. The player's own single-player save is never touched. 0: the session plays on the career in memory
    // and nothing it does is kept (the behaviour up to 0.8.0-pre27). Each machine's own setting, under [net].
    bool progressionSave = true;
    // true (default): a Progression co-op fight's end does to this machine's career what the stock end screens would have
    // (session/hsm_career.h, ProgressionResult 69): the game's own capture (each limb heals 10, between 25 and 100; a clean
    // win keeps the gear held and worn), on a win the whole purse and half of it as reputation, a Tourney won ranks up, and
    // the time moves one step (the host's clock). A fight then starts with the career's injuries. 0: only the host's purse
    // is paid (progressionPayout), as up to 0.8.0-pre27. Each machine's own setting, under [net].
    bool progressionFightEnd = true;
    // A Progression co-op fight won (every opponent dead, given up or downed) waits for a player to hold G, as the stock Talk
    // key after a win, at most 2 min (session/hsm_career.h CoopResultsMs). 0: back after 3 s, as up to pre27. The host's setting.
    bool progressionClaimG = true;
    // A multi-round contest (a Tourney's 7 rounds, a Duel's 3 to 6) plays its rounds in the same arena, the purse paid after
    // the last (ContestAfterFight). 0: one fight pays the whole purse, as up to pre27. The host's setting.
    bool progressionRounds = true;
    // Voice chat (gameplay/voice.h). Each player's own setting (0.7.1): voice and voiceReach are off (the default), by
    // distance (proximity: full volume within voiceNearM, silent past voiceFarM) or every living player hears every
    // living one (everyone); two players hear each other at the narrower of their settings. A fighter below voiceCutoff
    // of full consciousness (or knocked out, or dead) is cut off unless its voiceCut is false; the dead hear only the
    // dead. Also each player's own: voiceMic (push to talk on voiceKey by default, an open microphone above voiceLevelDb, or off), voiceDevice (a
    // Windows recording device by name; empty: the Windows default, recorded by Steam) and voiceVolume. The multiplayer
    // page and the in-match menu change them and write them back (SaveIniValues).
    bool voice = false;
    voice::Reach voiceReach = voice::Reach::Proximity;   // when voice is on
    bool voiceCut = true;        // false: keep talking when knocked out or low on consciousness
    voice::Mic voiceMic = voice::Mic::PushToTalk;
    std::string voiceDevice;
    float voiceLevelDb = -40.f;
    int voiceKey = 'V';          // a virtual-key code
    float voiceNearM = 5.f, voiceFarM = 30.f;
    float voiceCutoff = 0.3f;
    float voiceVolume = 1.f;
    ChatKey chatKey = ChatKey::Enter;
    // true: a stand-in's hands lock their grip joints and carry the weapon (the game's own firm grip); false: the grips
    // are freed and each weapon is servoed on its own (pre7 behaviour). Each machine's own setting applies to its stand-ins.
    bool standInGripLock = true;   // opens the chat (enter, t, y or u); the game's Talk is very likely T
    // true: replay a stand-in's two-handed weapon and arm targets together when both hands can follow the pose.
    // 0 restores the earlier two-handed playback. Receiver only; detailed measurements still require helpImprove.
    bool standInTwoHandPose = true;
    // true: while that playback owns a stand-in's two grip joints, both lock (the hands stay on the shaft). false keeps
    // the game's sliding, turning off-hand hold under it, as before. Receiver only.
    bool standInTwoHandLock = true;
    // true: the host starts a match even while another player runs a different mod build (testing only).
    bool allowBuildMismatch = false;
    // In-game bug reports ("/bug" in the chat; net/report_client.h). Nothing is sent without the player's command.
    // true: other fighters' names float over their heads in a match (name_tags=0 turns them off).
    bool nameTags = true;
    // Classes lobbies. classTags: a second name tag line with the fighter's class and stats (class_tags=0 hides it). classLook: each
    // fighter's muscles show their class's Strength (class_look=0 leaves the game's own look).
    bool classTags = true;
    bool classLook = true;
    // Build cards (match/hsm_build_card.h). buildCards: show other players' cards (build_cards=0 hides them; yours is still
    // published). buildName: the build named by a launcher with -hsmbuild=, shown on your card; empty: none.
    bool buildCards = true;
    std::string className;    // class=: the class picked in the mods menu; empty or Fair: the game's own fighter
    bool classWolverine = true;   // class_wolverine=0: don't write a class's parts into Half Wolverine's settings at start
    std::string classCustom[kCustomClassSlots];   // class_custom1=..: the Forge's slots (match/hsm_class.h)
    bool wolverineRefresh = true;   // wolverine_refresh=: when a class changes Half Wolverine's settings, reload UE4SS's mods in place. 0: restart instead
    bool blockHse = true;     // block_hse=: in lobbies you host, HS Enhancer is refused. Guests go by the host
    bool modBlockAll = true;  // mod_block=: in lobbies you host. all: unsupported mods are refused everywhere; classes: only with classes
    std::string classCustomName[kCustomClassSlots];   // class_custom1_name=..: what the player called it; checked wherever it's read
    int classBiteKey = 'B';   // class_bite_key=: bites, for a class with Bite. A letter or F1 to F12
    bool clawMirror = true;   // claw_mirror=0: don't draw Half Wolverine's claws on other players' fighters, or tell them about yours
    int buildCardKey = 'K';   // build_card_key=: brings cards back, holds them, steps through them, sends them away. One letter, or F1 to F12.
    std::string buildName;
    // Capture the flag: true (ctf_carry=hand, the default) the carrier's own machine puts the flag in a free hand; false (ctf_carry=back) it always hangs on the back.
    bool ctfFlagInHand = true;
    // true: armour swapped out while paint tasks still read it is destroyed later (the T5 crash); paint_defer=0 turns it off.
    bool paintDefer = true;
    // The Juggernaut role's body scale (juggernaut_scale, 1.0..1.6, default 1.35), written as the passport Height the game
    // maps to it (match/hsm_roles.h HeightForScale). Every machine needs the same value: each builds its own copy.
    double juggernautScale = 1.35;
    bool reports = true;
    // true: your HSMMod log is also sent to the report relay when someone in your lobby files a report or a crash is reported
    // (the multiplayer page's HELP IMPROVE THE MOD toggle; SaveHelpImprove writes the key). Off by default.
    bool helpImprove = false;
    // help_improve_asked=1: the one-time HELP IMPROVE THE MOD question (the first MULTIPLAYER click, 0.8.0-pre7) was
    // answered, or the toggle was used. It never turns help_improve on by itself.
    bool helpImproveAsked = false;
    std::string reportUrl = "https://reports.dudiebug.net";
    // One request to api.github.com at startup, asking whether a newer HSMMod release exists (net/update_check.h).
    bool updateCheck = true;
    // role=server only (see session/hsm_autoflow.h): a public Steam lobby.
    std::string serverFormat = "ffa";          // ffa | duel | lms | teams | teams3 | teams4 | survival | zombies
    int serverTeams = 2;                       // server_teams: the teams of server_format=teams, 2..4 (teams3 / teams4 say it in the word)
    std::string serverMaps = "all";            // comma list of map display names, or all
    int serverScoreLimit = 0;                  // 0: the format's default
    int serverTimeLimitS = 0;                  // 0: none
    int serverDamagePct = -1;                  // player damage for everyone, percent of the rate; -1: the stock rate
    int serverCountdownS = 30;                 // lobby countdown once serverMinPlayers have joined
    int serverMinPlayers = 2;
    // Moderation (session/hsm_moderation.h): on role=server, Steam IDs that may /kick, /ban and /unban from the chat (the
    // console, HSMMod.cmd, always may). serverVoteKick (votekick= or server_votekick=): whether players can vote a
    // player out; since 0.7.0-pre6 in every hosted session, from the chat or the in-match menu.
    std::string serverAdmins;
    bool serverVoteKick = true;
    // [mod] section (ue4ss_load.h): HSMMod's dwmapi.dll chain-loads UE4SS. ue4ss=0 skips it; ue4ss_order=before loads it
    // before HSMMod's ProcessEvent hook is installed, after (default) once the hook is in.
    bool ue4ss = true;
    bool ue4ssBefore = false;
    std::string ue4ssKeep;   // ue4ss_keep: the hash of a UE4SS.dll the player chose to keep over the bundled build
    // [mod] record=1: the recorder (gameplay/recorder.h) writes each fight level to HSMRec-<date>-<time>.hsmrec. Dev only.
    bool record = false;
    // [mod] pack=<name>: the modpack in Paks/~mods/<name>/ (HSMPack.ini) a host announces in its lobby; off: none; empty: the only
    // valid one installed (gameplay/mod_pack.h, docs/MODPACK.md).
    std::string modPack;
    // [mod] mod rules (gameplay/mod_rules.h, session/hsm_mod_rules.h). modRules=0 (mod_rules=0): no mod report, no lobby
    // rule, no host check, no HSMMod build gate. modChallenge=0: the host sends no code challenges. The host's rule:
    // mod_rule= vanilla | maps (default) | bundle | custom | anything | classes, mod_cheats=block (default) | allow,
    // mod_official=1 (official builds only), mod_allow= the Custom list ("A, B*1a2b3c4d, +Required"; empty: what the
    // host runs).
    bool modRules = true;
    bool modChallenge = true;
    std::string modRule = "maps";
    bool modBlockCheats = true;
    bool modOfficialOnly = false;
    std::string modAllow;
};

Config LoadConfig();          // writes a commented default file if none exists
// Re-reads only the lobby mod rule keys ([mod] mod_rule, mod_cheats, mod_official, mod_allow) into c: the host may edit
// them while the game runs (gameplay/mod_rules.cpp).
void ReadModRuleKeys(Config& c);
const char* RoleName(Role r);
bool SaveHelpImprove(bool on);   // writes help_improve= (and help_improve_asked=1) under [net] in HSMMod.ini, the rest untouched; false: could not
// Writes these keys under [section] (default [net]) in HSMMod.ini in one replace, the rest of the file untouched; false: could not.
bool SaveIniValues(const std::vector<std::pair<std::string, std::string>>& keys, const char* section = "net");
}  // namespace hsm
