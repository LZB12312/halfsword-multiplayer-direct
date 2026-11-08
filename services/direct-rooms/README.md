# Direct ICE room service

The public DLL uses this HTTPS WebSocket service to exchange room membership,
ICE candidates and ephemeral public keys. Gameplay is encrypted and sent directly
between players over UDP. Participants can see peer IP addresses, so share room
codes privately. Anyone with the room code can attempt to join.

The Worker uses two SQLite-backed Durable Objects on Workers Free. It limits
signalling admissions to 2,000 per UTC day and 30 per IP per hour. Rooms hold up
to eight players and expire after one hour. Both sides use the same DLL build.
The Worker receives the connecting IP and stores only its hash in the hourly
governor. Rejected requests can still count against Cloudflare's account quota.

Run Wrangler locally or deploy to Workers Free. The checked-in configuration
defaults to disabled. Pass `--var ENABLED:1` to explicitly enable the service.
The DLL defaults to the public service. Set the build environment variable
`HSM_ICE_SIGNALLING_HOST` or add `ice_signalling_host` under `[net]` in
`HSMMod.ini` to use a different hostname with a valid TLS certificate.

The public endpoint uses no managed TURN allocations or relay bandwidth.
