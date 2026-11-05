// HSMMod.ini next to the game exe. Missing file or role=off keeps the game fully vanilla.
#pragma once
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
    // true (default from 0.8.0-pre24): the Fenced-ring scenario keeps its BP_Fence_Bags: the level manager's own
    // level-load reset of "Current Lose Condition" is followed by the mod setting it back to 3 before the level script
    // destroys the bags (#706). fence_fix=0: as 0.8.0-pre23 (the fence never survives a level load). Per machine:
    // every machine's level manager wipes its own game instance, so every player in the fight needs it on.
    bool fenceFix = true;
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
    // 0: money never moves. Read on the host only; the SaveGuard still restores the saved career when the session ends.
    bool progressionPayout = true;
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
    // true: the host starts a match even while another player runs a different mod build (testing only).
    bool allowBuildMismatch = false;
    // In-game bug reports ("/bug" in the chat; net/report_client.h). Nothing is sent without the player's command.
    // true: other fighters' names float over their heads in a match (name_tags=0 turns them off).
    bool nameTags = true;
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
    // [mod] record=1: the recorder (gameplay/recorder.h) writes each fight level to HSMRec-<date>-<time>.hsmrec. Dev only.
    bool record = false;
    // [mod] pack=<name>: the modpack in Paks/~mods/<name>/ (HSMPack.ini) a host announces in its lobby; off: none; empty: the only
    // valid one installed (gameplay/mod_pack.h, docs/MODPACK.md).
    std::string modPack;
};

Config LoadConfig();          // writes a commented default file if none exists
const char* RoleName(Role r);
bool SaveHelpImprove(bool on);   // writes help_improve= (and help_improve_asked=1) under [net] in HSMMod.ini, the rest untouched; false: could not
// Writes these keys under [net] in HSMMod.ini in one replace, the rest of the file untouched; false: could not.
bool SaveIniValues(const std::vector<std::pair<std::string, std::string>>& keys);
}  // namespace hsm
