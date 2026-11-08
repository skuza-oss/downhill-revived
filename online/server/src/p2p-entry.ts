// Isolated deployment: this Worker owns only /v1/p2p/* and its own objects.
// Updating it never replaces the SFU Worker or restarts SFU rooms.
import online from "./index";
export { P2pRoom } from "./p2p";
export { LobbyIndex } from "./lobbies";

export default {
  fetch(request: Request, env: Env): Promise<Response> | Response {
    if (!new URL(request.url).pathname.startsWith("/v1/p2p/"))
      return new Response("Not found", { status: 404 });
    return online.fetch(request, env);
  },
};
