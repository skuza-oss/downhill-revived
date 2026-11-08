// Real browser WebRTC + shared WASM session, against local_server.py (no game data).
// python port/tools/local_server.py -- node port/web/check_p2p.mjs
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import path from 'node:path';
import { startChrome, connect } from './cdp.mjs';

const base=process.env.DHD_NET_SERVER;
if(!base)throw Error('Run through local_server.py');
const port=18327;
const server=spawn(process.execPath,['port/web/serve.mjs','--port',String(port)],{stdio:['ignore','pipe','inherit']});
let browser,cdp;
try {
  await once(server.stdout,'data');
  const started=await startChrome({chrome:process.env.CHROME||'C:/Program Files/Google/Chrome/Application/chrome.exe',
    profile:path.resolve(`build/web/p2p-profile-${process.pid}`),fresh:true});
  browser=started.browser;cdp=await connect(started.wsUrl);
  async function page() {
    const {result:{targetId}}=await cdp.send('Target.createTarget',{url:`http://127.0.0.1:${port}/net_layout.js`});
    const {result:{sessionId}}=await cdp.send('Target.attachToTarget',{targetId,flatten:true});
    await cdp.send('Runtime.enable',{},sessionId);
    const evaluate=async expression=>{
      const reply=await cdp.send('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true},sessionId);
      if(reply.result?.exceptionDetails)throw Error(JSON.stringify(reply.result.exceptionDetails));
      return reply.result?.result?.value;
    };
    await new Promise(r=>setTimeout(r,300));
    await evaluate(`(async()=>{
      globalThis.messages=[];globalThis.frames=[];
      const {createP2pBridge}=await import('/p2p.js');
      globalThis.handle=await createP2pBridge({postMessage(m){
        if(m.event==='open')handle({type:'net',op:'send',data:JSON.stringify({type:'hello',protocol:4,version:'browser-p2p-test',clientVersion:'0.7.13',isoSha256:'a'.repeat(64),name:'Browser'})});
        if(m.event==='message'){if(typeof m.data==='string')messages.push(JSON.parse(m.data));else frames.push(Array.from(m.data));}
      }});
      globalThis.command=m=>handle({type:'net',op:'send',data:JSON.stringify(m)});
    })()`);
    return evaluate;
  }
  async function wait(f) {
    const until=Date.now()+30000;
    while(Date.now()<until){if(await f())return;await new Promise(r=>setTimeout(r,50));}
    throw Error('Browser P2P test timed out');
  }
  const host=await page();
  await host(`handle({type:'net',op:'connect',url:${JSON.stringify(base+'/v1/p2p/rooms/new')}})`);
  await wait(()=>host(`messages.some(m=>m.type==='welcome')`));
  const code=await host(`messages.find(m=>m.type==='welcome').code`);
  const guest=await page();
  await guest(`handle({type:'net',op:'connect',url:${JSON.stringify(base+'/v1/p2p/rooms/'+code)}})`);
  await wait(()=>guest(`messages.some(m=>m.type==='room'&&m.members.length===2)`));
  for(const p of [host,guest])await p(`command({type:'ready',ready:true})`);
  await wait(async()=>await host(`messages.some(m=>m.type==='start')`)&&await guest(`messages.some(m=>m.type==='start')`));
  await wait(async()=>{
    await guest(`handle({type:'net',op:'send',data:new Uint8Array([5,0,6,...new Array(21).fill(0)])})`);
    return host(`frames.some(f=>f[0]===5&&f[1]===1)`);
  });
  for(const [i,p]of [host,guest].entries())await p(`command({type:'finish',ticks:${100+i},place:${i+1}})`);
  await wait(async()=>await host(`messages.some(m=>m.type==='ended')`)&&await guest(`messages.some(m=>m.type==='ended')`));
  console.log('P2P BROWSER MATCH: real WebRTC, shared WASM host, snapshots and results');
} finally {
  cdp?.ws.close();browser?.kill();server.kill();
}
