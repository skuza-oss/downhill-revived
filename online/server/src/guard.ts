// The online server's abuse guard (finding 161): the kill switch, limits per connection source,
// per-member caps, bans and the short moderation log. The game is open source, so the server can't
// tell an official build from another: what any client may do is limited here, and the operator
// can stop one.
import { CLOSE_CODES } from "./protocol";

/** The guard's settings and bindings. All optional: a server without them runs as before (online
    on, no limits per address, no bans, no log); wrangler.jsonc and README.md say which exist. */
export interface GuardEnv {
  /** The kill switch: "off" refuses new rooms, joins and race starts. Anything else is on. */
  ONLINE?: string;
  /** The refusal text while online is off (OFF_MESSAGE when unset). */
  ONLINE_MESSAGE?: string;
}

// ---- The kill switch ----

/**
 * The refusal while online is off. Every release shows a refusal's `message` on the ONLINE
 * screen's footer, which holds about 45 characters at 640 pixels (finding 054): keep it short.
 */
export const OFF_MESSAGE = "Online races are paused. Try again later.";

/** True while the operator has turned online off (ONLINE = "off"). */
export function onlineOff(env: GuardEnv): boolean {
  return typeof env.ONLINE === "string" && env.ONLINE.trim().toLowerCase() === "off";
}

/** What players read while online is off: ONLINE_MESSAGE, else OFF_MESSAGE. */
export function offMessage(env: GuardEnv): string {
  const m = env.ONLINE_MESSAGE;
  return typeof m === "string" && m.trim() ? m.trim().slice(0, 120) : OFF_MESSAGE;
}

// ---- Refusals before any room ----

/**
 * A refusal the Worker itself gives, before the connection reaches a room: the WebSocket is
 * accepted, says one error frame and closes with the code's close code. Every release reads it
 * as any room's refusal (an error frame, then a 4000-4099 close: no rejoin) and shows `message`.
 */
export function refuseSocket(code: keyof typeof CLOSE_CODES, message: string): Response {
  const { 0: client, 1: server } = new WebSocketPair();
  server.accept();
  server.send(JSON.stringify({ type: "error", code, message }));
  server.close(CLOSE_CODES[code], code);
  return new Response(null, { status: 101, webSocket: client });
}
