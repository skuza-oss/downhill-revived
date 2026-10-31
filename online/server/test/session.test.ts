import { env, runInDurableObject, runDurableObjectAlarm, evictDurableObject } from "cloudflare:test";
import { exports } from "cloudflare:workers";
import { describe, it, expect } from "vitest";
import { Client } from "./helpers";

const snap = async (c: Client, pred: (r: any) => boolean = () => true) =>
  JSON.parse(await c.next(f => typeof f === "string" && JSON.parse(f).type === "room" && pred(JSON.parse(f))) as string);
async function setup(n = 2) {
  const h = await Client.connect("/v1/rooms/new");
  h.hello("Host", {features:["session","countdown"]});
  const {code} = await h.json("welcome");
  const cs = [h];
  for (let i=1;i<n;i++) cs.push(await Client.join(code,"P"+i));
  await snap(h,r=>r.members.length===n);
  return {h,cs,code};
}
async function edit(code: string, fn: (st:any)=>void) {
  await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), async (room:any,ctx) => {
    fn(room.state);
    await ctx.storage.put("room",room.state);
  });
}
async function due(code:string, key:string) {
  await edit(code, st => { if (key==="finishEndsAt") st.race.finishEndsAt=Date.now()-1; else st[key]=Date.now()-1; });
  await runDurableObjectAlarm(env.ROOM.get(env.ROOM.idFromName(code)));
}
async function start(h:Client,cs:Client[],code:string) {
  for (const c of cs) c.sendJson({type:"ready",ready:true});
  h.sendJson({type:"start"});
  await snap(h,r=>r.phase==="starting");
  await due(code,"countdownEndsAt");
  await Promise.all(cs.map(c=>c.json("start")));
}
describe("session rooms",()=>{
  it("relays only between current entrants, excluding waiting and returned players",async()=>{
    const {h,cs,code}=await setup(); await start(h,cs,code);
    const late=await Client.join(code,"Late");
    cs[1].ws.send(new Uint8Array([5,0,99]).buffer);
    expect([...(await h.binary())]).toEqual([5,1,99]);
    late.sendJson({type:"ping",t:1}); await late.json("pong"); late.drain();
    await expect(late.next(f=>typeof f!=="string",50)).rejects.toThrow("timed out");
    h.sendJson({type:"back"}); await snap(h,r=>!r.members[0].inRace);
    cs[1].ws.send(new Uint8Array([5,0,77]).buffer);
    h.sendJson({type:"ping",t:2}); await h.json("pong");
    await expect(h.next(f=>typeof f!=="string",50)).rejects.toThrow("timed out");
  });
  it("one Ready alone does not start; host Start locks rules through countdown",async()=>{
    const {h,cs,code}=await setup(1);
    h.sendJson({type:"ready",ready:true});
    expect((await snap(h,r=>r.members[0].ready)).phase).toBe("open");
    h.sendJson({type:"start"}); await snap(h,r=>r.phase==="starting");
    h.sendJson({type:"settings",finishWindow:30,nextRace:"auto",then:"same"});
    expect((await snap(h,r=>r.rulesPending)).rulesPending).toBe(true);
    await due(code,"countdownEndsAt"); await h.json("start");
    h.sendJson({type:"finish",ticks:600,place:1});
    const r=await snap(h,r=>r.lastRace);
    expect(r.phase).toBe("open");
    expect(r.lastRace.entrants[0].ticks).toBe(600);
    h.sendJson({type:"ready",ready:true}); await snap(h,r=>r.members[0].ready);
    h.sendJson({type:"start"}); await snap(h,r=>r.phase==="starting");
    await due(code,"countdownEndsAt"); await h.json("start");
    h.sendJson({type:"end"});
    expect((await h.json("ended")).reason).toBe("void");
    expect((await snap(h,r=>r.phase==="intermission")).lastRace.id).toBe(1);
  });
  it("everyone ready starts the countdown during an intermission",async()=>{
    const {h,cs,code}=await setup();
    h.sendJson({type:"settings",nextRace:"auto"}); await snap(h,r=>r.rules.nextRace==="auto");
    await start(h,cs,code);
    for (const c of cs) c.sendJson({type:"finish",ticks:600,place:1});
    await snap(h,r=>r.phase==="intermission");
    for (const c of cs) c.sendJson({type:"ready",ready:true});
    const r=await snap(h,r=>r.phase==="starting");
    expect(r.nextRaceAt).toBe(null);
  });
  it("closes the race once nobody is still riding, before the finishers come back",async()=>{
    const {h,cs,code}=await setup(); await start(h,cs,code);
    h.sendJson({type:"finish",ticks:600,place:1});
    await snap(h,r=>r.members[0].raceState==="finished");
    cs[1].sendJson({type:"back"});
    expect((await h.json("ended")).reason).toBe("done");
    const r=await snap(h,r=>r.lastRace);
    expect(r.lastRace.entrants.map((x:any)=>x.dnfReason ?? "finished")).toEqual(["finished","gave_up"]);
  });
  it("keeps a track the host chose during the race over the rotation",async()=>{
    const {h,cs,code}=await setup();
    h.sendJson({type:"settings",rotation:[0,1,2],trackLimits:[10,10,10],then:"next",track:0}); await snap(h,r=>r.rotation.length===3);
    await start(h,cs,code);
    h.sendJson({type:"finish",ticks:600,place:1}); h.sendJson({type:"back"});
    await snap(h,r=>!r.members[0].inRace);
    h.sendJson({type:"settings",track:2}); await snap(h,r=>r.settings.track===2);
    cs[1].sendJson({type:"back"});
    expect((await snap(h,r=>r.lastRace)).settings.track).toBe(2);
  });
  it("bounds escaping for control characters and unpaired surrogates",async()=>{
    const h=await Client.connect("/v1/rooms/new");
    h.hello("\ud800".repeat(24),{features:["session"]});
    await h.json("welcome");
    expect((await snap(h)).members[0].name).toBe("\ufffd".repeat(24));
    h.sendJson({type:"settings",roomName:"\u0001".repeat(32)});
    expect((await snap(h,r=>r.roomName.length===32)).roomName).toBe("\ufffd".repeat(32));
  });
  it("defaults follow public until explicit; validates settings atomically",async()=>{
    const {h}=await setup(1);
    h.sendJson({type:"settings",public:true});
    expect((await snap(h,r=>r.public)).rules).toEqual({finishWindow:60,nextRace:"auto",then:"next"});
    h.sendJson({type:"settings",nextRace:"host",then:"same",finishWindow:0});
    await snap(h,r=>r.rules.finishWindow===0);
    h.sendJson({type:"settings",public:false});
    h.sendJson({type:"settings",public:true});
    expect((await snap(h,r=>r.public)).rules.nextRace).toBe("host");
    h.sendJson({type:"settings",finishWindow:33,track:9});
    expect((await h.json("error")).code).toBe("bad_value");
  });
  it("locks entrants, excludes late Ready, drops unready and cancels empty grids",async()=>{
    const {h,cs,code}=await setup(3);
    h.sendJson({type:"ready",ready:true});
    await snap(h,r=>r.members[0].ready);
    h.sendJson({type:"start"});
    await snap(h,r=>r.phase==="starting");
    cs[1].sendJson({type:"ready",ready:true});
    await snap(h,r=>r.members[1].ready);
    h.sendJson({type:"ready",ready:false});
    expect((await snap(h,r=>r.lastEvent==="start_cancelled")).phase).toBe("open");
    h.sendJson({type:"start"});
    await snap(h,r=>r.phase==="starting");
    await due(code,"countdownEndsAt");
    expect((await cs[1].json("start")).players).toHaveLength(1);
    expect((await snap(h,r=>r.phase==="racing")).members[0].inRace).toBe(false);
  });
  it("accepts mid-race joins; finish/back/gave_up preserve waiting ready and ordered results",async()=>{
    const {h,cs,code}=await setup();
    h.sendJson({type:"settings",rotation:[0,3,5],trackLimits:[10,10,10]});
    await snap(h,r=>r.rotation.length===3);
    await start(h,cs,code);
    const late=await Client.join(code,"Late");
    late.sendJson({type:"ready",ready:true});
    await snap(h,r=>r.members.length===3 && r.members[2].ready);
    h.sendJson({type:"finish",ticks:600,place:4});
    const r=await snap(h,r=>r.finishEndsAt!==null);
    expect(r.finishEndsAt-r.serverTime).toBeGreaterThan(59000);
    h.sendJson({type:"back"});
    expect((await cs[1].json("left")).reason).toBe("finished");
    cs[1].sendJson({type:"back"});
    expect((await h.json("ended")).reason).toBe("done");
    const end=await snap(h,r=>!!r.lastRace);
    expect(end.lastRace.entrants.map((x:any)=>[x.ticks,x.dnfReason,x.place])).toEqual([[600,undefined,4],[undefined,"gave_up",5]]);
    expect(end.settings.track).toBe(3);
    expect(end.members.map((m:any)=>m.ready)).toEqual([false,false,true]);
  });
  it("back in the room, the race's Ready is spent; a Ready pressed during the race lasts past it",async()=>{
    const {h,cs,code}=await setup(); await start(h,cs,code);
    h.sendJson({type:"finish",ticks:600,place:1}); h.sendJson({type:"back"});
    expect((await snap(h,r=>!r.members[0].inRace)).members.map((m:any)=>m.ready)).toEqual([false,true]);
    h.sendJson({type:"ready",ready:true}); await snap(h,r=>r.members[0].ready);
    cs[1].sendJson({type:"back"});
    expect((await snap(h,r=>r.lastRace)).members.map((m:any)=>m.ready)).toEqual([true,false]);
  });
  it("after a restart, a player back under their name gets their member again; the rest leave",async()=>{
    const {h,cs,code}=await setup(4);
    cs[1].sendJson({type:"ready",ready:true}); await snap(h,r=>r.members[1].ready);
    // a restart: every socket gone, no close events
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), async (room:any,ctx) => {
      for (const ws of ctx.getWebSockets()) ws.serializeAttachment({...ws.deserializeAttachment(), memberId:null, gone:true});
      room.sockets.clear();
    });
    const back=await Client.connect(`/v1/rooms/${code}`); back.hello("Host",{features:["session","countdown"]});
    expect(await back.json("welcome")).toMatchObject({memberId:0,host:true});
    const p1=await Client.connect(`/v1/rooms/${code}`); p1.hello("P1",{features:["session","countdown"]});
    expect((await p1.json("welcome")).memberId).toBe(1);
    expect((await snap(back,r=>r.members.length===4)).members[1].ready).toBe(true);
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), async (room:any) => { room.startedAt=Date.now()-60_000; });
    await runDurableObjectAlarm(env.ROOM.get(env.ROOM.idFromName(code)));
    const r=await snap(back,r=>r.members.length===2);
    expect([r.hostId,r.members.map((m:any)=>m.name)]).toEqual([0,["Host","P1"]]);
  });
  it("keeps finish deadline fixed; time_up retires unfinished entrants after hibernation",async()=>{
    const {h,cs,code}=await setup(3); await start(h,cs,code);
    h.sendJson({type:"finish",ticks:900,place:1}); const first=await snap(h,r=>r.finishEndsAt);
    cs[1].sendJson({type:"finish",ticks:800,place:1});
    expect((await snap(h,r=>r.members[1].finishTicks===800)).finishEndsAt).toBe(first.finishEndsAt);
    await evictDurableObject(env.ROOM.get(env.ROOM.idFromName(code)));
    await due(code,"finishEndsAt");
    const r=await snap(h,r=>r.lastRace);
    expect(r.lastRace.entrants.map((x:any)=>x.place)).toEqual([1,2,3]);
    expect(r.lastRace.entrants[2].dnfReason).toBe("time_up");
  });
  it("no-limit never starts finish clock; void retains results and does not rotate",async()=>{
    const {h,cs,code}=await setup();
    h.sendJson({type:"settings",finishWindow:0,rotation:[0,2],trackLimits:[10,10]});
    await snap(h,r=>r.rules.finishWindow===0);
    await start(h,cs,code);
    h.sendJson({type:"finish",ticks:300,place:2});
    expect((await snap(h,r=>r.members[0].raceState==="finished")).finishEndsAt).toBeNull();
    h.sendJson({type:"end"});
    expect((await h.json("ended")).reason).toBe("void");
    expect((await snap(h,r=>r.phase==="open")).lastRace).toBeNull();
  });
  for (const then of ["same","next","random"]) it("rotates "+then+" using running rules; stages pending rules",async()=>{
    const {h,cs,code}=await setup();
    h.sendJson({type:"settings",then,rotation:[0,2,4],trackLimits:[10,10,10]});
    await snap(h,r=>r.rotation.length===3); await start(h,cs,code);
    h.sendJson({type:"back"}); await snap(h,r=>!r.members[0].inRace);
    h.sendJson({type:"settings",then:then==="same"?"next":"same"});
    expect((await snap(h,r=>r.rulesPending)).rulesPending).toBe(true);
    cs[1].sendJson({type:"back"});
    const r=await snap(h,r=>r.lastRace);
    expect(then==="same"?r.settings.track===0:then==="next"?r.settings.track===2:[2,4].includes(r.settings.track)).toBe(true);
    expect(r.rulesPending).toBe(false);
  });
  it("auto timer waits for Ready, then 30 seconds; track change bumps to 15 seconds",async()=>{
    const {h,cs,code}=await setup();
    h.sendJson({type:"settings",public:true}); await snap(h,r=>r.public);
    await start(h,cs,code);
    for(const c of cs)c.sendJson({type:"back"});
    await snap(h,r=>r.phase==="intermission");
    await due(code,"nextRaceAt");
    await snap(h,r=>r.waitingForRider && r.phase==="open");
    h.sendJson({type:"ready",ready:true});
    const r=await snap(h,r=>r.phase==="intermission");
    expect(r.nextRaceAt-r.serverTime).toBeGreaterThan(29000);
    await edit(code,st=>st.nextRaceAt=Date.now()+1000);
    h.sendJson({type:"settings",track:9});
    const bumped=await snap(h,r=>r.settings.track===9);
    expect(bumped.nextRaceAt-bumped.serverTime).toBeGreaterThanOrEqual(14990);
    expect(bumped.members.every((m:any)=>!m.ready)).toBe(true);
    cs[1].sendJson({type:"ready",ready:true}); await snap(h,r=>r.members[1].ready);
    await due(code,"nextRaceAt");
    await snap(h,r=>r.phase==="starting");
  });
  it("migrates host in a race and records disconnect DNF",async()=>{
    const {h,cs,code}=await setup(); await start(h,cs,code);
    h.ws.close(1000);
    const r=await snap(cs[1],r=>r.hostId===1);
    expect(r.phase).toBe("racing");
    cs[1].sendJson({type:"back"});
    expect((await snap(cs[1],r=>r.lastRace)).lastRace.entrants[0].dnfReason).toBe("left");
  });
  it("defaults to ten and accepts exactly 2 through 10",async()=>{
    const {h}=await setup(1);
    h.sendJson({type:"ping",t:0}); await h.json("pong");
    h.sendJson({type:"settings",public:true});
    expect((await snap(h,r=>r.public)).maxPlayers).toBe(10);
    for(let n=2;n<=10;n++) {
      h.sendJson({type:"settings",maxPlayers:n});
      expect((await snap(h,r=>r.maxPlayers===n)).maxPlayers).toBe(n);
    }
    for(const n of [1,11]) {h.sendJson({type:"settings",maxPlayers:n});expect((await h.json("error")).code).toBe("bad_value");}
  });
  it("switches a six-rider selection at grid lock without dropping ready players",async()=>{
    const {h,cs,code}=await setup(10);
    const overflow = await Client.connect(`/v1/rooms/${code}`);
    overflow.hello("Overflow");
    expect(await overflow.closed()).toBe(4004);
    h.sendJson({type:"settings",track:9,rotation:[9,4,12],trackLimits:[6,8,10]});
    await snap(h,r=>r.settings.track===9);
    await start(h,cs,code);
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)),async(room:any)=>{
      expect(room.state.race.track).toBe(12);
      expect(room.state.race.slots.length).toBe(10);
      expect(room.state.lastEvent).toBe("track_switch:9:6:12");
    });
  });
  it("requires matching rotation limits and a ten-rider fallback",async()=>{
    const {h}=await setup(1);
    for(const trackLimits of [[6], [6,8], [6,9,10]]) {
      h.sendJson({type:"settings",rotation:[9,4,12],trackLimits});
      expect((await h.json("error")).code).toBe("bad_value");
    }
  });
  for(const then of ["next","random"]) it(then+" skips tracks smaller than room membership",async()=>{
    const {h,cs,code}=await setup(10);
    h.sendJson({type:"settings",track:0,then,rotation:[0,9,4,12],trackLimits:[10,6,8,10]});
    await snap(h,r=>r.rules.then===then);
    await start(h,cs,code);
    for(let i=0;i<10;i++)cs[i].sendJson({type:"finish",ticks:600+i,place:i+1});
    expect((await snap(h,r=>r.lastRace)).settings.track).toBe(12);
  });
  it("keeps ten results within ten places when the reported places lag",async()=>{
    const {h,cs,code}=await setup(10); await start(h,cs,code);
    for(let i=1;i<10;i++)cs[i].sendJson({type:"finish",ticks:600+i,place:i+1});
    h.sendJson({type:"finish",ticks:900,place:10});
    expect((await snap(h,r=>r.lastRace)).lastRace.entrants.map((r:any)=>r.place)).toEqual([1,2,3,4,5,6,7,8,9,10]);
  });
  it("lists racing and full session rooms; snapshot stays below 4KB for ten members",async()=>{
    const {h,cs,code}=await setup(1);
    h.sendJson({type:"settings",public:true,maxPlayers:10,roomName:"\u0800".repeat(32),rotation:Array.from({length:64},(_,i)=>i),trackLimits:Array(64).fill(10)});
    await snap(h,r=>r.public);
    for(let i=1;i<10;i++)cs.push(await Client.join(code,"\u0800".repeat(24)));
    await snap(h,r=>r.members.length===10);
    await edit(code, st=>{st.members[0].name="\u0800".repeat(24);st.version="v".repeat(64);});
    await start(h,cs,code);
    const list=async()=> (await (await exports.default.fetch(new Request("https://dhd.skuza.app/v1/lobbies"))).json<any>()).lobbies;
    await expect.poll(list).toContainEqual(expect.objectContaining({code,phase:"racing",full:true,players:10}));
    for (let i=0;i<10;i++){cs[i].sendJson({type:"finish",ticks:600+i,place:1});cs[i].sendJson({type:"back"});}
    await snap(h,r=>r.lastRace);
    await start(h,cs,code);
    const r=await snap(h,r=>r.phase==="racing");
    expect(new TextEncoder().encode(JSON.stringify(r)).length).toBeLessThanOrEqual(4096);
    for(let i=0;i<9;i++) {cs[i].sendJson({type:"finish",ticks:0xffffffff,place:255}); await snap(h,r=>r.members[i].finishTicks===0xffffffff);}
    const largest=await snap(cs[9],r=>r.members.filter((m:any)=>m.raceState==="finished").length===9);
    expect(new TextEncoder().encode(JSON.stringify(largest)).length).toBeLessThanOrEqual(4096);
  });
});
