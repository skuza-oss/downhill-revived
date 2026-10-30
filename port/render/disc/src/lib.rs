//! The disc image's online check (finding 098): the one implementation of the game files hash and
//! of its cache, iso-check.txt.
//!
//! Online races need the USA disc's game files, not a byte-exact copy of redump's image: dumping
//! tools differ in the sectors the game never reads (the system area, the padding between files,
//! the lead-out). So the hash covers the files of the ISO 9660 file system only: every file's path,
//! size and contents, in path order (`hash`). Two dumps whose files are the same give the same
//! hash wherever the files lie on the disc.
//!
//! - The game's host (port/host/host_sdl.c) calls the C interface (`c.rs`,
//!   port/render/include/dhd_disc.h) on a thread, through dhd_render's static library (`native`).
//!   The online menu runs in the game core, which has no Rust: it keeps a copy of the USA disc's
//!   value (port/hle/online_menu.c us_files_sha256; tests/constant.rs keeps the two the same).
//! - The launcher (port/launcher) uses it as a Rust crate, before the game starts.
//! - The browser (port/web/www/disc.js) loads it as a small wasm module of its own in the game
//!   worker (`web`, built by rustgs.py with `--crate dhd_disc`; finding 122): the same hash over the
//!   player's file, read through the page, without iso-check.txt.

#[cfg(not(target_arch = "wasm32"))]
pub mod c;
#[cfg(target_arch = "wasm32")]
mod web;

use ring::digest::{Context, SHA256};
use std::collections::HashSet;
use std::fmt;
use std::fs::{self, File};
use std::io::{self, Read, Seek, SeekFrom};
use std::path::Path;
use std::time::UNIX_EPOCH;

/// An ISO 9660 sector.
pub const SECTOR: u64 = 2048;

/// The game files hash of Downhill Domination (USA), SCUS-97177: redump disc 30393's image, whose
/// whole-file SHA-256 is 0fe2b77713d3440cc3030741ea0b18f49e3feaa09671a30a421e0d4888a7eb86. The one
/// online races take (online_menu.c us_files_sha256).
pub const USA_FILES_SHA256: &str = "c8c17bcae383daf9f2feadc7f6d7476c43d659ab8d66127a299b01311f2a3476";
/// How many files that disc has.
pub const USA_FILE_COUNT: usize = 2338;
/// The first 4 bytes of the SHA-256 of each of the USA disc's file records (`record`), sorted: they
/// name the first file of another image that isn't the USA disc's, for the log
/// (`DHD_DISC_BLESS=1 cargo test -p dhd_disc --test reference` writes it from the image).
static USA_TAGS: &[u8] = include_bytes!("usa_files.bin");

/// iso-check.txt's mark for a line holding this hash. 0.6.x wrote "size:ticks:" and the whole
/// file's SHA-256: such a line never starts with it, and this one never matches 0.6.x's key.
const CACHE_MARK: &str = "files:";

/// The deepest directory the walk follows (the USA disc goes 4 deep).
const MAX_DEPTH: usize = 32;
/// The largest directory read (the USA disc's largest is a few sectors).
const MAX_DIRECTORY: u32 = 16 << 20;

/// A file of the image: its path from the root ("BIKE/BANIM.NGA", without the ";1" version),
/// first sector and size.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Entry {
    pub path: String,
    pub sector: u32,
    pub size: u32,
}

#[derive(Debug)]
pub enum Error {
    /// The image can't be read: gone, or a read error.
    Read(io::Error),
    /// The image has no ISO 9660 file system the check can read, or a file lies past its end
    /// (a cut dump): its game files can't be the USA disc's.
    Format(String),
    /// The caller's progress said stop.
    Stopped,
}

impl From<io::Error> for Error {
    fn from(e: io::Error) -> Self {
        Error::Read(e)
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Error::Read(e) => write!(f, "{e}"),
            Error::Format(why) => f.write_str(why),
            Error::Stopped => f.write_str("stopped"),
        }
    }
}

fn format(why: impl Into<String>) -> Error {
    Error::Format(why.into())
}

/// The check's answer for an image.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Files {
    /// The game files hash.
    pub sha256: [u8; 32],
    /// How many files the image has.
    pub count: usize,
    /// The first file (in path order) that isn't one of the USA disc's: another path, size or
    /// contents. None when every file is one of the USA disc's.
    pub first_other: Option<String>,
}

impl Files {
    /// The hash in lower-case hex, as the server and iso-check.txt take it.
    pub fn hex(&self) -> String {
        hex(&self.sha256)
    }

    /// The USA disc's game files: online races take this image.
    pub fn is_usa(&self) -> bool {
        self.hex() == USA_FILES_SHA256
    }

    /// What differs from the USA disc, for the log: "" for the USA disc's files.
    pub fn note(&self) -> String {
        if self.is_usa() {
            return String::new();
        }
        let mut note = Vec::new();
        if let Some(path) = &self.first_other {
            note.push(format!("{path} isn't the USA disc's"));
        }
        if self.count != USA_FILE_COUNT {
            note.push(format!("{} files, the USA disc has {USA_FILE_COUNT}", self.count));
        }
        note.join("; ")
    }
}

fn hex(bytes: &[u8]) -> String {
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}

fn le32(b: &[u8]) -> u32 {
    u32::from_le_bytes([b[0], b[1], b[2], b[3]])
}

/// size bytes from sector on; a read past the image's end is a cut dump (what names it).
fn read_at(r: &mut (impl Read + Seek), sector: u32, buf: &mut [u8], what: &str) -> Result<(), Error> {
    r.seek(SeekFrom::Start(sector as u64 * SECTOR))?;
    r.read_exact(buf).map_err(|e| match e.kind() {
        io::ErrorKind::UnexpectedEof => format(format!("{what} lies past the image's end")),
        _ => Error::Read(e),
    })
}

/// Every file of the image's ISO 9660 file system (its primary volume descriptor, sector 16), in
/// the order the directories list them.
pub fn entries(r: &mut (impl Read + Seek)) -> Result<Vec<Entry>, Error> {
    let mut pvd = [0u8; SECTOR as usize];
    read_at(r, 16, &mut pvd, "the volume descriptor").map_err(|e| match e {
        Error::Format(_) => format("it isn't a disc image (no ISO 9660 volume in it)"),
        e => e,
    })?;
    if pvd[0] != 1 || &pvd[1..6] != b"CD001" {
        return Err(format("it isn't a disc image (no ISO 9660 volume in it)"));
    }
    // the root directory's record (at 156): its extent at +2, its size at +10
    let mut todo = vec![(le32(&pvd[158..]), le32(&pvd[166..]), String::new(), 0)];
    let mut seen = HashSet::new();
    let mut files = Vec::new();
    while let Some((sector, size, prefix, depth)) = todo.pop() {
        if depth > MAX_DEPTH || size > MAX_DIRECTORY {
            return Err(format(format!("the directory {prefix} is damaged")));
        }
        if !seen.insert(sector) {
            continue; // a directory listed twice: its files count once
        }
        let mut data = vec![0u8; size as usize];
        read_at(r, sector, &mut data, &format!("the directory /{prefix}"))?;
        let mut i = 0;
        let mut found = Vec::new();
        while i < data.len() {
            let n = data[i] as usize;
            if n == 0 {
                // no record crosses a sector: the rest of this one is padding
                i = (i / SECTOR as usize + 1) * SECTOR as usize;
                continue;
            }
            let rec = data.get(i..i + n).filter(|r| n >= 34 && 33 + r[32] as usize <= n);
            let Some(rec) = rec else {
                return Err(format(format!("the directory /{prefix} is damaged")));
            };
            i += n;
            let name = &rec[33..33 + rec[32] as usize];
            if name == [0] || name == [1] {
                continue; // "." and ".."
            }
            let (extent, length, flags) = (le32(&rec[2..]), le32(&rec[10..]), rec[25]);
            let name = String::from_utf8_lossy(name);
            if flags & 2 != 0 {
                found.push((extent, length, format!("{prefix}{name}/"), depth + 1));
                continue;
            }
            if flags & 0x80 != 0 {
                return Err(format(format!("{prefix}{name} is stored in pieces")));
            }
            // FILE.EXT;1 is FILE.EXT; FILE.;1 (no extension) is FILE
            let name = name.split(';').next().unwrap_or_default();
            let name = name.strip_suffix('.').unwrap_or(name);
            files.push(Entry { path: format!("{prefix}{name}"), sector: extent, size: length });
        }
        todo.extend(found.into_iter().rev());
    }
    Ok(files)
}

/// What the hash covers for one file: its path (UTF-8), a 0, its size (8 bytes, little-endian)
/// and its contents' SHA-256.
pub fn record(path: &str, size: u32, contents_sha256: &[u8]) -> Vec<u8> {
    let mut r = Vec::with_capacity(path.len() + 41);
    r.extend_from_slice(path.as_bytes());
    r.push(0);
    r.extend_from_slice(&(size as u64).to_le_bytes());
    r.extend_from_slice(contents_sha256);
    r
}

fn sha256(bytes: &[u8]) -> [u8; 32] {
    let mut out = [0u8; 32];
    out.copy_from_slice(ring::digest::digest(&SHA256, bytes).as_ref());
    out
}

/// The USA disc's tag i (`USA_TAGS`).
fn usa_tag(i: usize) -> &'static [u8] {
    &USA_TAGS[4 * i..4 * i + 4]
}

fn usa_has(record: &[u8]) -> bool {
    let tag = &sha256(record)[..4];
    let (mut lo, mut hi) = (0, USA_TAGS.len() / 4);
    while lo < hi {
        let mid = (lo + hi) / 2;
        match usa_tag(mid).cmp(tag) {
            std::cmp::Ordering::Less => lo = mid + 1,
            std::cmp::Ordering::Greater => hi = mid,
            std::cmp::Ordering::Equal => return true,
        }
    }
    false
}

/// The image's game files hash: the SHA-256 of every file's record (`record`), sorted (by path,
/// as the records start with it), one after the other. Nothing outside the files counts. The files
/// are read in their order on the disc, so the image is read once from start to end.
/// progress(done, total), in bytes of the files, is called first and after each piece; it stops the
/// hashing (Error::Stopped) when it returns false.
pub fn hash(r: &mut (impl Read + Seek), mut progress: impl FnMut(u64, u64) -> bool) -> Result<Files, Error> {
    let mut files = entries(r)?;
    files.sort_by_key(|f| f.sector);
    let total = files.iter().map(|f| f.size as u64).sum();
    if !progress(0, total) {
        return Err(Error::Stopped);
    }
    let mut buf = vec![0u8; 1 << 20];
    let mut done = 0u64;
    let mut records = Vec::with_capacity(files.len());
    for f in &files {
        let mut contents = Context::new(&SHA256);
        r.seek(SeekFrom::Start(f.sector as u64 * SECTOR))?;
        let mut left = f.size as usize;
        while left > 0 {
            let n = left.min(buf.len());
            r.read_exact(&mut buf[..n]).map_err(|e| match e.kind() {
                io::ErrorKind::UnexpectedEof => format(format!("{} lies past the image's end", f.path)),
                _ => Error::Read(e),
            })?;
            contents.update(&buf[..n]);
            left -= n;
            done += n as u64;
            if !progress(done, total) {
                return Err(Error::Stopped);
            }
        }
        records.push((record(&f.path, f.size, contents.finish().as_ref()), &f.path));
    }
    records.sort();
    let mut all = Context::new(&SHA256);
    for (record, _) in &records {
        all.update(record);
    }
    let mut sha256 = [0u8; 32];
    sha256.copy_from_slice(all.finish().as_ref());
    let first_other = records.iter().find(|(record, _)| !usa_has(record)).map(|(_, path)| path.to_string());
    Ok(Files { sha256, count: records.len(), first_other })
}

/// The game files hash of the image at path (`hash`).
pub fn files_sha256(path: &Path, progress: impl FnMut(u64, u64) -> bool) -> Result<Files, Error> {
    hash(&mut File::open(path)?, progress)
}

/// The start of iso-check.txt's line for the image at path as it is now: the mark, its size and
/// its last write time (ns since 1970).
fn cache_key(path: &Path) -> Option<String> {
    let meta = fs::metadata(path).ok()?;
    let since = meta.modified().ok()?.duration_since(UNIX_EPOCH).ok()?;
    Some(format!("{CACHE_MARK}{}:{}:", meta.len(), since.as_nanos()))
}

/// The answer iso-check.txt (at cache) holds for the image at path, when it was written for the
/// file as it is now. Its line: "files:size:time:hash:count:first other file".
pub fn cached(cache: &Path, path: &Path) -> Option<Files> {
    let key = cache_key(path)?;
    let text = fs::read_to_string(cache).ok()?;
    let rest = text.lines().next()?.strip_prefix(key.as_str())?;
    let mut parts = rest.splitn(3, ':');
    let digest = parts.next().filter(|h| h.len() == 64)?;
    let count = parts.next()?.parse().ok()?;
    let first_other = parts.next().filter(|p| !p.is_empty()).map(str::to_string);
    let mut sha256 = [0u8; 32];
    for (i, b) in sha256.iter_mut().enumerate() {
        *b = u8::from_str_radix(digest.get(2 * i..2 * i + 2)?, 16).ok()?;
    }
    Some(Files { sha256, count, first_other })
}

/// The online check of the image at path: iso-check.txt's answer (at cache) when it holds this
/// file's, else hashed now (progress as in `hash`) and written there, for the next start and for
/// the launcher or the game, whichever comes next. The bool: from iso-check.txt.
pub fn check(cache: Option<&Path>, path: &Path, progress: impl FnMut(u64, u64) -> bool) -> Result<(Files, bool), Error> {
    if let Some(files) = cache.and_then(|c| cached(c, path)) {
        return Ok((files, true));
    }
    let key = cache_key(path);
    let files = files_sha256(path, progress)?;
    if let (Some(cache), Some(key)) = (cache, key) {
        // a cache that can't be written only costs the next start its hashing
        let first = files.first_other.as_deref().unwrap_or_default();
        let _ = fs::write(cache, format!("{key}{}:{}:{first}\n", files.hex(), files.count));
    }
    Ok((files, false))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    /// A small ISO 9660 image: the volume descriptor at 16, the root directory at 20, a
    /// subdirectory at 21, the files from 24 on (each in its own sectors, in the order given).
    /// Paths with a '/' go in the subdirectory, which is named by it.
    fn image(files: &[(&str, &[u8])]) -> Vec<u8> {
        let mut img = vec![0u8; 24 * SECTOR as usize];
        let pvd = 16 * SECTOR as usize;
        img[pvd] = 1;
        img[pvd + 1..pvd + 6].copy_from_slice(b"CD001");
        img[pvd + 158..pvd + 162].copy_from_slice(&20u32.to_le_bytes());
        img[pvd + 166..pvd + 170].copy_from_slice(&(SECTOR as u32).to_le_bytes());
        let mut dirs: [Vec<u8>; 2] = [Vec::new(), Vec::new()];
        let entry = |extent: u32, size: u32, flags: u8, name: &[u8]| {
            let mut r = vec![0u8; 33 + name.len() + (name.len() + 1) % 2];
            r[0] = r.len() as u8;
            r[2..6].copy_from_slice(&extent.to_le_bytes());
            r[10..14].copy_from_slice(&size.to_le_bytes());
            r[25] = flags;
            r[32] = name.len() as u8;
            r[33..33 + name.len()].copy_from_slice(name);
            r
        };
        for d in &mut dirs {
            d.extend(entry(0, 0, 2, &[0]));
            d.extend(entry(0, 0, 2, &[1]));
        }
        let mut sub = None;
        for (path, data) in files {
            let at = (img.len() as u64 / SECTOR) as u32;
            img.extend_from_slice(data);
            img.resize(img.len().div_ceil(SECTOR as usize) * SECTOR as usize + SECTOR as usize, 0);
            let (dir, name) = match path.split_once('/') {
                Some((d, n)) => {
                    sub = Some(d.to_string());
                    (1, n)
                }
                None => (0, *path),
            };
            dirs[dir].extend(entry(at, data.len() as u32, 0, format!("{name};1").as_bytes()));
        }
        if let Some(name) = sub {
            dirs[0].extend(entry(21, SECTOR as u32, 2, name.as_bytes()));
        }
        for (d, at) in dirs.iter().zip([20, 21]) {
            img[at * SECTOR as usize..at * SECTOR as usize + d.len()].copy_from_slice(d);
        }
        img
    }

    fn files_of(img: &[u8]) -> Result<Files, Error> {
        hash(&mut Cursor::new(img), |_, _| true)
    }

    const FILES: &[(&str, &[u8])] = &[("SYSTEM.CNF", b"BOOT2 = cdrom0:\\SCUS_971.77;1\r\n"), ("DATA/B.BIN", b"bee"), ("A.BIN", b"a")];

    #[test]
    fn hashes_paths_sizes_and_contents_in_path_order() {
        let img = image(FILES);
        let mut paths: Vec<_> = entries(&mut Cursor::new(&img)).unwrap().into_iter().map(|e| e.path).collect();
        paths.sort();
        assert_eq!(paths, ["A.BIN", "DATA/B.BIN", "SYSTEM.CNF"]);
        // the definition, written out: each record, in path order
        let mut want = Vec::new();
        for (path, data) in [FILES[2], FILES[1], FILES[0]] {
            want.extend(record(path, data.len() as u32, &sha256(data)));
        }
        let files = files_of(&img).unwrap();
        assert_eq!(files.sha256, sha256(&want));
        // the value port/web/check_disc.mjs wants of the browser's module for this image (finding 122)
        assert_eq!(files.hex(), "0932bcbc0f752445101cdd1452fd03f6ff592d06c9e7d28cb2dbfd27d7864106");
        assert_eq!(files.count, 3);
        assert_eq!(files.first_other.as_deref(), Some("A.BIN"));
        assert!(!files.is_usa());
        assert_eq!(files.note(), "A.BIN isn't the USA disc's; 3 files, the USA disc has 2338");
    }

    #[test]
    fn only_the_files_count() {
        let img = image(FILES);
        let want = files_of(&img).unwrap();
        // the system area, the padding after a file and anything past the last one
        for at in [0, 15 * SECTOR as usize + 7, 24 * SECTOR as usize + 40] {
            let mut other = img.clone();
            other[at] ^= 1;
            assert_eq!(files_of(&other).unwrap(), want, "byte {at}");
        }
        let mut longer = img.clone();
        longer.extend([0x55; 3 * SECTOR as usize]);
        assert_eq!(files_of(&longer).unwrap(), want);
        // the same files written in another order on the disc
        assert_eq!(files_of(&image(&[FILES[2], FILES[0], FILES[1]])).unwrap(), want);
        // a byte of a file
        let mut other = img.clone();
        other[(24 + 2) * SECTOR as usize + 1] ^= 1; // DATA/B.BIN's second byte
        let files = files_of(&other).unwrap();
        assert_ne!(files.sha256, want.sha256);
        // a name or a size
        assert_ne!(files_of(&image(&[FILES[0], ("DATA/C.BIN", b"bee"), FILES[2]])).unwrap().sha256, want.sha256);
        assert_ne!(files_of(&image(&[FILES[0], ("DATA/B.BIN", b"bee\0"), FILES[2]])).unwrap().sha256, want.sha256);
        assert_ne!(files_of(&image(&FILES[..2])).unwrap().sha256, want.sha256);
    }

    #[test]
    fn names_what_it_cant_read() {
        let img = image(FILES);
        let no_volume = files_of(&img[..16 * SECTOR as usize]).unwrap_err();
        assert!(matches!(&no_volume, Error::Format(w) if w.contains("no ISO 9660")), "{no_volume}");
        let mut other = img.clone();
        other[16 * SECTOR as usize + 1] = b'X';
        assert!(matches!(files_of(&other), Err(Error::Format(_))));
        // a cut dump: the last file's sector (28) is gone
        let cut = files_of(&img[..28 * SECTOR as usize]).unwrap_err();
        assert!(matches!(&cut, Error::Format(w) if w == "A.BIN lies past the image's end"), "{cut}");
        // a damaged directory record
        let mut other = img.clone();
        other[20 * SECTOR as usize + 34] = 20;
        assert!(matches!(files_of(&other), Err(Error::Format(w)) if w.contains("damaged")));
        assert!(matches!(files_sha256(Path::new("no-such.iso"), |_, _| true), Err(Error::Read(_))));
        // stopped when another disc is chosen
        assert!(matches!(hash(&mut Cursor::new(&img), |done, _| done == 0), Err(Error::Stopped)));
    }

    #[test]
    fn reports_progress_over_the_files() {
        let big = vec![7u8; 3 << 20];
        let img = image(&[FILES[0], ("BIG.BIN", &big)]);
        let mut seen = vec![];
        hash(&mut Cursor::new(&img), |done, total| {
            seen.push((done, total));
            true
        })
        .unwrap();
        let total = (big.len() + FILES[0].1.len()) as u64;
        assert_eq!(seen.first(), Some(&(0, total)));
        assert_eq!(seen.last(), Some(&(total, total)));
        assert!(seen.len() > 3);
    }

    fn temp(name: &str) -> std::path::PathBuf {
        std::env::temp_dir().join(format!("dhd-disc-{}-{name}", std::process::id()))
    }

    #[test]
    fn keeps_its_answer_in_iso_check_txt() {
        let (iso, cache) = (temp("cache.iso"), temp("iso-check.txt"));
        fs::write(&iso, image(FILES)).unwrap();
        let want = files_sha256(&iso, |_, _| true).unwrap();
        let mut hashed = false;
        let (files, from_cache) = check(Some(&cache), &iso, |_, _| {
            hashed = true;
            true
        })
        .unwrap();
        assert!(hashed && !from_cache);
        assert_eq!(files, want);
        let line = fs::read_to_string(&cache).unwrap();
        assert_eq!(line, format!("{}{}:3:A.BIN\n", cache_key(&iso).unwrap(), want.hex()));
        assert!(line.starts_with("files:"));
        // the next time from the cache, without hashing
        assert_eq!(check(Some(&cache), &iso, |_, _| panic!("hashed again")).unwrap(), (want.clone(), true));
        // 0.6.x's line (size:ticks:whole file's SHA-256) is no answer, even for this file
        let meta = fs::metadata(&iso).unwrap();
        fs::write(&cache, format!("{}:133000000000000000:{}\n", meta.len(), USA_FILES_SHA256)).unwrap();
        assert_eq!(cached(&cache, &iso), None);
        // stale: the file changed (only its write time)
        fs::write(&cache, line).unwrap();
        assert!(cached(&cache, &iso).is_some());
        let later = meta.modified().unwrap() + std::time::Duration::from_secs(60);
        File::options().write(true).open(&iso).unwrap().set_modified(later).unwrap();
        assert_eq!(cached(&cache, &iso), None);
        // without a cache: hashed every time
        assert_eq!(check(None, &iso, |_, _| true).unwrap(), (want, false));
        let _ = fs::remove_file(iso);
        let _ = fs::remove_file(cache);
    }

    #[test]
    fn the_usa_tags_are_sorted() {
        assert_eq!(USA_TAGS.len(), 4 * USA_FILE_COUNT);
        assert!((1..USA_FILE_COUNT).all(|i| usa_tag(i - 1) <= usa_tag(i)));
    }
}
