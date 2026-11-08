import { exports } from "cloudflare:workers";

export const VERSION = "dhd-port test";
/** The release the test clients claim (hello.clientVersion); vitest.config.ts sets MIN_VERSION to it. */
export const CLIENT_VERSION = "0.2.0";
export const ISO = "a".repeat(64);

type Frame = string | ArrayBuffer | Blob;

/** A random documentation address (198.18.0.0/15, the benchmarking range). */
export function randomIp(): string {
  const b = crypto.getRandomValues(new Uint8Array(3));
  return `198.${18 + (b[0] & 1)}.${b[1]}.${b[2]}`;
}

/** A test player: a WebSocket to the worker with an inbox. */
export class Client {
  private inbox: Frame[] = [];
  private waiter: (() => void) | null = null;
  closeCode: number | null = null;
  /** Every text frame this client received, kept (the privacy test reads them all). */
  seen: string[] = [];
  /** The last welcome read (json("welcome")): its rejoinToken is what the game's rejoin sends. */
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  welcome: any = null;

  constructor(readonly ws: WebSocket) {
    ws.addEventListener("message", (e) => {
      if (typeof e.data === "string") this.seen.push(e.data);
      this.inbox.push(e.data as Frame);
      this.waiter?.();
    });
    ws.addEventListener("close", (e) => {
      this.closeCode = e.code;
      this.waiter?.();
    });
  }

  /** cf: what Cloudflare would say of the connection (request.cf: colo, country, continent...). */
  /** ip: the address Cloudflare would give (CF-Connecting-IP); each connection has its own by
      default, so the limits per address (wrangler.jsonc ratelimits) don't add up across tests. */
  static async connect(path: string, cf?: Record<string, unknown>, ip = randomIp()): Promise<Client> {
    const res = await exports.default.fetch(`https://dhd.test${path}`, {
      headers: { Upgrade: "websocket", "CF-Connecting-IP": ip },
      ...(cf ? { cf } : {}),
    });
    if (res.status !== 101 || !res.webSocket) throw new Error(`upgrade failed: ${res.status}`);
    res.webSocket.accept();
    return new Client(res.webSocket);
  }

  /** Creates a room as host; returns the client and the room code. */
  static async host(name = "Host"): Promise<{ host: Client; code: string }> {
    const host = await Client.connect("/v1/rooms/new");
    host.hello(name);
    const welcome = await host.json("welcome");
    return { host, code: welcome.code };
  }

  static async join(code: string, name: string): Promise<Client> {
    const c = await Client.connect(`/v1/rooms/${code}`);
    c.hello(name);
    await c.json("welcome");
    return c;
  }

  hello(name: string, extra: object = {}): void {
    this.sendJson({ type: "hello", protocol: 3, version: VERSION, clientVersion: CLIENT_VERSION, isoSha256: ISO, name, ...extra });
  }

  sendJson(obj: object): void {
    this.ws.send(JSON.stringify(obj));
  }

  /** Next frame matching `pred`, discarding the ones before it. */
  async next(pred: (f: Frame) => boolean, timeoutMs = 2000): Promise<Frame> {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      while (this.inbox.length) {
        const f = this.inbox.shift()!;
        if (pred(f)) return f;
      }
      if (this.closeCode !== null) throw new Error(`socket closed (${this.closeCode})`);
      const left = deadline - Date.now();
      if (left <= 0) throw new Error("timed out waiting for a frame");
      await new Promise<void>((resolve) => {
        const t = setTimeout(resolve, left);
        this.waiter = () => {
          clearTimeout(t);
          resolve();
        };
      });
      this.waiter = null;
    }
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  async json(type: string): Promise<any> {
    const f = await this.next((f) => typeof f === "string" && JSON.parse(f).type === type);
    const m = JSON.parse(f as string);
    if (type === "welcome") this.welcome = m;
    return m;
  }

  async binary(): Promise<Uint8Array> {
    const f = (await this.next((f) => typeof f !== "string")) as ArrayBuffer | Blob;
    // workerd's client sockets deliver binary messages as Blobs.
    return new Uint8Array(f instanceof Blob ? await f.arrayBuffer() : f);
  }

  /** Waits until the server closes the socket; returns the close code. */
  async closed(timeoutMs = 2000): Promise<number> {
    try {
      await this.next(() => false, timeoutMs);
    } catch {}
    if (this.closeCode === null) throw new Error("socket not closed");
    return this.closeCode;
  }

  /** Drops queued frames. */
  drain(): void {
    this.inbox.length = 0;
  }
}

/** Host plus `n - 1` joiners, all ready, race started. Returns clients by slot. */
export async function startedRace(n: number): Promise<{ clients: Client[]; code: string; starts: any[] }> {
  const { host, code } = await Client.host("P0");
  const clients = [host];
  for (let i = 1; i < n; i++) clients.push(await Client.join(code, `P${i}`));
  for (const c of clients) c.sendJson({ type: "ready", ready: true });
  // Wait until the host has seen everyone ready.
  await host.next((f) => typeof f === "string" && JSON.parse(f).type === "room" && JSON.parse(f).members.every((m: any) => m.ready) && JSON.parse(f).members.length === n);
  host.sendJson({ type: "start" });
  const starts = await Promise.all(clients.map((c) => c.json("start")));
  const bySlot: Client[] = [];
  starts.forEach((s, i) => (bySlot[s.yourSlot] = clients[i]));
  return { clients: bySlot, code, starts };
}
