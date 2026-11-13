// The launcher's one way to its native side (finding 225; the calls: port/launcher/BRIDGE.md).
// The page asks for everything that touches files, the network or the game through call(name,
// args) and hears of progress through on(event, handler), with the same names on every platform:
// - the desktop (Tauri): its commands (src-tauri/src/main.rs) and events;
// - a phone or tablet (Android's WebView now, iOS's WKWebView later): a MessagePort the app posts to
//   this page's origin only, once it has loaded, with the message "dhd-bridge". The page sends
//   {id, call, args} as JSON text; the app answers {id, ok: true, value} or {id, ok: false, error}
//   and sends {event, payload} for events.
// What the platform can do comes from the snapshot's caps list (BRIDGE.md, "Capabilities"); the
// page never asks which platform it is on.
'use strict';

// The handoff gate (finding 228): the calls that write dhd.ini, and the handoffs that must wait
// for them. PLAY (the editor row's too) starts a game that reads dhd.ini at once, and an update
// replaces the launcher. A setting operation (`operation`: one change from its first write to its
// last, e.g. the three writes of Performance mode or a tab's reset) is one unit. A handoff waits
// until no operation is in progress and no write is pending, looking again after every change (an
// operation issues its next write only after the one before answered), and only then goes to the
// native side, which keeps its own order too (Android's settings queue, the settings crate's lock).
// Once a handoff has begun no new operation starts, no write goes out after it was sent, and a
// second handoff (PLAY tapped twice, the editor row during the transition) is the same one.
const WRITES = new Set(['save_setting', 'controls_edit']);
const HANDOFFS = new Set(['play', 'install_update']);
function orderedCalls(call) {
  const pending = new Set();   // writes sent and not answered
  const changes = new Set();   // a handoff waiting for the next change
  let operations = 0, handoff = null, sent = false;
  const changed = () => { for (const wake of [...changes]) wake(); };
  const busy = () => ({ code: 'footer.handoffBusy' });
  async function handOff(name, args) {
    while (operations || pending.size) {
      await new Promise((resolve) => {
        const wake = () => { changes.delete(wake); resolve(); };
        changes.add(wake);
      });
    }
    sent = true;
    return call(name, args);
  }
  const ordered = (name, args) => {
    if (HANDOFFS.has(name)) {
      if (handoff) return handoff;
      const started = handOff(name, args);
      handoff = started.finally(() => { handoff = null; sent = false; });
      return handoff;
    }
    if (WRITES.has(name)) {
      if (sent) return Promise.reject(busy());
      const answer = call(name, args);
      pending.add(answer);
      const settled = () => { pending.delete(answer); changed(); };
      answer.then(settled, settled);
      return answer;
    }
    return call(name, args);
  };
  ordered.operation = async (fn) => {
    if (handoff) throw busy();
    operations++;
    try {
      return await fn();
    } finally {
      operations--;
      changed();
    }
  };
  return ordered;
}

const bridge = (() => {
  const tauri = typeof window !== 'undefined' ? window.__TAURI__ : undefined;
  if (tauri) {
    const tauriCall = orderedCalls((name, args) => tauri.core.invoke(name, args));
    return {
      kind: 'tauri',
      call: tauriCall,
      operation: tauriCall.operation,
      on: (event, handler) => tauri.event.listen(event, (e) => handler(e.payload)),
      ready: Promise.resolve(),
    };
  }
  return messageBridge(typeof window !== 'undefined' ? window : null);
})();

// The phones' bridge over a MessagePort. target: the window that receives the port (a stand-in in
// the tests: test_bridge.js).
function messageBridge(target) {
  let port = null, next = 1;
  const waiting = new Map(), handlers = new Map(), queue = [];
  let opened;
  const ready = new Promise((resolve) => { opened = resolve; });
  const send = (text) => (port ? port.postMessage(text) : queue.push(text));
  function receive(e) {
    let m;
    try { m = typeof e.data === 'string' ? JSON.parse(e.data) : e.data; } catch { return; }
    if (!m) return;
    if (m.event) {
      for (const h of handlers.get(m.event) || []) h(m.payload);
      return;
    }
    const w = waiting.get(m.id);
    if (!w) return;
    waiting.delete(m.id);
    if (m.ok) w.resolve(m.value);
    else w.reject(m.error);
  }
  target?.addEventListener('message', (e) => {
    if (port || e.data !== 'dhd-bridge' || !e.ports || !e.ports[0]) return;
    port = e.ports[0];
    port.onmessage = receive;
    if (port.start) port.start();
    for (const text of queue.splice(0)) port.postMessage(text);
    opened();
  });
  const call = orderedCalls((name, args = {}) => {
    const id = next++;
    return new Promise((resolve, reject) => {
      waiting.set(id, { resolve, reject });
      send(JSON.stringify({ id, call: name, args }));
    });
  });
  return {
    kind: 'message',
    call,
    operation: call.operation,
    on(event, handler) {
      if (!handlers.has(event)) handlers.set(event, []);
      handlers.get(event).push(handler);
      return Promise.resolve(() => handlers.set(event, handlers.get(event).filter((h) => h !== handler)));
    },
    ready,
    receive,  // for the tests
  };
}

if (typeof module !== 'undefined') module.exports = { messageBridge, orderedCalls };
