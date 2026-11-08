# Relay room service

This service handles admission and ICE signalling for the DLL's relay rooms. It
is disabled by default and uses a separate Worker name and separate invitation
list. The build and tests require no hosted TURN account.

## Operator setup

Use a coturn server you control on an existing internet-reachable IPv4 host.
Relay-only rooms require a running TURN server reachable over UDP. An existing
host with enough bandwidth and a routable address can avoid a new service bill.
The repo does not provision a server or a managed relay account.

- Configure coturn for TURN REST shared-secret authentication (`use-auth-secret`).
  Generate a strong shared secret of at least 32 characters. Store it in coturn's
  protected config as `static-auth-secret` and in the Worker as the
  `TURN_REST_SECRET` secret. Keep the same relay IPv4 on the listener and
  advertised allocation addresses.
- Allow the chosen UDP TURN listener port and a restricted allocation range.
  Configure per-user and global allocation quotas plus per-session and
  aggregate bandwidth caps in coturn. The host can need up to seven allocations
  per room. Restrict permitted TURN peers to the relay IPv4, with a server-side
  deny rule for all other peer IPs. Verify that two allocations on the same
  relay can reach each other and that an allocation cannot relay to an unrelated
  address. Only the coturn operator can enforce these data-plane limits.
- In the Worker set `TURN_RELAY_IP` to the relay's advertised IPv4,
  `TURN_PORT` to the UDP listener port, and `ENABLED=1`. Set the
  `INVITE_SHA256` secret to comma-separated SHA‑256 hashes of separate random
  bearer invitations. Send each invitation to its tester privately.
- Set the build environment variable `HSM_RELAY_SIGNALLING_HOST` to the
  Worker hostname. GitHub Actions can read a repository variable of that name.
  A test install may instead set `relay_signalling_host` in `[net]` of
  `HSMMod.ini`. The client requires HTTPS with a valid certificate.

The Worker issues a unique TURN credential only after invitation and room
admission. It expires with the room. The Worker limits admissions and candidate
messages. To stop the relay immediately, stop coturn or block its UDP listener
and allocation ports. Disabling the Worker blocks new rooms and credentials.

## Tests

Run `node --test test/*.test.mjs` in this directory for credential and
signalling-policy tests. On Windows, build the DLL and run
`implementation/client/HSMMod/tests/run_direct_network.py`. The test starts a
temporary TURN relay on loopback and exercises the DLL's native relay bridge
with real encrypted datagrams. It also exercises the existing direct transport.

For an internet acceptance test, use two different networks. Confirm that both
clients connect through the relay, inspect the signalled candidate addresses, and
stop coturn before another join to confirm failure. The TURN operator can see
client addresses as part of routing. Direct IP mode shares addresses with peers.
