// The page's side of a P2P room (finding 180): WebRTC belongs to Window, so the page holds the
// room connection, the peer connections and the coordinator (the native games' Rust, as wasm) and
// bridges them to the game worker (net.js). Host star: the host links to every guest.
import initSession, { P2pSession } from '/build/session/dhd_session.js';

export async function createP2pBridge(worker) {
  await initSession();
  let ws=null, session=null, welcome=null, turn=null, members=[], generation=0, offset=0;
  const peers=new Map(), pending=new Map();
  const now=()=>Date.now()+offset;
  const notify=(event,data={})=>worker.postMessage({type:'net',event,...data});
  const service=m=>{if(ws?.readyState===1)ws.send(JSON.stringify(m));};
  function close() {
    generation++; for(const p of peers.values())p.pc.close();peers.clear();pending.clear();
    ws?.close();ws=null;session?.free();session=null;welcome=null;turn=null;members=[];
  }
  function fail(id) {
    const p=peers.get(id);if(!p)return;
    peers.delete(id);p.pc.close();session?.link(id,false,now());drain();
  }
  function queue(p,reliable,bytes) {
    if(reliable){if(p.events.length>=256){fail(p.id);return;}p.events.push(bytes);}
    else if(bytes.length>=13)p.states.set(`${bytes[10]}:${bytes[12]}`,bytes);
  }
  function flush(p) {
    if(p.event.readyState==='open')while(p.events.length&&p.event.bufferedAmount<65536)p.event.send(p.events.shift());
    if(p.state.readyState==='open'&&p.state.bufferedAmount<16384){for(const bytes of p.states.values())p.state.send(bytes);p.states.clear();}
  }
  function drain() {
    if(!session)return;
    for(const a of JSON.parse(session.drain())) {
      if(a.kind==='game')notify('message',{data:JSON.stringify(a.message)});
      else if(a.kind==='frame')notify('message',{data:new Uint8Array(a.bytes)});
      else if(a.kind==='server')service(a.message);
      else if(a.kind==='peer'){const p=peers.get(a.to);if(p)queue(p,a.reliable,new Uint8Array(a.bytes));}
    }
    for(const p of peers.values())flush(p);
  }
  async function signal(id,m) {
    const p=peers.get(id);
    if(!p){const held=pending.get(id)||[];if(held.length<32)held.push(m);pending.set(id,held);return;}
    try {
      if(m.type==='candidate') {
        const c={candidate:m.candidate,sdpMid:'0',sdpMLineIndex:0};
        if(p.pc.remoteDescription)await p.pc.addIceCandidate(c);else p.candidates.push(c);
      } else if(m.type==='offer'||m.type==='answer') {
        await p.pc.setRemoteDescription({type:m.type,sdp:m.sdp});
        for(const c of p.candidates.splice(0))await p.pc.addIceCandidate(c);
        if(m.type==='offer'){await p.pc.setLocalDescription(await p.pc.createAnswer());service({type:'p2p_signal',to:id,signal:{type:'answer',sdp:p.pc.localDescription.sdp}});}
      }
    } catch(e){console.error('P2P signaling failed',e);fail(id);}
  }
  async function reconcile() {
    if(!welcome||!session||(welcome.turnPending&&!turn))return;
    const ids=new Set(members.map(m=>m.id));
    for(const [id,p]of peers)if(!ids.has(id)){p.pc.close();peers.delete(id);}
    for(const {id}of members) {
      if(id===welcome.memberId||(welcome.memberId!==welcome.hostId&&id!==welcome.hostId)||peers.has(id))continue;
      const iceServers=[{urls:'stun:stun.cloudflare.com:3478'}];
      if(turn)iceServers.push({urls:turn.urls,username:turn.username,credential:turn.credential});
      const pc=new RTCPeerConnection({iceServers});
      const p={id,pc,event:pc.createDataChannel('events',{negotiated:true,id:1,ordered:true}),
        state:pc.createDataChannel('state',{negotiated:true,id:0,ordered:false,maxRetransmits:0}),
        events:[],states:new Map(),candidates:[],up:false,created:performance.now()};
      peers.set(id,p);
      for(const [dc,reliable]of [[p.event,true],[p.state,false]]) {
        dc.binaryType='arraybuffer';
        dc.onopen=()=>{if(!p.up&&p.event.readyState==='open'&&p.state.readyState==='open'){p.up=true;session?.link(id,true,now());drain();}};
        dc.onmessage=e=>{const bytes=new Uint8Array(e.data);if(bytes.length<=16384){session?.receive(id,reliable,bytes,now());drain();}};
        dc.onclose=()=>fail(id);
      }
      pc.onicecandidate=e=>{if(e.candidate)service({type:'p2p_signal',to:id,signal:{type:'candidate',candidate:e.candidate.candidate}});};
      pc.onconnectionstatechange=()=>{if(['failed','closed'].includes(pc.connectionState))fail(id);};
      const held=pending.get(id)||[];pending.delete(id);
      if(welcome.memberId===welcome.hostId){await pc.setLocalDescription(await pc.createOffer());service({type:'p2p_signal',to:id,signal:{type:'offer',sdp:pc.localDescription.sdp}});}
      for(const m of held)await signal(id,m);
    }
  }
  setInterval(()=>{
    session?.tick(now());drain();
    for(const p of peers.values())if(!p.up&&performance.now()-p.created>20000)fail(p.id);
  },10);
  return async function handle(m) {
    if(m.type!=='net')return false;
    if(m.op==='connect') {
      close();const current=generation;
      ws=new WebSocket(m.url);ws.binaryType='arraybuffer';
      ws.onopen=()=>notify('open');
      ws.onclose=()=>{if(current===generation){close();notify('close');}};
      ws.onerror=()=>notify('error');
      ws.onmessage=async e=>{
        if(current!==generation)return;
        if(typeof e.data!=='string'){notify('message',{data:new Uint8Array(e.data)});return;}
        let data;try{data=JSON.parse(e.data);}catch{return;}
        if(data.type==='p2p_welcome'){
          welcome=data;members=data.members;offset=data.serverTime-Date.now();
          session=new P2pSession(e.data,now());drain();await reconcile();
        }else if(data.type==='p2p_members'){
          members=data.members;session?.members(JSON.stringify(members),now());drain();await reconcile();
        }else if(data.type==='p2p_turn'){
          // The room renews them before they expire: live connections' relays refresh with them.
          turn=data;
          for(const p of peers.values()){
            try{p.pc.setConfiguration({...p.pc.getConfiguration(),iceServers:[{urls:'stun:stun.cloudflare.com:3478'},{urls:turn.urls,username:turn.username,credential:turn.credential}]});}
            catch(e){console.error('P2P: renewed relay credentials not applied',e);}
          }
          await reconcile();
        }
        else if(data.type==='p2p_signal')await signal(data.from,data.signal);
        else {
          if(data.type==='error'&&data.code==='turn_unavailable'){
            if(welcome)welcome.turnPending=false;
            await reconcile();
          }
          notify('message',{data:e.data});
        }
      };
    }else if(m.op==='close')close();
    else if(m.op==='send') {
      if(typeof m.data==='string') {
        let data;try{data=JSON.parse(m.data);}catch{return true;}
        if(['hello','ping','pong','leave'].includes(data.type))service(data);
        else session?.local(m.data,now());
      }else {
        const bytes=new Uint8Array(m.data);
        if(bytes[0]===3){if(ws?.readyState===1)ws.send(bytes);}else session?.frame(bytes);
      }
      drain();
    }
    return true;
  };
}
