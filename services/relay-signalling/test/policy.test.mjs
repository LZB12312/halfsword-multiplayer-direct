import assert from 'node:assert/strict';
import {createHmac} from 'node:crypto';
import {test} from 'node:test';
import {relayConfig, relayWelcome, validSignal} from '../policy.mjs';

const ip = '203.0.113.12';
const secret = 'fixture-only-turn-rest-secret-123456';
const sdp = [
 'a=ice-ufrag:Ab+/',
 'a=ice-pwd:abcdefghijklmnopqrstuv',
 `a=candidate:1 1 UDP 99 ${ip} 49152 typ relay raddr 0.0.0.0 rport 0`,
 'a=end-of-candidates',
 'a=ice-options:ice2',
 '',
].join('\r\n');
const message = (description, to = 'host') =>
 ({to, payload:{kind:'description', sdp:description, key:'A'.repeat(96)}});

test('requires an explicit valid relay configuration', () => {
 assert.equal(relayConfig({}), null);
 assert.equal(relayConfig({TURN_RELAY_IP:ip,TURN_REST_SECRET:'short'}), null);
 assert.equal(relayConfig({TURN_RELAY_IP:ip,TURN_PORT:'65536',TURN_REST_SECRET:secret}), null);
 assert.equal(relayConfig({TURN_RELAY_IP:'256.0.1.1',TURN_REST_SECRET:secret}), null);
 assert.deepEqual(relayConfig({TURN_RELAY_IP:ip,TURN_REST_SECRET:secret}), {ip,port:'3478',secret});
});

test('issues unique coturn REST credentials that expire with the room', async () => {
 const config = relayConfig({TURN_RELAY_IP:ip,TURN_REST_SECRET:secret});
 const expires = 1_900_000_123_456;
 const welcome = await relayWelcome(config,'host',expires);
 const next = await relayWelcome(config,'host',expires);
 assert.match(welcome.turn_user,/^1900000123:[a-f0-9-]{36}$/);
 assert.notEqual(welcome.turn_user,next.turn_user);
 assert.equal(welcome.turn_password, createHmac('sha1',secret).update(welcome.turn_user).digest('base64'));
 assert.equal(welcome.mode,'relay-v1');
 assert.equal(welcome.relay_ip,ip);
 assert.equal(welcome.turn_port,'3478');
 assert.equal(JSON.stringify(welcome).includes(secret),false);
});

test('permits only the approved relay address and hides related addresses', () => {
 assert.equal(validSignal(message(sdp),false,ip),true);
 assert.equal(validSignal(message(sdp,'00000000-0000-0000-0000-000000000000'),true,ip),true);
 for (const unsafe of [
  sdp.replace('typ relay','typ host'),
  sdp.replace('typ relay','typ srflx'),
  sdp.replace(ip,'198.51.100.9'),
  sdp.replace('raddr 0.0.0.0','raddr 192.168.0.7'),
  sdp.replace('UDP','TCP'),
  sdp.replace('49152','99999'),
  sdp.replace('a=end-of-candidates\r\n',''),
  sdp.replace(/a=candidate[^\n]*\n/,''),
  sdp + 'a=candidate:2 1 UDP 50 192.168.0.7 9999 typ host\r\n',
 ]) assert.equal(validSignal(message(unsafe),false,ip),false);
 assert.equal(validSignal(message(sdp,'host'),true,ip),false);
 assert.equal(validSignal(message(sdp,'00000000-0000-0000-0000-000000000000'),false,ip),false);
});
