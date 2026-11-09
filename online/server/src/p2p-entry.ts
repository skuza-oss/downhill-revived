// The official server's P2P Worker (wrangler.p2p.jsonc): only /v1/p2p/*, its own P2P room objects,
// so deploying it never restarts an SFU room. Self-hosted servers need not run it: their main
// Worker serves the same routes (src/index.ts, wrangler.selfhost.jsonc).
import { p2pRoute } from "./p2p";
export { P2pRoom } from "./p2p";

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const path = new URL(request.url).pathname.replace(/\/+$/, "");
    return (await p2pRoute(request, env, path)) ?? Response.json({ error: "Not found" }, { status: 404 });
  },
} satisfies ExportedHandler<Env>;
