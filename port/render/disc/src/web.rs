//! The browser's interface (port/web/www/disc.js, finding 122): the crate built on its own as a small
//! wasm module (`rustgs.py build --target wasm32-unknown-unknown --crate dhd_disc`, then
//! build/web/dhd_disc.wasm), so a browser player's disc gives the same game files hash as the
//! native game's for the same image: online rooms compare it.
//!
//! The module defines and exports its own memory and imports one function, `env.dhd_disc_read`: the
//! game worker reads the player's file (a File, through FileReaderSync) at the offsets `hash`
//! asks for. No wasm-bindgen: a plain C interface, as dhd_settings's. No cache: the browser keeps
//! no iso-check.txt, and hashes at each start.

use std::io::{self, Read, Seek, SeekFrom};

#[link(wasm_import_module = "env")]
extern "C" {
    /// Up to `len` bytes of the image from `offset_hi << 32 | offset_lo` into `ptr` (this module's
    /// memory): how many were read (0 only at the image's end), or < 0 when it can't be read.
    fn dhd_disc_read(offset_lo: u32, offset_hi: u32, ptr: *mut u8, len: u32) -> i32;
}

/// The image the page reads: `size` bytes, through `dhd_disc_read`.
struct Image {
    pos: u64,
    size: u64,
}

impl Read for Image {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        if self.pos >= self.size || buf.is_empty() {
            return Ok(0);
        }
        let n = (buf.len() as u64).min(self.size - self.pos).min(i32::MAX as u64) as u32;
        // SAFETY: the import writes at most n bytes into buf.
        let got = unsafe { dhd_disc_read(self.pos as u32, (self.pos >> 32) as u32, buf.as_mut_ptr(), n) };
        if got < 0 || got as u32 > n {
            return Err(io::Error::other("the browser can't read the disc image"));
        }
        self.pos += got as u64;
        Ok(got as usize)
    }
}

impl Seek for Image {
    fn seek(&mut self, to: SeekFrom) -> io::Result<u64> {
        let pos = match to {
            SeekFrom::Start(n) => Some(n),
            SeekFrom::End(d) => self.size.checked_add_signed(d),
            SeekFrom::Current(d) => self.pos.checked_add_signed(d),
        };
        self.pos = pos.ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "a seek before the image's start"))?;
        Ok(self.pos)
    }
}

/// ring's getrandom has no source of random bytes in this target: the hash needs none.
fn no_random(_: &mut [u8]) -> Result<(), getrandom::Error> {
    Err(getrandom::Error::UNSUPPORTED)
}
getrandom::register_custom_getrandom!(no_random);

/// A buffer of `len` bytes in this module's memory, for `dhd_disc_web_check`'s digest and note.
#[no_mangle]
pub extern "C" fn dhd_disc_alloc(len: usize) -> *mut u8 {
    let mut v = Vec::<u8>::with_capacity(len.max(1));
    let p = v.as_mut_ptr();
    std::mem::forget(v);
    p
}

/// # Safety
/// `ptr` / `len` came from `dhd_disc_alloc(len)`.
#[no_mangle]
pub unsafe extern "C" fn dhd_disc_free(ptr: *mut u8, len: usize) {
    // SAFETY: the caller's contract: the allocation of that capacity.
    drop(unsafe { Vec::from_raw_parts(ptr, 0, len.max(1)) });
}

/// The online check of the image of `size_hi << 32 | size_lo` bytes the page reads
/// (`dhd_disc_read`): `crate::hash`, as the game's `dhd_disc_check` (c.rs) without its cache. On an
/// answer, `digest` gets the game files hash (32 bytes). `note` (`note_size` bytes, cut to fit,
/// NUL-terminated UTF-8) gets what the log says: on an answer, what differs from the USA disc (""
/// for its files); else why there is none. 1 hashed, -1 the image can't be read, -2 its game files
/// can't be the USA disc's (no ISO 9660 file system, or a cut dump): dhd_disc_check's codes.
///
/// # Safety
/// `digest` has room for 32 bytes, `note` for `note_size` (or is NULL).
#[no_mangle]
pub unsafe extern "C" fn dhd_disc_web_check(size_lo: u32, size_hi: u32, digest: *mut u8, note: *mut u8, note_size: usize) -> i32 {
    let mut image = Image { pos: 0, size: (size_hi as u64) << 32 | size_lo as u64 };
    let (code, said) = match crate::hash(&mut image, |_, _| true) {
        Ok(files) => {
            // SAFETY: room for 32 bytes.
            unsafe { std::ptr::copy_nonoverlapping(files.sha256.as_ptr(), digest, 32) };
            (1, files.note())
        }
        Err(e @ crate::Error::Format(_)) => (-2, e.to_string()),
        Err(e) => (-1, e.to_string()),
    };
    if !note.is_null() && note_size > 0 {
        let n = said.floor_char_boundary(note_size - 1);
        // SAFETY: room for note_size bytes; n < note_size.
        unsafe {
            std::ptr::copy_nonoverlapping(said.as_ptr(), note, n);
            *note.add(n) = 0;
        }
    }
    code
}
