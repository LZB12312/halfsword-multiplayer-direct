import {DurableObject} from 'cloudflare:workers';
import {relayConfig, relayWelcome, validSignal} from './policy.mjs';
export const digest=async text=>Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',new TextEncoder().encode(text))),b=>b.toString(16).padStart(2,'0')).join('');
const reject=(status)=>new Response('Unavailable',{status});
async function handle(req,env) {
 if(env.ENABLED!=='1'||!relayConfig(env)) return reject(503);
 const path=new URL(req.url).pathname;
 if(req.method!=='GET'||!/^\/room\/[a-f0-9]{12}$/.test(path)||req.headers.get('Upgrade')?.toLowerCase()!=='websocket') return reject(400);
 const auth=req.headers.get('Authorization')??'';
 if(!/^Bearer [a-zA-Z0-9_-]{32,128}$/.test(auth)) return reject(401);
 const tokenHash=await digest(auth.slice(7));
 const allowed=(env.INVITE_SHA256??'').split(',');
 if(!allowed.includes(tokenHash)) return reject(401);
 const role=req.headers.get('X-HSM-Role'); if(!['host','join'].includes(role)) return reject(400);
 // Global governor is reached only by authenticated invitations. Invalid tokens
 // cannot allocate room objects or receive a short-lived TURN credential.
 const ip=await digest(req.headers.get('CF-Connecting-IP')??'local');
 const gate=env.GATE.get(env.GATE.idFromName('global'));
 const verdict=await gate.fetch('https://gate/admit',{method:'POST',body:JSON.stringify([ip,tokenHash])});
 if(verdict.status!==200)return reject(503);
 if(!await verdict.json())return reject(429);
 return await env.ROOMS.get(env.ROOMS.idFromName(path)).fetch(req);
}
export default {async fetch(req,env){try{return await handle(req,env);}catch(e){console.error(e.stack);return reject(500);}}};
export class Gate extends DurableObject {
 async fetch(req){try {const [ip,user]=await req.json();return Response.json(await this.admit(ip,user));}catch(e){console.error("gate",e.stack);return reject(500);}}
 constructor(ctx,env){super(ctx,env);}
 async admit(ip,user) {
  return this.ctx.storage.transaction(async tx=>{
   const now=Date.now(), day=Math.floor(now/86400000), hour=Math.floor(now/3600000);
   let s=await tx.get('budget');
   if(!s||s.day!==day) s={day,count:0,hour,ips:{},users:{}};
   if(s.hour!==hour) {s.hour=hour;s.ips={};s.users={};}
   const perIp=Number(this.env.PER_IP_HOURLY_LIMIT)||60;
   if(!(ip in s.ips)&&Object.keys(s.ips).length>=512)return false;
   if(s.count>=(Number(this.env.DAILY_CONNECTION_LIMIT)||2000)||(s.ips[ip]??0)>=perIp||(s.users[user]??0)>=perIp) return false;
   s.count++;s.ips[ip]=(s.ips[ip]??0)+1;s.users[user]=(s.users[user]??0)+1;
   await tx.put('budget',s); return true;
  });
 }
}
export class Room extends DurableObject {
 constructor(ctx,env){super(ctx,env);}
 async fetch(req) {
  return this.ctx.blockConcurrencyWhile(() => this.admit(req));
 }
 async admit(req) {
  const config=relayConfig(this.env);
  if(!config)return reject(503);
  const sockets=this.ctx.getWebSockets(); const host=req.headers.get('X-HSM-Role')==='host';
  if(sockets.length>=8||(host?sockets.some(s=>s.deserializeAttachment()?.id==='host'):!sockets.some(s=>s.deserializeAttachment()?.id==='host'))) return reject(409);
  let expires=await this.ctx.storage.get('expires');
  if(!expires) {expires=Date.now()+Math.min(Number(this.env.ROOM_LIFETIME_SECONDS)||3600,7200)*1000;await this.ctx.storage.put('expires',expires);await this.ctx.storage.setAlarm(expires);}
  if(Date.now()>=expires)return reject(410);
  const id=host?'host':crypto.randomUUID();
  const welcome=await relayWelcome(config,id,expires);
  const pair=new WebSocketPair(), client=pair[0], socket=pair[1];
  this.ctx.acceptWebSocket(socket);
  socket.serializeAttachment({id,messages:0,window:0,count:0});
  socket.send(JSON.stringify(welcome));
  if(!host) for(const peer of sockets) if(peer.deserializeAttachment()?.id==='host') peer.send(JSON.stringify({kind:'joined',id}));
  return new Response(null,{status:101,webSocket:client});
 }
 async webSocketMessage(socket,data) {
  const state=socket.deserializeAttachment(), now=Math.floor(Date.now()/1000);
  if(typeof data!=='string'||data.length>8192) return socket.close(1008,'Invalid message');
  if(state.window!==now){state.window=now;state.count=0;}
  if(++state.count>10 || ++state.messages>(Number(this.env.MAX_MESSAGES)||512)) return socket.close(1008,'Limit');
  let m;try{m=JSON.parse(data);}catch{return socket.close(1008,'Invalid message');}
  if(!validSignal(m,state.id==='host',this.env.TURN_RELAY_IP))return socket.close(1008,'Relay-only description required');
  state.sentTo??=[];
  if(state.sentTo.includes(m.to))return socket.close(1008,'Repeated description');
  state.sentTo.push(m.to);
  socket.serializeAttachment(state);
  for(const peer of this.ctx.getWebSockets()) if(peer.deserializeAttachment()?.id===m.to) peer.send(JSON.stringify({kind:'signal',from:state.id,payload:{kind:'description',sdp:m.payload.sdp,key:m.payload.key}}));
 }
 async webSocketClose(socket) {
  socket.close(1000,'Closed');
  const id=socket.deserializeAttachment()?.id;
  for(const peer of this.ctx.getWebSockets()) if(peer!==socket) {
   if(id==='host')peer.close(1000,'Host left'); else if(peer.deserializeAttachment()?.id==='host')peer.send(JSON.stringify({kind:'left',id}));
  }
 }
 async webSocketError(socket) {socket.close(1011,'Connection failed');}
 async alarm(){for(const s of this.ctx.getWebSockets())s.close(1000,'Room expired');await this.ctx.storage.deleteAll();}
}
