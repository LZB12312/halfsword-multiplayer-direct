# Half Sword Multiplayer â€” Direct IP fork

This fork replaces the mod's Steamworks networking and lobby backend with ENet
over UDP. It provides a host-owned lobby, player names from `HSMMod.ini`, chat,
ready/team/settings synchronization and a direct address field in the game menu.
UPnP is optional; manual UDP forwarding also works. No Steam SDK, Steamworks
interfaces, Steam lobby, Steam account lookup or Steam relay is used by the mod.
The game's own distribution, launcher and supported executable requirements are unchanged.

Based on Dudiebug/halfsword-multiplayer-mod commit
`cb8c6646f361f1e1ccb81980108874bdf6fead27` (0.8.0-pre27).
Fork version: **0.2.2**. Everyone must use this same fork and build.
The upstream Steam mod cannot connect to it.

## Install

1. Close the game and keep a backup of your existing `dwmapi.dll`.
2. Copy this package's `dwmapi.dll` into
   `<Half Sword>/HalfswordUE5/Binaries/Win64/`, beside the game executable.
3. Start the game. `HSMMod.ini` is created beside the DLL. Existing INIs work;
   `role=steam` is accepted as an alias for `role=direct`.
4. Use MULTIPLAYER on the main menu. No additional ENet or UPnP DLLs are needed.

The supported game executable is the same as upstream: Steam build 24185754
(UE 5.4.4 CL-2705). Other executable builds are still rejected by the mod's
existing executable fingerprint checks. This change does not change game compatibility.
UE4SS coexistence follows the original proxy arrangement: HSMMod's DLL loads
`ue4ss/UE4SS.dll`. See `README.upstream.md` for the underlying gameplay features.

## Host

Change your display name with **SAVE NAME** in the multiplayer menu. It is saved
for future sessions and updates other players' lobby and scoreboard displays.
Host settings remain in `HSMMod.ini`:

```ini
[net]
role=direct
name=Host
port=27015
upnp=1
reports=off
update_check=1
public_ip_lookup=1
```

Click **HOST DIRECT**. Allow the game through Windows Firewall for UDP on the
chosen port. The mod does not change your Windows Firewall rules.

With `upnp=1`, the host discovers a UPnP IGD router on a background thread and
requests external **UDP 27015 â†’ this PC's UDP 27015** (or your chosen port).
Check `HSMMod.log` for `upnp:` lines: they report success/failure, internal
address and the gateway's external IPv4. Discovery does not prove Internet
reachability. Existing router mappings are preserved. Leases are renewed every
20 minutes; mappings created by this instance are removed on a clean leave.
A router requiring permanent mappings is supported. A crash may leave a
permanent mapping behind; remove that rule in the router if it is no longer needed.

For manual forwarding, set `upnp=0` and create a router rule:

| Router setting | Value |
| --- | --- |
| Protocol | UDP |
| External port | 27015 |
| Internal destination | Host PC's LAN IPv4, e.g. 192.168.1.50 |
| Internal port | 27015 |

Reserve that LAN address in your router's DHCP settings. Only the host needs
forwarding. Share the host's public IPv4 and port with Internet players.
For LAN players, share its LAN IPv4 and port instead.

The lobby shows **HOST IP** with **COPY IP** and **COPY LAN** buttons. The join
address field has a **PASTE ADDRESS** button and supports ordinary text pasting.
Public IPv4 discovery uses HTTPS to api.ipify.org on a background thread when
hosting. Set `public_ip_lookup=0` to disable that request; LAN copying still works.
The displayed public IP does not verify port forwarding or bypass CGNAT.

## Lobby equipment and map changes

Click **EDIT LOADOUT** in a connected lobby to open the game's full chest
equipment editor directly. No Free Mode navigation or map travel is required. The main menu is hidden
while editing and the preview camera is explicitly selected.
**BACK TO LOBBY** closes the preview and sends the updated loadout while keeping
everyone connected. Editing clears your ready state; mark ready again afterwards.
The normal session save guard keeps these edits separate from career saves.

During a round, the host can press Escape and select **RETURN TO LOBBY / CHANGE MAP**.
All players return together with lobby membership retained. Choose another map
in the lobby and start the next match. Clients cannot force a host map change.

## Join

Open MULTIPLAYER, enter **host IPv4 or hostname:port** in the address field, then
click **JOIN ADDRESS**. Example: `192.168.1.50:27015` for a LAN host.
The address is saved to `connect=` in your INI. A missing port uses the local
`port=` value (27015 by default). IPv4/DNS names are supported; IPv6 is not.
The configured-host panel is an address shortcut, not an Internet lobby browser;
compatibility, capacity and password checks happen when connecting.

The host can set a lobby password using the existing menu controls. A joining
player is prompted when the host requires one. Direct transport is not encrypted;
passwords should be unique to the game session. Local voice uses Windows
microphone capture and the mod's ADPCM codec; no Steam voice service is used.

## Servers and moderation

`role=server` still runs a normal supported game instance with automatic matches;
it now listens on `port=`. It does not register with a public master server.
Use the existing local `HSMMod.cmd` console file to administer it.
`server_admins=` is disabled because session IDs are not authenticated accounts.
Host kick/ban and vote controls remain, with bans keyed by the connection's
source IPv4. An IP ban affects other players behind the same NAT. Player IDs
are assigned by the host for this session; reconnecting creates a new ID.

## Changes and limits

- All clients communicate through the host using reliable and unreliable ENet channels.
- Steam invites, avatars, public discovery, rich presence, peer mesh, detours and
  automatic host migration are disabled. The session ends if the host disconnects.
- `mesh`, `detour`, `peer_ping` and `host_migration` are forced off in direct mode.
- Automatic updates check only `LZB12312/halfsword-multiplayer-direct` releases. New DLLs are downloaded in the background, verified against the SHA-256 release asset and GitHub digest, checked as a valid x64 proxy DLL, and installed for the next restart. Set `update_check=0` to disable. Updates never hot-reload into a match. The initial direct1 build needs this release installed manually once.
- Bug reporting is off for fresh configurations. Existing INIs keep their report
  preference; optional reports still use upstream's endpoint when enabled.
- CGNAT, double NAT and ISPs blocking inbound UDP can prevent Internet hosting.
  UPnP only configures the reachable router. For double NAT, forward at each router;
  with CGNAT, request a public IPv4 from the ISP or use a virtual LAN connection.
- LAN joining may work even if the same router cannot loop back through its public IP.
- This is a development build. The DLL was compiled and the backend tested in
  separate local processes; actual combat, menu layout, microphone audio and an
  Internet match across real routers still require in-game testing.

## Build and test

Windows: unpack an LLVM-MinGW UCRT toolchain, then:

```powershell
$env:LLVM_MINGW = 'C:\tools\llvm-mingw'
cd implementation/client/HSMMod
./build.ps1
python tests/run_direct_network.py
```

Output: `implementation/client/HSMMod/build/dwmapi.dll`.
Default builds compile every source with `-Wall -Wextra -Werror` for C++.
`-Incremental` only checks source timestamps, so use a full build after header
or compiler-option changes. Linux cross builds retain `build.sh` and require
LLVM-MinGW with the existing `LLVM_MINGW` variable.

ENet 1.3.18 and MiniUPnPc 2.3.3 are vendored and statically linked.
Licenses and upstream attribution are in `LICENSE`, `THIRD_PARTY_NOTICES.md`
and each `third_party` directory.

`tests/direct_protocol.cpp` checks truncation, malformed input, sender binding
and protected host state. `tests/upnp_mapping.cpp` uses a fake gateway to check
mapping ownership, failure handling and cleanup without changing a real router.
`tests/run_direct_network.py` builds/runs actual UDP host/client processes with
UPnP disabled; it checks snapshots, membership, chat, readiness, 16 KiB reliable
packets, unreliable traffic and capacity rejection.

## 0.1.0 dismemberment recovery

Owner-replayed sever operations now publish a reliable confirmation containing the
accepted cut geometry after the delayed game function completes. Pelvis cuts stay
eligible for native display recovery for three seconds; limbs retain the existing
300 ms window because their hide-bone fallback can take over. New owner-side
logging distinguishes an accepted cut from a missing sever-state bit.
These changes address recovery gaps found during the reported torso-cut desync;
matching torso geometry on the client and host still needs a two-player retest.
The observed host-side cut is confirmed by its log; the client log was unavailable.
