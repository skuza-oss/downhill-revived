// The browser's disc check against the native game's (finding 137): build/web/dhd_disc.wasm (rustgs.py
// build --target wasm32-unknown-unknown --crate dhd_disc) through www/disc.js, in node, over small
// synthetic ISO 9660 images made here (no game data): the layout of the crate's own test images
// (port/render/disc/src/lib.rs tests::image), and the hash written out from its definition (each
// file's path, a 0, its size as 8 bytes and its contents' SHA-256, in path order; finding 098) with
// node's crypto. FIXTURE_SHA256 is the value the crate's native test gives the first image
// (hashes_paths_sizes_and_contents_in_path_order), so the native and the browser modules agree.
// The cases: that image (code 1 and the native note); its files in another order on the disc (the
// same hash); the same files past 4 GiB in a sparse image (64-bit offsets); a cut dump and an image
// without a volume (-2, the native notes); a read that fails (-1).
// --iso PATH: the check of a real image too, read with fs (printed: code, digest, note, seconds;
// the image is only read).
//   node port/web/check_disc.mjs [--wasm build/web/dhd_disc.wasm] [--iso PATH]
import crypto from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { discCheck } from './www/disc.js';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const args = process.argv.slice(2), option = (n, f) => { const i = args.indexOf(n); return i < 0 ? f : args[i + 1]; };
const wasmPath = path.resolve(option('--wasm', path.join(root, 'build/web/dhd_disc.wasm')));
const iso = option('--iso', null);
if (!fs.existsSync(wasmPath)) {
  console.error(`no ${wasmPath}: python port/tools/rustgs.py build --target wasm32-unknown-unknown --crate dhd_disc`);
  process.exit(2);
}
const wasm = new WebAssembly.Module(fs.readFileSync(wasmPath));
const FIXTURE_SHA256 = '0932bcbc0f752445101cdd1452fd03f6ff592d06c9e7d28cb2dbfd27d7864106';
const SECTOR = 2048;
const sha256 = (b) => crypto.createHash('sha256').update(b).digest();
const bytes = (s) => Buffer.from(s, 'latin1');

// The crate's test image: the volume descriptor at 16, the root directory at 20, a subdirectory at
// 21, each file in its own sectors from `first` on (24 in the crate's; another sector puts them far
// out), each followed by an empty sector. A path with a '/' goes in the subdirectory, named by it.
// Returns { size, read(offset, out) }: the image as a sparse one (zeros between its pieces).
function image(files, first = 24) {
  const low = Buffer.alloc(24 * SECTOR), pieces = [[0, low]];
  const pvd = 16 * SECTOR;
  low[pvd] = 1; low.write('CD001', pvd + 1, 'latin1');
  low.writeUInt32LE(20, pvd + 158); low.writeUInt32LE(SECTOR, pvd + 166);
  const entry = (extent, size, flags, name) => {
    const r = Buffer.alloc(33 + name.length + (name.length + 1) % 2);
    r[0] = r.length; r.writeUInt32LE(extent, 2); r.writeUInt32LE(size, 10); r[25] = flags; r[32] = name.length;
    name.copy(r, 33);
    return r;
  };
  const dirs = [[], []];
  for (const d of dirs) d.push(entry(0, 0, 2, Buffer.from([0])), entry(0, 0, 2, Buffer.from([1])));
  let at = first, sub = null;
  for (const [p, data] of files) {
    pieces.push([at * SECTOR, data]);
    const slash = p.indexOf('/'), dir = slash < 0 ? 0 : 1;
    if (slash >= 0) sub = p.slice(0, slash);
    dirs[dir].push(entry(at, data.length, 0, bytes(`${p.slice(slash + 1)};1`)));
    at += Math.ceil(data.length / SECTOR) + 1;
  }
  if (sub) dirs[0].push(entry(21, SECTOR, 2, bytes(sub)));
  Buffer.concat(dirs[0]).copy(low, 20 * SECTOR);
  Buffer.concat(dirs[1]).copy(low, 21 * SECTOR);
  const size = Math.max(at, 24) * SECTOR;
  return { size, read(offset, out) {
    const n = Math.max(0, Math.min(out.length, size - offset));
    out.fill(0, 0, n);
    for (const [o, b] of pieces) {
      const lo = Math.max(o, offset), hi = Math.min(o + b.length, offset + n);
      if (lo < hi) out.set(b.subarray(lo - o, hi - o), lo - offset);
    }
    return n;
  } };
}

// The hash from its definition: every file's record, in path order.
function definition(files) {
  const records = files.map(([p, data]) => Buffer.concat([bytes(p), Buffer.from([0]),
    (() => { const s = Buffer.alloc(8); s.writeBigUInt64LE(BigInt(data.length)); return s; })(), sha256(data)]));
  records.sort(Buffer.compare);
  return sha256(Buffer.concat(records)).toString('hex');
}

const FILES = [['SYSTEM.CNF', bytes('BOOT2 = cdrom0:\\SCUS_971.77;1\r\n')], ['DATA/B.BIN', bytes('bee')], ['A.BIN', bytes('a')]];
const NOTE = 'A.BIN isn\'t the USA disc\'s; 3 files, the USA disc has 2338';
let failed = 0;
function expect(name, got, want) {
  const ok = got.code === want.code && (want.digest === undefined || got.digest === want.digest) &&
    (want.note === undefined || (want.note instanceof RegExp ? want.note.test(got.note) : got.note === want.note));
  if (!ok) failed++;
  console.log(`${ok ? 'PASS' : 'FAIL'} ${name}: code ${got.code}${got.digest ? ` digest ${got.digest}` : ''} note ${JSON.stringify(got.note)}` +
    (ok ? '' : `; want ${JSON.stringify({ ...want, note: String(want.note) })}`));
}
const check = (img) => discCheck(wasm, img.read, img.size);

const want = definition(FILES);
if (want !== FIXTURE_SHA256) { failed++; console.log(`FAIL the definition gives ${want}, the crate's test ${FIXTURE_SHA256}`); }
expect('the crate\'s test image', await check(image(FILES)), { code: 1, digest: FIXTURE_SHA256, note: NOTE });
expect('its files in another order on the disc', await check(image([FILES[2], FILES[0], FILES[1]])), { code: 1, digest: FIXTURE_SHA256, note: NOTE });
const big = Buffer.alloc(3 << 20, 7);
const far = image([...FILES, ['BIG.BIN', big]], 3_000_000);   // from byte 6 144 000 000 on: past 2^32
expect(`files past 4 GiB (a ${(far.size / 2 ** 30).toFixed(1)} GiB image)`, await check(far),
  { code: 1, digest: definition([...FILES, ['BIG.BIN', big]]), note: 'A.BIN isn\'t the USA disc\'s; 4 files, the USA disc has 2338' });
const whole = image(FILES);
expect('a cut dump', await discCheck(wasm, whole.read, 28 * SECTOR), { code: -2, note: 'A.BIN lies past the image\'s end' });
expect('no volume', await discCheck(wasm, whole.read, 16 * SECTOR), { code: -2, note: /no ISO 9660/ });
expect('a read that fails', await discCheck(wasm, () => { throw Error('gone'); }, whole.size), { code: -1, note: /can't read.*gone/ });

if (iso) {
  const fd = fs.openSync(iso, 'r'), size = fs.fstatSync(fd).size, t = performance.now();
  const got = await discCheck(wasm, (offset, out) => fs.readSync(fd, out, 0, out.length, offset), size);
  fs.closeSync(fd);
  const usa = /USA_FILES_SHA256: &str = "([0-9a-f]{64})"/.exec(fs.readFileSync(path.join(root, 'port/render/disc/src/lib.rs'), 'utf8'))[1];
  console.log(`iso ${path.basename(iso)} (${(size / 2 ** 30).toFixed(2)} GiB, ${((performance.now() - t) / 1000).toFixed(1)} s): code ${got.code} ` +
    `digest ${got.digest} note ${JSON.stringify(got.note)}${got.digest === usa ? ' (the USA disc\'s files)' : ''}`);
  if (got.code !== 1) failed++;
}
console.log(failed ? `disc check: FAIL (${failed})` : 'disc check: OK');
process.exit(failed ? 1 : 0);
