const encoder = new TextEncoder();
const ipv4 = /^(?:0|[1-9][0-9]{0,2})(?:\.(?:0|[1-9][0-9]{0,2})){3}$/;
export function relayConfig(env) {
 const ip = env.TURN_RELAY_IP ?? '';
 const port = env.TURN_PORT ?? '3478';
 const secret = env.TURN_REST_SECRET ?? '';
 if (typeof ip !== 'string' || typeof port !== 'string' || typeof secret !== 'string' ||
     !ipv4.test(ip) || ip.split('.').some(n => Number(n) > 255) ||
     !/^[1-9][0-9]{0,4}$/.test(port) || Number(port) > 65535 || secret.length < 32)
  return null;
 return {ip, port, secret};
}

export async function relayWelcome(config, id, expires) {
 const username = `${Math.floor(expires / 1000)}:${crypto.randomUUID()}`;
 const key = await crypto.subtle.importKey('raw', encoder.encode(config.secret),
  {name:'HMAC', hash:'SHA-1'}, false, ['sign']);
 const signed = new Uint8Array(await crypto.subtle.sign('HMAC', key, encoder.encode(username)));
 return {kind:'welcome', id, mode:'relay-v1', relay_ip:config.ip, turn_port:config.port,
         turn_user:username, turn_password:btoa(String.fromCharCode(...signed))};
}

export function validSignal(message, isHost, relayIp) {
 const payload = message?.payload;
 if (typeof message?.to !== 'string' || !(isHost ? /^[a-f0-9-]{36}$/.test(message?.to ?? '') : message?.to === 'host') ||
     payload?.kind !== 'description' || typeof payload.sdp !== 'string' ||
     payload.sdp.length > 4096 || typeof payload.key !== 'string' ||
     !/^[A-Za-z0-9+/]{96}$/.test(payload.key)) return false;
 const lines = payload.sdp.split(/\r?\n/);
 if (lines.pop() !== '') return false;
 const types = new Set();
 let candidates = 0;
 for (const line of lines) {
  let type;
  if (/^a=ice-ufrag:[A-Za-z0-9+/]{4}$/.test(line)) type = 'user';
  else if (/^a=ice-pwd:[A-Za-z0-9+/]{22}$/.test(line)) type = 'password';
  else if (line === 'a=end-of-candidates') type = 'finished';
  else if (line === 'a=ice-options:ice2') type = 'options';
  else {
   const candidate = /^a=candidate:[0-9]{1,32} 1 UDP ([0-9]{1,10}) ([0-9.]+) ([1-9][0-9]{0,4}) typ relay raddr 0\.0\.0\.0 rport 0$/.exec(line);
   if (!candidate || Number(candidate[1]) > 4294967295 || candidate[2] !== relayIp || Number(candidate[3]) > 65535 || ++candidates > 8)
    return false;
   continue;
  }
  if (types.has(type)) return false;
  types.add(type);
 }
 return types.size === 4 && candidates > 0;
}
