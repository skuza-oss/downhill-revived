import { describe,it,expect } from "vitest";
import { Client } from "./helpers";

async function room() {
  const host=await Client.connect("/v1/p2p/rooms/new");host.hello("Host");
  const welcome=await host.json("p2p_welcome");
  return {host,welcome};
}
async function join(code: string,name: string) {
  const peer=await Client.connect(`/v1/p2p/rooms/${code}`);peer.hello(name);
  return {peer,welcome:await peer.json("p2p_welcome")};
}
describe("P2P signaling",()=>{
  it("authenticates sender identity and restricts signaling to host-star pairs",async()=>{
    const {host,welcome:h}=await room();
    const {peer:a,welcome:wa}=await join(h.code,"A");
    const {peer:b,welcome:wb}=await join(h.code,"B");
    a.sendJson({type:"p2p_signal",to:h.memberId,from:wb.memberId,signal:{type:"offer",sdp:"test"}});
    const signal=await host.json("p2p_signal");expect(signal.from).toBe(wa.memberId);
    a.sendJson({type:"p2p_signal",to:wb.memberId,signal:{type:"offer",sdp:"test"}});
    expect((await a.json("error")).message).toContain("host-star");
    host.ws.close();a.ws.close();b.ws.close();
  });
  it("ends the room on host loss rather than migrating the host",async()=>{
    const {host,welcome:h}=await room();const {peer}=await join(h.code,"Guest");
    host.ws.close();expect((await peer.json("error")).code).toBe("host_left");
    await peer.closed();
  });
  it("does not coordinate race starts or relay gameplay over the lobby",async()=>{
    const {host,welcome:h}=await room();const {peer}=await join(h.code,"Guest");
    host.sendJson({type:"start"});host.sendJson({type:"ready",ready:true});
    peer.sendJson({type:"ping",t:12});expect((await peer.json("pong")).t).toBe(12);
    expect(peer.seen.some(s=>JSON.parse(s).type==="start")).toBe(false);
    host.ws.close();peer.ws.close();
  });
});
