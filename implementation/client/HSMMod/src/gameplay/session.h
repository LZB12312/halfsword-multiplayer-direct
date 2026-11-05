// What the UI needs from the running session.
#pragma once
#include "gameplay/lobby.h"
#include "match/hsm_mp_scene.h"
#include <string>
#include <vector>

namespace hsm {

class Session {
public:
    virtual ~Session() = default;
    virtual game::Lobby& lobby() = 0;
    virtual bool steamMode() const = 0;
    virtual void StartOnMap(const std::string& path) = 0;   // host: open a fight level; the lobby follows
    virtual void StartProgression() = 0;                    // host: the stock Progression hub, Free Mode off
    virtual void ReturnToLobby() = 0;                       // host: end the current round and bring everyone to the lobby
    virtual std::string OpenLoadoutEditor() = 0;            // direct stock equipment editor, without leaving the lobby
    virtual std::string SubmitReport(const std::string& text) = 0;   // a bug report (same checks as /bug); the line to show
    virtual std::string ReportStatus() const = 0;                    // the last report's outcome line, as told to the player
    virtual uint32_t ReportStatusSeq() const = 0;                    // changes with every new outcome (0: none yet)
    virtual void LobbyChatLine(const std::string& line) = 0;         // typed in the lobby chat box: /bug, /sendlog and /nolog work as in the match chat, else it goes to every member
    virtual std::string TakeNotice() = 0;                            // why the host turned us away (kicked, banned, full...), once; empty: nothing new
    virtual bool HelpImprove() const = 0;                            // help_improve in HSMMod.ini: also send the log when someone in the lobby files a report
    virtual void SetHelpImprove(bool on) = 0;                        // also written back to HSMMod.ini (and marks the question answered)
    virtual bool HelpImproveAsked() const = 0;                       // the one-time question on the first MULTIPLAYER click was answered

    // The multiplayer menus (the page's voice block, the in-match Esc menu). Voice settings are written to HSMMod.ini.
    virtual bool InSession() const = 0;                              // joined (a client welcomed) or hosting
    virtual MpVoice VoiceView() = 0;                                 // also keeps the microphone recording for the meter a moment
    virtual void SetVoiceReach(voice::Reach reach) = 0;              // this player's own voice chat (Off / Proximity / Everyone); a role=server host only relays
    virtual void SetVoicePrefs(const MpVoice& v) = 0;                // mic mode, threshold, key, volume, device
    virtual void SetVoiceMonitor(bool on) = 0;                       // HEAR MYSELF
    virtual std::vector<MpPlayer> Players() = 0;                     // the session's players, this machine's view
    virtual void SetPlayerListen(uint8_t slot, bool muted, int volumePct) = 0;   // this player's own mute and volume for one speaker
    virtual void SetLobbyPassword(const std::string& password) = 0;  // host: lock the lobby with it, or open it again with ""; nobody is kicked
    virtual void KickPlayer(uint8_t slot, bool ban) = 0;             // host
    virtual void VoteKickPlayer(uint8_t slot) = 0;                   // starts a vote (the host counts it)
    virtual void VoteBanPlayer(uint8_t slot) = 0;                    // starts a voteban (role=server sessions: 7 days)
    virtual void CastVote(bool yes) = 0;
    virtual MpLoadout LoadoutView() = 0;                             // the Esc menu's CHANGE LOADOUT button: shown, available, why not
    virtual std::string ChangeLoadout() = 0;                         // a click: re-reads this player's passport and asks for the new kit; the line to show
    virtual MpVote Vote() = 0;                                       // the vote running now, if any
    virtual std::vector<std::string> NetworkLines() = 0;             // read-only network facts for the menu
    virtual void LeaveSession() = 0;                                 // out of the session (and the lobby), back to the main menu
};

}  // namespace hsm
