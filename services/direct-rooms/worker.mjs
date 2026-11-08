import {DurableObject} from 'cloudflare:workers';

const MAX_PLAYERS = 8;
const ROOM_LIFETIME_MS = 60 * 60 * 1000;
const DAILY_LIMIT = 2000;
const IP_HOURLY_LIMIT = 30;
const reject = status => new Response('Unavailable', {status});

async function digest(text) {
 const bytes = await crypto.subtle.digest('SHA-256', new TextEncoder().encode(text));
 return Array.from(new Uint8Array(bytes), b => b.toString(16).padStart(2, '0')).join('');
}

export function validSignal(message, host) {
 const payload = message?.payload;
 return typeof message?.to === 'string' && (host ? /^[a-f0-9-]{36}$/.test(message.to) : message.to === 'host') &&
  payload?.kind === 'description' && typeof payload.sdp === 'string' &&
  payload.sdp.length > 0 && payload.sdp.length <= 4096 && !payload.sdp.includes('\0') &&
  typeof payload.key === 'string' && /^[A-Za-z0-9+/]{96}$/.test(payload.key);
}

export default {
 async fetch(req, env) {
  if (env.ENABLED !== '1') return reject(503);
  const path = new URL(req.url).pathname;
  const role = req.headers.get('X-HSM-Role');
  if (req.method !== 'GET' || !/^\/room\/[a-f0-9]{12}$/.test(path) ||
      req.headers.get('Upgrade')?.toLowerCase() !== 'websocket' ||
      (role !== 'host' && role !== 'join')) return reject(400);

  try {
   const ip = await digest(req.headers.get('CF-Connecting-IP') ?? 'local');
   const gate = env.GATE.get(env.GATE.idFromName('global'));
   const verdict = await gate.fetch('https://gate/admit', {method:'POST', body:ip});
   if (!verdict.ok) return reject(503);
   if (!(await verdict.json())) return reject(429);
   return await env.ROOMS.get(env.ROOMS.idFromName(path)).fetch(req);
  } catch {
   return reject(503);
  }
 }
};

export class Gate extends DurableObject {
 async fetch(req) {
  const ip = await req.text();
  const admitted = await this.ctx.storage.transaction(async tx => {
   const hour = Math.floor(Date.now() / 3_600_000);
   const day = Math.floor(hour / 24);
   let state = await tx.get('budget');
   if (!state || state.day !== day) state = {day, hour, count:0, ips:{}};
   if (state.hour !== hour) { state.hour = hour; state.ips = {}; }
   const fromIp = state.ips[ip] ?? 0;
   if (state.count >= DAILY_LIMIT || fromIp >= IP_HOURLY_LIMIT) return false;
   state.count++;
   state.ips[ip] = fromIp + 1;
   await tx.put('budget', state);
   return true;
  });
  return Response.json(admitted);
 }
}

export class Room extends DurableObject {
 async fetch(req) {
  return this.ctx.blockConcurrencyWhile(() => this.admit(req));
 }

 async admit(req) {
  const sockets = this.ctx.getWebSockets();
  const host = req.headers.get('X-HSM-Role') === 'host';
  const hostSocket = sockets.find(socket => socket.deserializeAttachment()?.id === 'host');
  if (sockets.length >= MAX_PLAYERS || Boolean(hostSocket) === host) return reject(409);

  let expires = await this.ctx.storage.get('expires');
  if (!expires) {
   expires = Date.now() + ROOM_LIFETIME_MS;
   await this.ctx.storage.put('expires', expires);
   await this.ctx.storage.setAlarm(expires);
  }
  if (Date.now() >= expires) return reject(410);

  const pair = new WebSocketPair();
  const socket = pair[1];
  const id = host ? 'host' : crypto.randomUUID();
  this.ctx.acceptWebSocket(socket);
  socket.serializeAttachment({id, messages:0, window:0, count:0, sentTo:[]});
  socket.send(JSON.stringify({kind:'welcome', id}));
  if (hostSocket) hostSocket.send(JSON.stringify({kind:'joined', id}));
  return new Response(null, {status:101, webSocket:pair[0]});
 }

 webSocketMessage(socket, data) {
  const state = socket.deserializeAttachment();
  if (typeof data !== 'string' || data.length > 8192) return socket.close(1008, 'Invalid message');
  const now = Math.floor(Date.now() / 1000);
  if (state.window !== now) { state.window = now; state.count = 0; }
  if (++state.count > 10 || ++state.messages > 512) return socket.close(1008, 'Limit');
  let message;
  try { message = JSON.parse(data); } catch { return socket.close(1008, 'Invalid message'); }
  if (!validSignal(message, state.id === 'host')) return socket.close(1008, 'Invalid signal');
  const target = this.ctx.getWebSockets().find(peer => peer.deserializeAttachment()?.id === message.to);
  if (!target) {
   socket.serializeAttachment(state);
   return;
  }
  if (state.sentTo.includes(message.to)) return socket.close(1008, 'Repeated description');
  state.sentTo.push(message.to);
  socket.serializeAttachment(state);
  const {sdp,key} = message.payload;
  target.send(JSON.stringify({kind:'signal', from:state.id, payload:{kind:'description', sdp, key}}));
 }

 webSocketClose(socket) {
  socket.close(1000, 'Closed');
  const id = socket.deserializeAttachment()?.id;
  for (const peer of this.ctx.getWebSockets()) {
   if (peer === socket || peer.readyState !== WebSocket.OPEN) continue;
   if (id === 'host') peer.close(1000, 'Host left');
   else if (peer.deserializeAttachment()?.id === 'host') peer.send(JSON.stringify({kind:'left', id}));
  }
 }

 webSocketError(socket) { socket.close(1011, 'Connection failed'); }

 async alarm() {
  for (const socket of this.ctx.getWebSockets()) socket.close(1000, 'Room expired');
  await this.ctx.storage.deleteAll();
 }
}
