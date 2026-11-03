// Browser network host services. Struct offsets come from the shared C header.
import { layout } from './net_layout.js';

// Online play is not offered in the browser (finding 143): the page's CSP blocks the race server's
// socket, so it never worked here. The core asks host_service_net_offered and shows no ONLINE row;
// the connections below refuse whatever asks. The same switch is off on Android (port/net/net.c).
// online: true only in check_net.mjs, which tests the session code for when it comes back.
const ONLINE_OFFERED = false;

export function createNet({span, str, online = ONLINE_OFFERED}) {
  let base = 'wss://dhd.skuza.app', name = '', version = '', iso = '', ws;
  let room = {}, start = {}, member = -1, state = 0, seq = 0, offset = 0, welcome = {}, where = {};
  let frames = [], list = [], listing = false, listError = false, timer, clockKnown = false;
  let error = '', errorCode = '';
  const send = m => { if (ws?.readyState === 1) ws.send(JSON.stringify(m)); };
  const now = () => performance.now();
  const serverMs = () => now() + offset;
  const me = () => room.members?.find(x => x.id === member);
  function connect(path) {
    if (!online) { state = 5; error = 'Online play is not offered in the browser.'; seq++; return -1; }
    ws?.close(); clearInterval(timer); state = 1; room = {}; start = {}; frames = []; welcome = {}; where = {};
    ws = new WebSocket(base + path); ws.binaryType = 'arraybuffer';
    const socket = ws;
    ws.onopen = () => {
      send({type:'hello', protocol:2, version, clientVersion:version.match(/\d+\.\d+\.\d+/)?.[0] || '0.7.4',
        isoSha256:iso, name, features:['countdown','session']});
      timer = setInterval(() => send({type:'ping',t:now()}),1000);
    };
    ws.onmessage = e => {
      if (socket !== ws) return;
      if (typeof e.data !== 'string') { if (state===3) {frames.push(new Uint8Array(e.data)); if(frames.length>128)frames.shift();} return; }
      const m = JSON.parse(e.data);
      if(m.type==='ping') send({type:'pong',s:m.s});
      if(m.type==='pong' && m.t !== undefined) offset=m.serverTime-(now()+m.t)/2;
      if(m.type==='welcome') {member=m.memberId;state=2;room.code=m.code;offset=m.serverTime-now();welcome=m;}
      if(m.type==='where') where=m;  // the room's region and the members' regions (finding 135)
      if(m.type==='room') {room=m;if(!me()?.inRace && m.phase)state=2;}
      if(m.type==='start') {start=m;state=3;frames=[];}
      if(m.type==='left' && start.players?.[m.slot])start.players[m.slot].left=1;
      if(m.type==='ended') {state=2;frames=[];}
      if(m.type==='error') {error=m.message;errorCode=m.code;}
      seq++;
    };
    ws.onclose = () => {if(socket===ws){clearInterval(timer);state=5;seq++;}};
    // A socket refused before it opens (e.g. by the page's CSP) may get no close event in Chrome.
    ws.onerror = () => {if(socket===ws&&state===1){clearInterval(timer);state=5;error=error||'Could not reach the server.';seq++;}};
    return 0;
  }
  function write(type, address, values) {
    const l=layout[type], b=span(address,l.size), d=new DataView(b.buffer,b.byteOffset,b.byteLength);
    b.fill(0);
    for(const [key,value] of Object.entries(values)) {
      const f=l[key]; if(!f)continue;
      const [at,size]=f;
      if(typeof value==='string') b.set(new TextEncoder().encode(value).subarray(0,size-1),at);
      else if(typeof value==='bigint') d.setBigInt64(at,value,true);
      else if(['race_started_at','finish_ends_at','next_race_at','countdown_ends_at','server_time','start_at'].includes(key)) d.setFloat64(at,value||0,true);
      else if(typeof value==='number'||typeof value==='boolean') d.setInt32(at,Number(value),true);
    }
  }
  function lobby(a) {
    const rules=room.rules||{}, members=room.members||[], results=room.lastRace?.entrants||[];
    write('NetLobby',a,{state,seq,code:room.code||'',member_id:member,host_id:room.hostId??-1,is_host:room.hostId===member,
      room_name:room.roomName||'',is_public:!!room.public,max_players:room.maxPlayers||0,started:!!room.started&&(!room.phase||!!me()?.inRace),
      track:room.settings?.track||0,laps:room.settings?.laps||0,seed:BigInt(room.settings?.seed??-1),
      nmembers:members.length,ping_ms:-1,local_slot:start.yourSlot||0,nslots:start.players?.length||0,
      race_track:start.track||0,race_laps:start.laps||0,race_seed:start.seed||0,
      start_at:start.startAt?(start.startAt-offset)/1000:0,offline:state===5,
      error_code:errorCode,error,countdown_ms:room.countdownEndsAt?Math.max(0,room.countdownEndsAt-serverMs()):-1,
      phase:room.phase||'',last_event:room.lastEvent||'',race_started_at:room.raceStartedAt,
      finish_ends_at:room.finishEndsAt,next_race_at:room.nextRaceAt,countdown_ends_at:room.countdownEndsAt,
      server_time:room.serverTime,waiting_for_rider:!!room.waitingForRider,rules_pending:!!room.rulesPending,
      finish_window:rules.finishWindow??60,next_race:rules.nextRace==='host',then:rules.then==='same'?0:rules.then==='random'?2:1,
      rotation_count:room.rotation?.length||0,last_race_id:room.lastRace?.id||0,last_race_track:room.lastRace?.track||0,nresults:results.length,
      edge_colo:welcome.edgeColo||'',country:welcome.country||'',continent:welcome.continent||'',
      room_colo:where.colo??welcome.roomColo??'',room_region:where.region??welcome.roomRegion??''});
    members.slice(0,10).forEach((m,i)=>write('NetMember',a+layout.NetLobby.member[0]+i*layout.NetMember.size,
      {...m,rtt_ms:m.rttMs??-1,in_race:!!m.inRace,race_state:m.raceState||'',dnf_reason:m.dnfReason||'',finish_ticks:BigInt(m.finishTicks??-1),
        region:where.members?.find(x=>x.id===m.id)?.region||''}));
    (start.players||[]).slice(0,10).forEach((m,i)=>write('NetRacer',a+layout.NetLobby.racer[0]+i*layout.NetRacer.size,{...m,member_id:m.memberId}));
    results.slice(0,10).forEach((m,i)=>write('NetResult',a+layout.NetLobby.result[0]+i*layout.NetResult.size,{...m,dnf_reason:m.dnfReason||'',ticks:BigInt(m.ticks??-1)}));
    span(a+layout.NetLobby.rotation[0],64).set((room.rotation||[]).slice(0,64));
  }
  function writeList(a,max) {
    list.slice(0,max).forEach((m,i)=>write('NetLobbyInfo',a+i*layout.NetLobbyInfo.size,
      {...m,room_name:m.roomName,host_name:m.hostName,max_players:m.maxPlayers,race_started_at:m.raceStartedAt,
        finish_ends_at:m.finishEndsAt,next_race_at:m.nextRaceAt}));
    return Math.min(list.length,max);
  }
  const api = {
    host_service_net_init:(a,b,c,d)=>{base=a?str(a):base;name=str(b);version=str(c);iso=str(d);},
    host_service_net_offered:()=>online?1:0,
    host_service_net_create_room:()=>connect('/v1/rooms/new'),
    host_service_net_join_room:a=>connect('/v1/rooms/'+str(a)),
    host_service_net_leave:()=>{send({type:'leave'});ws?.close();ws=null;clearInterval(timer);state=0;},
    host_service_net_get_lobby:lobby,
    host_service_net_pick:(member,rider,bike)=>send({type:'pick',...(member>=0?{member}:{}),...(rider<0?{choosing:true}:{rider,bike,choosing:false})}),
    host_service_net_set_ready:ready=>send({type:'ready',ready:!!ready}),
    host_service_net_start_race:()=>send({type:'start'}),
    host_service_net_end_race:()=>send({type:'end'}),
    host_service_net_finish:(ticks,place)=>send({type:'finish',ticks:ticks>>>0,place}),
    host_service_net_back:()=>{send({type:'back'});state=2;frames=[];const m=me();if(m)m.inRace=false;seq++;},
    host_service_net_rules:(finishWindow,nextRace,then)=>send({type:'settings',finishWindow,nextRace:['auto','host'][nextRace],then:['same','next','random'][then]}),
    host_service_net_rotation:(a,n)=>{if(n>=0&&n<=64)send({type:'settings',rotation:[...span(a,n)].map(x=>x&31),trackLimits:[...span(a,n)].map(x=>10-2*(x>>5&3)),pickOnly:[...span(a,n)].filter(x=>x&128).map(x=>x&31)});},
    host_service_net_set_room:a=>{
      const b=span(a,28),d=new DataView(b.buffer,b.byteOffset,b.byteLength),m={type:'settings'};
      ['track','laps','public','maxPlayers'].forEach((k,i)=>{const v=d.getInt32(i*4,true);if(v>=0)m[k]=k==='public'?!!v:v;});
      const seed=d.getBigInt64(16,true);if(seed>=0)m.seed=Number(seed);
      const p=d.getUint32(24,true);if(p)m.roomName=str(p);send(m);
    },
    host_service_net_racing:()=>state===3?1:0,
    host_service_net_seconds:()=>now()/1000,
    host_service_net_server_ms:serverMs,
    host_service_net_send_frame:(type,a,n)=>{
      if(state!==3||n>3070)return 0;const f=new Uint8Array(n+2);f[0]=type;f.set(span(a,n),2);ws.send(f);return 1;},
    host_service_net_receive_frame:(a,n)=>{const f=frames.shift();if(!f||f.length>n)return 0;span(a,f.length).set(f);return f.length;},
    host_service_net_list_lobbies_start:()=>{
      if(listing||!online)return 0;listing=true;listError=false;
      // the list's "now" sets the clock when no room's pings have, so its times count from the server's (finding 135)
      const sent=now();
      fetch(base.replace(/^ws/,'http')+'/v1/lobbies').then(r=>r.json()).then(r=>{
        const inRoom=state===2||state===3;
        if(r.now>0&&!inRoom){offset=r.now-(sent+now())/2;clockKnown=true;}
        list=r.lobbies.map(x=>({...x,timed:clockKnown||inRoom?1:0}));
      }).catch(()=>listError=true).finally(()=>listing=false);
      return 1;
    },
    host_service_net_list_lobbies_poll:(a,n)=>listing?-2:listError?-1:writeList(a,n),
    host_service_net_list_lobbies:(a,n)=>writeList(a,n),
  };
  return api;
}
