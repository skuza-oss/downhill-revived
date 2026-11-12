import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createNet } from './www/net.js';
import { layout } from './www/net_layout.js';
test('disabled online makes no connections',()=>{
  const bytes=new Uint8Array(4096),span=(a,n)=>bytes.subarray(a,a+n);
  const str=a=>new TextDecoder().decode(bytes.subarray(a,bytes.indexOf(0,a)));
  let made=0;
  globalThis.WebSocket=class {constructor(){made++;}};
  const api=createNet({span,str,online:false});
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
  const api=createNet({span,str,online:true,openSocket:url=>new WebSocket(url)});
  api.host_service_net_init(0,put(1,'Test'),put(20,'0.7.4'),put(40,'a'.repeat(64)));
  assert.equal(api.host_service_net_offered(),2);  // P2P rooms only (findings 180, 186)
  assert.equal(api.host_service_net_create_room(0),-1);
  assert.equal(api.host_service_net_join_room(put(100,'ABCDEF')),-1);  // an SFU room's code
  api.host_service_net_create_room(1);socket.onopen();
  assert.match(socket.url,/\/v1\/rooms\/new\?transport=p2p$/);  // a server room with the P2P transport (finding 186)
  assert.deepEqual(socket.sent[0].features,['countdown','session','race_go','p2p']);
  const recv=m=>socket.onmessage({data:JSON.stringify(m)});
  recv({type:'welcome',memberId:7,code:'TEST222',serverTime:1000,transport:'p2p'});
  recv({type:'room',phase:'intermission',hostId:7,nextRaceAt:31000,rules:{finishWindow:60,nextRace:'auto',then:'next'},
    members:[{id:7,name:'Test',ready:true,inRace:false,finishTicks:360,place:4}],
    lastRace:{id:2,track:3,entrants:[{id:7,name:'Test',ticks:360,place:4}]}});
  api.host_service_net_get_lobby(1024);
  assert.equal(str(1024+layout.NetLobby.phase[0]),'intermission');
  assert.equal(dv.getFloat64(1024+layout.NetLobby.next_race_at[0],true),31000);
  assert.equal(dv.getInt32(1024+layout.NetLobby.state[0],true),2);
  assert.equal(dv.getInt32(1024+layout.NetLobby.is_p2p[0],true),1);
  assert.equal(dv.getBigInt64(1024+layout.NetLobby.member[0]+layout.NetMember.finish_ticks[0],true),360n);
  // where the room runs and the members' regions (finding 135)
  recv({type:'where',region:'EU',colo:'FRA',members:[{id:7,region:'EU'}]});
  api.host_service_net_get_lobby(1024);
  assert.equal(str(1024+layout.NetLobby.room_region[0]),'EU');
  assert.equal(str(1024+layout.NetLobby.room_colo[0]),'FRA');
  assert.equal(str(1024+layout.NetLobby.member[0]+layout.NetMember.region[0]),'EU');
  // The start moment over the room (finding 187): sent to a server that takes it, taken in the race.
  recv({type:'welcome',memberId:7,code:'TEST222',serverTime:1000,transport:'p2p',features:['sfu_silent','race_go']});
  recv({type:'start',yourSlot:0,startAt:5000,players:[{slot:0,memberId:7,name:'Test'},{slot:1,memberId:8,name:'B'}]});
  api.host_service_net_race_go(9000.5);assert.deepEqual(socket.sent.at(-1),{type:'race_go',race:5000,at:9000.5});
  recv({type:'race_go',race:5000,at:9500});api.host_service_net_get_lobby(1024);
  assert.equal(dv.getFloat64(1024+layout.NetLobby.race_go_at[0],true),9500);
  recv({type:'ended',reason:'done'});
  // An ended race's last seconds (finding 216): its frames still go both ways until the game is back.
  const frame=()=>socket.onmessage({data:new Uint8Array([6,1,9]).buffer});
  frame();assert.equal(api.host_service_net_receive_frame(600,64),3);assert.deepEqual([...span(600,3)],[6,1,9]);
  bytes.set([9,9],700);assert.equal(api.host_service_net_send_frame(6,700,2),1);assert.deepEqual([...socket.sent.at(-1)],[6,0,9,9]);
  api.host_service_net_finish(600,4);assert.deepEqual(socket.sent.at(-1),{type:'finish',ticks:600,place:4});
  api.host_service_net_back();assert.deepEqual(socket.sent.at(-1),{type:'back'});
  assert.equal(api.host_service_net_send_frame(6,700,2),0);
  frame();assert.equal(api.host_service_net_receive_frame(600,64),0);
  api.host_service_net_rules(60,1,2);assert.equal(socket.sent.at(-1).then,'random');
  bytes.set([1,3,5],500);api.host_service_net_rotation(500,3);assert.deepEqual(socket.sent.at(-1).rotation,[1,3,5]);
  api.host_service_net_end_race();assert.equal(socket.sent.at(-1).type,'end');
  api.host_service_net_leave();
});
