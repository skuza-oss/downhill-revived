// The page's side of a P2P room (findings 180, 186): WebRTC belongs to Window, so the page holds the
// room connection and the star's peer connections, routed by the native games' own Rust star
// (dhd_star, as wasm), and bridges them to the game worker (net.js). The room is a server room: its
// messages go to the game as they come; the routes (`p2p`) and the star's signaling (`p2p_signal`)
// stay here. The hub links to every member, a member to the hub. The browser has no SFU client
// yet: a page that can't reach the hub doesn't race (the room takes it out at its setup deadline),
// and a page hub hands the hub to a native game when racers fall back (online/server room.ts).
import initStar, { P2pStar } from '/build/star/dhd_star.js';

export async function createP2pBridge(worker) {
  await initStar();
  // epoch: the hub's (the routes'): signaling for another is dropped, held signals with it.
  let ws=null, star=null, generation=0, epoch=0;
  const peers=new Map(), pending=new Map(), t0=performance.now();
  const now=()=>performance.now()-t0;
  const notify=(event,data={})=>worker.postMessage({type:'net',event,...data});
  const service=m=>{if(ws?.readyState===1)ws.send(typeof m==='string'?m:JSON.stringify(m));};
  function close() {
    generation++; for(const p of peers.values())p.pc.close();peers.clear();pending.clear();
    ws?.close();ws=null;star?.free();star=null;epoch=0;
  }
  function drop(id) {
    const p=peers.get(id);if(!p)return;
    peers.delete(id);p.pc.close();
  }
  function fail(id) {
    if(!peers.has(id))return;
    drop(id);star?.link(id,false,now());drain();
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
    if(!star)return;
    for(const a of JSON.parse(star.drain())) {
      if(a.kind==='connect')connect(a.member,a.offerer).catch(e=>{console.error('P2P: no connection',e);fail(a.member);});
      else if(a.kind==='close')drop(a.member);
      else if(a.kind==='peer'){const p=peers.get(a.to);if(p)queue(p,a.reliable,new Uint8Array(a.bytes));}
      else if(a.kind==='game')notify('message',{data:new Uint8Array(a.bytes)});
      else if(a.kind==='room')service(a.message);
      else if(a.kind==='log')console.log(a.line);
      // 'sfu': no SFU client in the page (above).
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
        if(m.type==='offer'){await p.pc.setLocalDescription(await p.pc.createAnswer());service({type:'p2p_signal',to:id,epoch,signal:{type:'answer',sdp:p.pc.localDescription.sdp}});}
      }
    } catch(e){console.error('P2P signaling failed',e);fail(id);}
  }
  // Direct links only (STUN for the address the internet sees); no relay between games.
  async function connect(id,offerer) {
    drop(id);
    const pc=new RTCPeerConnection({iceServers:[{urls:'stun:stun.cloudflare.com:3478'}]});
    const p={id,pc,event:pc.createDataChannel('events',{negotiated:true,id:1,ordered:true}),
      state:pc.createDataChannel('state',{negotiated:true,id:0,ordered:false,maxRetransmits:0}),
      events:[],states:new Map(),candidates:[],up:false,created:now()};
    peers.set(id,p);
    for(const [dc,reliable]of [[p.event,true],[p.state,false]]) {
      dc.binaryType='arraybuffer';
      dc.onopen=()=>{if(!p.up&&p.event.readyState==='open'&&p.state.readyState==='open'){p.up=true;star?.link(id,true,now());drain();}};
      dc.onmessage=e=>{const bytes=new Uint8Array(e.data);if(bytes.length<=16384){star?.peer_message(id,reliable,bytes,now());drain();}};
      dc.onclose=()=>{if(peers.get(id)===p)fail(id);};
    }
    pc.onicecandidate=e=>{if(peers.get(id)===p&&e.candidate?.candidate&&!/ typ relay/.test(e.candidate.candidate))service({type:'p2p_signal',to:id,epoch,signal:{type:'candidate',candidate:e.candidate.candidate}});};
    pc.onconnectionstatechange=()=>{if(peers.get(id)===p&&['failed','closed'].includes(pc.connectionState))fail(id);};
    const held=pending.get(id)||[];pending.delete(id);
    if(offerer){await pc.setLocalDescription(await pc.createOffer());service({type:'p2p_signal',to:id,epoch,signal:{type:'offer',sdp:pc.localDescription.sdp}});}
    for(const m of held)await signal(id,m);
  }
  setInterval(()=>{
    star?.tick(now());drain();
    for(const p of peers.values())if(!p.up&&now()-p.created>20000)fail(p.id);
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
        if(data.type==='welcome'&&data.transport==='p2p'&&!star)star=new P2pStar(true);
        if(data.type==='p2p'){if(data.epoch!==epoch){epoch=data.epoch;pending.clear();}star?.room(e.data);drain();return;}
        if(data.type==='p2p_signal'){if(data.epoch===epoch)await signal(data.from,data.signal);return;}
        if(data.type==='welcome'){star?.room(e.data);drain();}
        notify('message',{data:e.data});
      };
    }else if(m.op==='close')close();
    else if(m.op==='send') {
      if(typeof m.data==='string')service(m.data);
      else {
        const bytes=new Uint8Array(m.data);
        // The clock's PING goes to the room; race frames to the star.
        if(bytes[0]===3){if(ws?.readyState===1)ws.send(bytes);}else{star?.game_frame(bytes);drain();}
      }
    }
    return true;
  };
}
