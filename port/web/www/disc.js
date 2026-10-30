// The disc image's online check in the browser (finding 122): the native game's own, the crate
// dhd_disc (port/render/disc, finding 098), built on its own as build/web/dhd_disc.wasm
// (port/render/disc/src/web.rs; its own memory, apart from the game's). It hashes the game files of
// the ISO 9660 file system, so a browser player's disc gives the native game's value for the same
// image, and online rooms compare it.
//
// It runs in the game worker: the module asks for the image's bytes through its one import,
// env.dhd_disc_read, which calls read(offset, out) synchronously (FileReaderSync on the player's
// File, as worker.js discSource does). Offsets are whole numbers of bytes, past 2^32 for a 2.5 GiB
// image (exact as JS numbers up to 2^53).

const NOTE = 1024;   // the note's room, NUL included (a file's path and a count: well under it)

// discCheck(wasm, read, size): wasm is build/web/dhd_disc.wasm's bytes (an ArrayBuffer or a view)
// or its compiled WebAssembly.Module; read(offset, out: Uint8Array) fills out from the image at
// offset and returns how many bytes it read (0 only at the image's end; < 0 or a throw: it can't be
// read); size is the image's size in bytes. The answer, as the native game's dhd_disc_check without
// its cache: { code, digest, note }:
//   code    1 hashed; -1 the image can't be read; -2 its game files can't be the USA disc's (no
//           ISO 9660 file system, or a cut dump)
//   digest  the game files hash, 64 lower-case hex digits (code 1), else ''
//   note    on code 1, what differs from the USA disc ('' for its files); else why there is no hash
// The hashing itself is synchronous (the whole image's files are read once, from start to end).
export async function discCheck(wasm, read, size) {
  if (!Number.isSafeInteger(size) || size < 0) throw Error(`disc check: size ${size}`);
  let memory, failure;
  const env = {
    dhd_disc_read(lo, hi, ptr, len) {
      try {
        const n = read((hi >>> 0) * 2 ** 32 + (lo >>> 0), new Uint8Array(memory.buffer, ptr >>> 0, len >>> 0));
        return Number.isInteger(n) && n >= 0 && n <= len ? n : -1;
      } catch (e) {
        failure = e;
        return -1;
      }
    },
  };
  const made = await WebAssembly.instantiate(wasm, { env });
  const exports = (made.instance || made).exports;
  memory = exports.memory;
  const digest = exports.dhd_disc_alloc(32), note = exports.dhd_disc_alloc(NOTE);
  try {
    const code = exports.dhd_disc_web_check(size % 2 ** 32, Math.floor(size / 2 ** 32), digest, note, NOTE);
    const bytes = new Uint8Array(memory.buffer);
    const text = new TextDecoder().decode(bytes.subarray(note, bytes.indexOf(0, note)));
    return {
      code,
      digest: code === 1 ? Array.from(bytes.subarray(digest, digest + 32), (b) => b.toString(16).padStart(2, '0')).join('') : '',
      note: failure && code === -1 ? `${text} (${failure.message || failure})` : text,
    };
  } finally {
    exports.dhd_disc_free(digest, 32);
    exports.dhd_disc_free(note, NOTE);
  }
}
