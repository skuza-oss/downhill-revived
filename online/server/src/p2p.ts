// P2P control plane only: membership, bounded host-star signaling, discovery and TURN credentials.
// Race rules and results belong to the player host. No gameplay binary frame is forwarded here.
import { DurableObject } from "cloudflare:workers";
import { Turn } from "./sfu";
import { PROTOCOL, minVersionOf, isOutdated } from "./protocol";
import type { LobbyEntry } from "./lobbies";
import { findBan, installIdOf, banMessage } from "./guard";

interface SocketInfo {
  id: number | null; name: string; opened: number; heard: number;
  window: number; messages: number; turnUser?: string;
}
interface Meta {
  code: string; hostId: number; nextId: number; version: string; iso: string;
  closed: boolean; listing: LobbyEntry | null;
}

export class P2pRoom extends DurableObject<Env> {
  private meta: Meta | null = null;
  constructor(ctx: DurableObjectState, env: Env) {
    super(ctx, env);
    ctx.blockConcurrencyWhile(async () => { this.meta = await ctx.storage.get<Meta>("meta") ?? null; });
  }
  async claim(code: string): Promise<boolean> {
    if (this.meta) return false;
    this.meta = { code, hostId: 1, nextId: 1, version: "", iso: "", closed: false, listing: null };
    await this.save(); return true;
  }
  private save(): Promise<void> { return this.ctx.storage.put("meta", this.meta); }
  private index() { return this.env.LOBBIES.get(this.env.LOBBIES.idFromName("p2p")); }
  private members(): [WebSocket, SocketInfo][] {
    return this.ctx.getWebSockets().map(ws => [ws, ws.deserializeAttachment() as SocketInfo] as [WebSocket, SocketInfo])
      .filter(([,i]) => i?.id != null);
  }
  private send(ws: WebSocket, m: unknown): void { try { ws.send(JSON.stringify(m)); } catch {} }
  private refuse(ws: WebSocket, code: string, message: string): void {
    this.send(ws,{type:"error",code,message}); try { ws.close(4000,message.slice(0,100)); } catch {}
  }
  private broadcastMembers(): void {
    const members=this.members().map(([,i])=>({id:i.id,name:i.name}));
    for (const [ws] of this.members()) this.send(ws,{type:"p2p_members",members});
  }
  async fetch(request: Request): Promise<Response> {
    if (!this.meta || this.meta.closed) return new Response("Room not found",{status:404});
    if (request.headers.get("Upgrade")?.toLowerCase() !== "websocket") return new Response("Expected WebSocket",{status:426});
    if (this.ctx.getWebSockets().length >= 12) return new Response("Room full",{status:429});
    const pair=new WebSocketPair(), now=Date.now();
    const info: SocketInfo={id:null,name:"",opened:now,heard:now,window:now,messages:0};
    this.ctx.acceptWebSocket(pair[1]); pair[1].serializeAttachment(info);
    await this.ctx.storage.setAlarm(now+5000);
    return new Response(null,{status:101,webSocket:pair[0]});
  }
  async webSocketMessage(ws: WebSocket, data: string | ArrayBuffer): Promise<void> {
    const meta=this.meta, info=ws.deserializeAttachment() as SocketInfo;
    if (!meta || meta.closed || !info) return;
    const now=Date.now(); info.heard=now;
    if (now-info.window>=10000) {info.window=now;info.messages=0;}
    if (++info.messages>300) return this.refuse(ws,"too_fast","Too many signaling messages.");
    ws.serializeAttachment(info);
    if (typeof data!=="string") {
      if (info.id===null || data.byteLength!==10 || new Uint8Array(data)[0]!==3) return;
      const out=new Uint8Array(18);out.set(new Uint8Array(data));out[0]=4;
      new DataView(out.buffer).setFloat64(10,now,true);ws.send(out);return;
    }
    if (data.length>24000) return this.refuse(ws,"bad_message","Message too large.");
    let m: Record<string,unknown>;
    try { m=JSON.parse(data); if (!m || typeof m!=="object" || Array.isArray(m)) throw new Error(); }
    catch { return this.refuse(ws,"bad_message","Invalid message."); }
    if (m.type==="ping") {this.send(ws,{type:"pong",t:m.t,serverTime:now});return;}
    if (m.type==="hello") {
      if (info.id!==null) return this.refuse(ws,"bad_message","Already joined.");
      const ban=await findBan(this.env,{install:installIdOf(m.installId)});
      if (ban) return this.refuse(ws,"banned",banMessage(ban));
      if ((ws.deserializeAttachment() as SocketInfo).id!==null || meta.closed) return;
      if (m.protocol!==PROTOCOL) return this.refuse(ws,"protocol_mismatch","Incompatible P2P protocol.");
      if (typeof m.name!=="string" || !m.name.trim() || m.name.length>24 || typeof m.version!=="string" || m.version.length>100 || typeof m.isoSha256!=="string" || !/^[a-f0-9]{64}$/.test(m.isoSha256))
        return this.refuse(ws,"bad_message","Invalid player details.");
      if (isOutdated(typeof m.clientVersion==="string"?m.clientVersion:"0.0.0",minVersionOf(this.env.MIN_VERSION)))
        return this.refuse(ws,"client_outdated","Update the game before joining.");
      if (meta.version && (meta.version!==m.version || meta.iso!==m.isoSha256))
        return this.refuse(ws,"version_mismatch","The host has a different game build or disc.");
      if (this.members().length >= (meta.listing?.maxPlayers??10)) return this.refuse(ws,"room_full","This room is full.");
      info.id=meta.nextId++;info.name=m.name.trim();
      if (!meta.version) {meta.version=m.version;meta.iso=m.isoSha256;meta.hostId=info.id;}
      ws.serializeAttachment(info);await this.save();
      this.send(ws,{type:"p2p_welcome",code:meta.code,memberId:info.id,hostId:meta.hostId,version:meta.version,
        turnPending:!!(this.env.TURN_KEY_ID && this.env.TURN_KEY_API_TOKEN),
        serverTime:Date.now(),members:this.members().map(([,i])=>({id:i.id,name:i.name}))});
      this.broadcastMembers();
      if (this.env.TURN_KEY_ID && this.env.TURN_KEY_API_TOKEN) {
        try {
          const credentials=await new Turn(this.env).mint(`p2p-${meta.code}-${info.id}`);
          if (meta.closed || !this.members().some(([s])=>s===ws)) {await new Turn(this.env).revoke(credentials.username);return;}
          const current=ws.deserializeAttachment() as SocketInfo;
          current.turnUser=credentials.username;ws.serializeAttachment(current);
          this.send(ws,{...credentials.message,type:"p2p_turn"});
        } catch {this.send(ws,{type:"error",code:"turn_unavailable",message:"Relay setup failed. Direct connections may still work."});}
      }
      return;
    }
    if (info.id===null) return this.refuse(ws,"bad_message","Send hello first.");
    if (m.type==="leave") {await this.remove(ws);try {ws.close(1000,"left");} catch {}return;}
    if (m.type==="p2p_signal") {
      if (!Number.isSafeInteger(m.to) || !m.signal || typeof m.signal!=="object") return;
      if (info.id!==meta.hostId && m.to!==meta.hostId) return this.refuse(ws,"bad_message","Only host-star links are allowed.");
      const target=this.members().find(([,i])=>i.id===m.to);
      if (target && target[1].id!==info.id) this.send(target[0],{type:"p2p_signal",from:info.id,signal:m.signal});
      return;
    }
    if (m.type==="p2p_kick" && info.id===meta.hostId && m.member!==meta.hostId) {
      const target=this.members().find(([,i])=>i.id===m.member);
      if (target) {this.refuse(target[0],"kicked","The host removed you from the room.");await this.remove(target[0]);}return;
    }
    if (m.type==="p2p_listing" && info.id===meta.hostId) {
      const max=Number(m.maxPlayers), track=Number(m.track);
      if (!Number.isInteger(max)||max<2||max>10||!Number.isInteger(track)||track<0||track>255) return;
      const entry: LobbyEntry={code:meta.code,roomName:String(m.roomName??"P2P race").slice(0,96),version:meta.version,
        track,players:this.members().length,maxPlayers:max,hostName:info.name,phase:["open","starting","racing","intermission"].includes(String(m.phase))?String(m.phase):"open"};
      for (const k of ["raceStartedAt","finishEndsAt","nextRaceAt"] as const) if (typeof m[k]==="number" && Number.isFinite(m[k])) entry[k]=m[k];
      meta.listing=entry;await this.save();
      if (m.public===true) await this.index().upsert(entry);else await this.index().remove(meta.code);
      return;
    }
    // No race commands or gameplay frames are relayed by the lobby.
  }
  private async remove(ws: WebSocket): Promise<void> {
    const meta=this.meta, info=ws.deserializeAttachment() as SocketInfo;
    if (!meta || !info || info.id===null) return;
    const id=info.id;info.id=null;ws.serializeAttachment(info);
    if (info.turnUser) this.ctx.waitUntil(new Turn(this.env).revoke(info.turnUser).catch(()=>{}));
    if (id===meta.hostId) {
      meta.closed=true;await this.save();await this.index().remove(meta.code);
      for (const [other] of this.members()) {this.refuse(other,"host_left","The host disconnected.");}
    } else {this.broadcastMembers();}
  }
  async webSocketClose(ws: WebSocket): Promise<void> {await this.remove(ws);}
  async webSocketError(ws: WebSocket): Promise<void> {await this.remove(ws);}
  async alarm(): Promise<void> {
    if (!this.meta || this.meta.closed) return;
    const now=Date.now();
    for (const ws of this.ctx.getWebSockets()) {
      const info=ws.deserializeAttachment() as SocketInfo;
      if (info && (info.id===null ? now-info.opened>10000 : now-info.heard>20000)) {
        await this.remove(ws);try {ws.close(1001,"timed out");} catch {}
      }
    }
    if (!this.meta.closed && this.ctx.getWebSockets().length) await this.ctx.storage.setAlarm(now+5000);
  }
}
