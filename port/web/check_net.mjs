import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createNet } from './www/net.js';
import { layout } from './www/net_layout.js';
test('no online in the browser: not offered, and nothing connects (finding 143)',()=>{
  const bytes=new Uint8Array(4096),span=(a,n)=>bytes.subarray(a,a+n);
  const str=a=>new TextDecoder().decode(bytes.subarray(a,bytes.indexOf(0,a)));
  let made=0;
  globalThis.WebSocket=class {constructor(){made++;}};
  const api=createNet({span,str});
  assert.equal(api.host_service_net_offered(),0);
  assert.equal(api.host_service_net_create_room(),-1);
  assert.equal(api.host_service_net_join_room(0),-1);
  assert.equal(api.host_service_net_list_lobbies_start(8),0);
  assert.equal(made,0);
});
test('browser session messages and shared snapshots',()=>{
  const memory=new ArrayBuffer(32768),bytes=new Uint8Array(memory),dv=new DataView(memory);
  const span=(a,n)=>bytes.subarray(a,a+n);
  const put=(a,s)=>{bytes.set(new TextEncoder().encode(s+'\0'),a);return a;};
  const str=a=>new TextDecoder().decode(bytes.subarray(a,bytes.indexOf(0,a)));
  let socket;
  globalThis.WebSocket=class {
    readyState=1;sent=[];
    constructor(url){this.url=url;socket=this;}
    send(m){this.sent.push(typeof m==='string'?JSON.parse(m):m);}
    close(){}
  };
  const api=createNet({span,str,online:true});
  api.host_service_net_init(0,put(1,'Test'),put(20,'0.7.4'),put(40,'a'.repeat(64)));
  api.host_service_net_create_room();socket.onopen();
  assert.deepEqual(socket.sent[0].features,['countdown','session']);
  const recv=m=>socket.onmessage({data:JSON.stringify(m)});
  recv({type:'welcome',memberId:7,code:'TEST22',serverTime:1000});
  recv({type:'room',phase:'intermission',hostId:7,nextRaceAt:31000,rules:{finishWindow:60,nextRace:'auto',then:'next'},
    members:[{id:7,name:'Test',ready:true,inRace:false,finishTicks:360,place:4}],
    lastRace:{id:2,track:3,entrants:[{id:7,name:'Test',ticks:360,place:4}]}});
  api.host_service_net_get_lobby(1024);
  assert.equal(str(1024+layout.NetLobby.phase[0]),'intermission');
  assert.equal(dv.getFloat64(1024+layout.NetLobby.next_race_at[0],true),31000);
  assert.equal(dv.getInt32(1024+layout.NetLobby.state[0],true),2);
  assert.equal(dv.getBigInt64(1024+layout.NetLobby.member[0]+layout.NetMember.finish_ticks[0],true),360n);
  // where the room runs and the members' regions (finding 135)
  recv({type:'where',region:'EU',colo:'FRA',members:[{id:7,region:'EU'}]});
  api.host_service_net_get_lobby(1024);
  assert.equal(str(1024+layout.NetLobby.room_region[0]),'EU');
  assert.equal(str(1024+layout.NetLobby.room_colo[0]),'FRA');
  assert.equal(str(1024+layout.NetLobby.member[0]+layout.NetMember.region[0]),'EU');
  api.host_service_net_finish(600,4);assert.deepEqual(socket.sent.at(-1),{type:'finish',ticks:600,place:4});
  api.host_service_net_back();assert.deepEqual(socket.sent.at(-1),{type:'back'});
  api.host_service_net_rules(60,1,2);assert.equal(socket.sent.at(-1).then,'random');
  bytes.set([1,3,5],500);api.host_service_net_rotation(500,3);assert.deepEqual(socket.sent.at(-1).rotation,[1,3,5]);
  api.host_service_net_end_race();assert.equal(socket.sent.at(-1).type,'end');
  api.host_service_net_leave();
});
