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
  it("never relays race data over the control socket",async()=>{
    const {h,cs,code}=await setup(); await start(h,cs,code);
    const late=await Client.join(code,"Late");
    cs[1].ws.send(new Uint8Array([5,0,99]).buffer);
    cs[1].sendJson({type:"ping",t:1}); await cs[1].json("pong");
    await expect(h.next(f=>typeof f!=="string",50)).rejects.toThrow("timed out");
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
  it("after a restart, a player back with their token gets their member again; the rest leave",async()=>{
    const {h,cs,code}=await setup(4);
    cs[1].sendJson({type:"ready",ready:true}); await snap(h,r=>r.members[1].ready);
    // a restart: every socket gone, no close events
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)), async (room:any,ctx) => {
      for (const ws of ctx.getWebSockets()) ws.serializeAttachment({...ws.deserializeAttachment(), memberId:null, gone:true});
      room.sockets.clear();
    });
    const back=await Client.connect(`/v1/rooms/${code}`); back.hello("Host",{features:["session","countdown"],rejoinToken:h.welcome.rejoinToken});
    expect(await back.json("welcome")).toMatchObject({memberId:0,host:true});
    const p1=await Client.connect(`/v1/rooms/${code}`); p1.hello("P1",{features:["session","countdown"],rejoinToken:cs[1].welcome.rejoinToken});
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
    expect(bumped.members.map((m:any)=>m.ready)).toEqual([true,false]);  // a new track keeps Ready (finding 128)
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
  // Finding 166: the game's online list as a host sends it (online_menu.c tracks, limits from
  // func_00182290), with Salt City MX (17, six riders) and Salt City TD (18, eight); MOSH pick-only.
  const gameTracks=[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,22,23,24,25];
  const gameLimits=gameTracks.map(t=>[0,1,2,3,5,11,12,19,20].includes(t)?10:[4,10,13,16,18,22,24].includes(t)?8:6);
  it("takes the game's list with both Salt City events and their rider limits",async()=>{
    const {h,cs,code}=await setup(8);
    h.sendJson({type:"settings",track:17,then:"next",rotation:gameTracks,trackLimits:gameLimits,pickOnly:[23]});
    await snap(h,r=>r.rotation.length===25&&r.settings.track===17);
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)),async(room:any)=>{
      expect([room.trackLimit(room.state,17),room.trackLimit(room.state,18)]).toEqual([6,8]);
      expect(room.fittingRotation(room.state,6).slice(0,2)).toEqual([18,19]);
      expect(room.fittingRotation(room.state,8)).toEqual([18,19,20,22,24,0,1,2,3,4,5,10,11,12,13,16]);
    });
    // eight ready on Salt City MX (six places): Salt City TD at grid lock, then NEXT goes on to BC
    await start(h,cs,code);
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)),async(room:any)=>{
      expect([room.state.race.track,room.state.race.slots.length,room.state.lastEvent]).toEqual([18,8,"track_switch:17:6:18"]);
    });
    for(let i=0;i<8;i++)cs[i].sendJson({type:"finish",ticks:600+i,place:i+1});
    expect((await snap(h,r=>r.lastRace)).settings.track).toBe(19);
  });
  it("races six on Salt City MX without a switch",async()=>{
    const {h,cs,code}=await setup(6);
    h.sendJson({type:"settings",track:17,rotation:gameTracks,trackLimits:gameLimits,pickOnly:[23]});
    await snap(h,r=>r.rotation.length===25&&r.settings.track===17);
    await start(h,cs,code);
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)),async(room:any)=>{
      expect([room.state.race.track,room.state.race.slots.length]).toEqual([17,6]);
    });
  });
  // Finding 128: Ready means "count me in for the next race".
  const readies=(r:any)=>r.members.map((m:any)=>m.ready);
  const allReady=async(h:Client,cs:Client[])=>{for(const c of cs)c.sendJson({type:"ready",ready:true}); return snap(h,r=>r.members.every((m:any)=>m.ready));};
  it("keeps Ready across track, NEXT/RANDOM picks and computer riders; cheats and choosing clear it",async()=>{
    const {h,cs}=await setup(3);
    h.sendJson({type:"settings",nextRace:"host"}); await snap(h,r=>r.rules.nextRace==="host");
    cs[1].sendJson({type:"ready",ready:true}); cs[2].sendJson({type:"ready",ready:true});
    await snap(h,r=>r.members[1].ready&&r.members[2].ready);
    h.sendJson({type:"settings",track:5});
    expect(readies(await snap(h,r=>r.settings.track===5))).toEqual([false,true,true]);
    h.sendJson({type:"settings",laps:1});  // computer riders off
    expect(readies(await snap(h,r=>r.settings.laps===1))).toEqual([false,true,true]);
    h.sendJson({type:"settings",laps:0});  // and on again
    expect(readies(await snap(h,r=>r.settings.laps===0))).toEqual([false,true,true]);
    h.sendJson({type:"settings",laps:2});  // cheats on
    expect(readies(await snap(h,r=>r.settings.laps===2))).toEqual([false,false,false]);
    cs[1].sendJson({type:"ready",ready:true}); cs[2].sendJson({type:"ready",ready:true});
    await snap(h,r=>r.members[1].ready&&r.members[2].ready);
    h.sendJson({type:"settings",laps:3});  // computer riders off, cheats still on
    expect(readies(await snap(h,r=>r.settings.laps===3))).toEqual([false,true,true]);
    h.sendJson({type:"settings",laps:1});  // cheats off
    expect(readies(await snap(h,r=>r.settings.laps===1))).toEqual([false,false,false]);
    cs[1].sendJson({type:"ready",ready:true}); await snap(h,r=>r.members[1].ready);
    cs[1].sendJson({type:"pick",choosing:true});
    expect(readies(await snap(h,r=>r.members[1].choosing))).toEqual([false,false,false]);
  });
  it("race end, retiring to the room and the host's END spend Ready",async()=>{
    const {h,cs,code}=await setup(3);
    h.sendJson({type:"settings",nextRace:"host"}); await snap(h,r=>r.rules.nextRace==="host");
    await start(h,cs,code);
    cs[1].sendJson({type:"back"});  // retires to the room
    expect(readies(await snap(h,r=>!r.members[1].inRace))).toEqual([true,false,true]);
    h.sendJson({type:"finish",ticks:600,place:1}); cs[2].sendJson({type:"finish",ticks:700,place:2});
    expect(readies(await snap(h,r=>!!r.lastRace))).toEqual([false,false,false]);  // the race ended for those who raced
    await start(h,cs,code);
    h.sendJson({type:"end"});
    expect((await h.json("ended")).reason).toBe("void");
    expect(readies(await snap(h,r=>r.phase==="open"))).toEqual([false,false,false]);
    const late=await Client.join(code,"Late");  // joining: not ready
    expect((await snap(late,r=>r.members.length===4)).members[3].ready).toBe(false);
  });
  it("a track change during the countdown keeps the grid and runs the countdown again",async()=>{
    const {h,cs,code}=await setup(3);
    h.sendJson({type:"settings",rotation:[0,9,12],trackLimits:[10,6,10]}); await snap(h,r=>r.rotation.length===3);
    expect((await allReady(h,cs)).phase).toBe("starting");
    await edit(code,st=>st.countdownEndsAt=Date.now()+60_000);  // held: the test's countdown is 300 ms
    h.sendJson({type:"settings",track:12});
    const r=await snap(h,r=>r.settings.track===12);
    expect(r.phase).toBe("starting");
    expect(readies(r)).toEqual([true,true,true]);
    expect(r.countdownEndsAt-r.serverTime).toBeGreaterThan(200);  // a full countdown from now again
    expect(r.countdownEndsAt-r.serverTime).toBeLessThanOrEqual(300);
    await due(code,"countdownEndsAt");
    expect((await h.json("start")).track).toBe(12);
  });
  it("a too-small track chosen during the countdown switches to the next fitting one",async()=>{
    const {h,cs,code}=await setup(8);
    h.sendJson({type:"settings",track:0,rotation:[0,9,12],trackLimits:[10,6,10]}); await snap(h,r=>r.rotation.length===3);
    expect((await allReady(h,cs)).phase).toBe("starting");
    await edit(code,st=>st.countdownEndsAt=Date.now()+60_000);
    h.sendJson({type:"settings",track:9});
    const r=await snap(h,r=>r.lastEvent==="track_switch:9:6:12");
    expect([r.settings.track,r.phase,readies(r).every(Boolean)]).toEqual([12,"starting",true]);
  });
  for(const then of ["next","random"]) it(then+" never chooses a pick-only track (MOSH), which keeps its limit",async()=>{
    const {h,cs,code}=await setup(2);
    h.sendJson({type:"settings",track:0,then,nextRace:"host",rotation:[0,23,2],trackLimits:[10,6,10],pickOnly:[23]});
    await snap(h,r=>r.rules.then===then);
    for(let race=0;race<4;race++){
      await start(h,cs,code);
      for(const c of cs)c.sendJson({type:"finish",ticks:600,place:1});
      const r=await snap(h,r=>r.lastRace?.id===race+1);
      expect(r.settings.track).not.toBe(23);
    }
    h.sendJson({type:"settings",track:23}); await snap(h,r=>r.settings.track===23);
    await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)),async(room:any)=>{
      expect(room.trackLimit(room.state,23)).toBe(6);
      expect(room.fittingRotation(room.state,2)).toEqual([2,0]);
    });
  });
  it("pickOnly must name rotation tracks and comes with a rotation",async()=>{
    const {h,code}=await setup(1);
    for(const m of [{pickOnly:[23]},{rotation:[0,2],trackLimits:[10,10],pickOnly:[23]},{rotation:[0,2],trackLimits:[10,10],pickOnly:"23"}]) {
      h.sendJson({type:"settings",...m});
      expect((await h.json("error")).code).toBe("bad_value");
    }
    h.sendJson({type:"settings",rotation:[0,23],trackLimits:[10,6],pickOnly:[23]});
    await snap(h,r=>r.rotation.length===2);
    h.sendJson({type:"settings",rotation:[0,23],trackLimits:[10,6]});  // a 0.7.6 client: no pickOnly, MOSH in the rotation
    await snap(h,r=>r.rotation.length===2);
    const st:any=await runInDurableObject(env.ROOM.get(env.ROOM.idFromName(code)),async(room:any)=>room.state);
    expect(st.pickOnly).toEqual([]);
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
    // names of the same size, each its own (a silent member's name again is its rejoin, finding 137)
    for(let i=1;i<10;i++)cs.push(await Client.join(code,"\u0800".repeat(23)+String.fromCharCode(0x0800+i)));
    await snap(h,r=>r.members.length===10);
    // every member with a region and the room with a colo and hint (finding 135): still within 4 KB
    await edit(code, st=>{st.members[0].name="\u0800".repeat(24);st.version="v".repeat(64);
      for (const m of st.members) m.region="ASIA"; st.colo="FRA"; st.origin={hint:"apac-ne",createdIn:"ASIA",createdFrom:"WAW"};});
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
