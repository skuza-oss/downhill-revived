// The online disc check's own worker (finding 138): disc.js (dhd_disc, finding 137) reads the whole
// image's game files, about 17 s for the USA disc, so it runs here, apart from the game worker, as
// host_sdl.c runs it on a thread of its own. The answer is kept in this browser (IndexedDB), by the
// file's name, size and last change, as the native game keeps iso-check.txt: then it is code 2,
// "from the cache", and the image isn't read again.
import { discCheck } from './disc.js';

const DB = 'dhd-disc-check', STORE = 'checks';
function store(mode, f) {
  return new Promise(done => {
    let open;
    try { open = indexedDB.open(DB, 1); } catch { done(undefined); return; }
    open.onupgradeneeded = () => open.result.createObjectStore(STORE);
    open.onerror = () => done(undefined);
    open.onsuccess = () => { try { const r = f(open.result.transaction(STORE, mode).objectStore(STORE));
      r.onsuccess = () => done(r.result); r.onerror = () => done(undefined); } catch { done(undefined); } };
  });
}

onmessage = async e => {
  const { file, isoUrl, wasm } = e.data;
  try {
    let size, read, key = null;
    if (file) {
      const reader = new FileReaderSync(); size = file.size; key = `${file.name}:${file.size}:${file.lastModified}`;
      read = (at, out) => { const b = new Uint8Array(reader.readAsArrayBuffer(file.slice(at, at + out.length))); out.set(b); return b.length; };
    } else {  // the test hook (serve.mjs --iso): Range requests to localhost, as worker.js reads it
      const head = new XMLHttpRequest(); head.open('HEAD', isoUrl, false); head.send();
      size = Number(head.getResponseHeader('Content-Length'));
      read = (at, out) => { const x = new XMLHttpRequest(); x.open('GET', isoUrl, false); x.responseType = 'arraybuffer';
        x.setRequestHeader('Range', `bytes=${at}-${Math.min(size, at + out.length) - 1}`); x.send();
        if (x.status !== 206) return -1; const b = new Uint8Array(x.response); out.set(b); return b.length; };
    }
    const kept = key && await store('readonly', s => s.get(key));
    if (kept?.code === 1) { postMessage({ ...kept, code: 2 }); return; }
    const answer = await discCheck(wasm, read, size);
    if (key && answer.code === 1) await store('readwrite', s => s.put(answer, key));
    postMessage(answer);
  } catch (err) { postMessage({ code: -1, digest: '', note: String(err?.message || err) }); }
};
