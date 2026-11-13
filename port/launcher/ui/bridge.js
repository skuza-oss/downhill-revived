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

const bridge = (() => {
  const tauri = typeof window !== 'undefined' ? window.__TAURI__ : undefined;
  if (tauri) {
    return {
      kind: 'tauri',
      call: (name, args) => tauri.core.invoke(name, args),
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
  return {
    kind: 'message',
    call(name, args = {}) {
      const id = next++;
      return new Promise((resolve, reject) => {
        waiting.set(id, { resolve, reject });
        send(JSON.stringify({ id, call: name, args }));
      });
    },
    on(event, handler) {
      if (!handlers.has(event)) handlers.set(event, []);
      handlers.get(event).push(handler);
      return Promise.resolve(() => handlers.set(event, handlers.get(event).filter((h) => h !== handler)));
    },
    ready,
    receive,  // for the tests
  };
}

if (typeof module !== 'undefined') module.exports = { messageBridge };
