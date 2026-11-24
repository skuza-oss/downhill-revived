//! What every export enters first: the MXCSR guard, the export's name for the panic hook,
//! `Owned<T>` (one entry at a time per object) and the fatal path.
//!
//! **MXCSR.** The EE thread calls with `0xFFC0` (round toward zero, FTZ, DAZ). naga folds WGSL
//! constants with the host's floating point, and FXC parses the HLSL text naga writes the same way,
//! so a pipeline built under the caller's mode gets constants one ulp off (finding 085, S3; the
//! 244df66db class). `Guard::enter` saves MXCSR, sets IEEE (`ieee(csr)`: round to nearest, no FTZ,
//! no DAZ, every exception masked, flags clear) and restores the caller's value on drop, on every
//! return path. The `no-fp-guard` feature keeps the caller's MXCSR: the negative control of
//! `tests/device_smoke.rs`'s guard tests, which rustgs refuses outside its selftest. On aarch64
//! (Android) the same guard holds FPCR instead (`ieee_fpcr`: the EE thread's round toward zero and
//! flush to zero off). On wasm there is no control register and the guard only names the export.
//!
//! **Fatal path.** A panic is logged through `host::log` (naming the export), then the
//! `panic = "abort"` profile ends the process. An uncaptured device error is logged, then the
//! process aborts (`install_error_handler`), as the C path's on_error does: a validation error
//! means a wrong picture, never carry on. Out of memory on Windows ends with an error box and exit
//! code 1 instead (`out_of_memory`).
use std::cell::{Cell, UnsafeCell};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, Once};

/// IEEE mode for `csr`: every exception masked, flags clear, round to nearest, no FTZ, no DAZ.
pub const fn ieee(csr: u32) -> u32 {
    (csr | 0x1F80) & !(0x3F | 0xE040)
}

/// IEEE mode for an aarch64 FPCR (the Android build, finding 091): the EE thread runs with RMode 3
/// (round toward zero) and FZ (fp_mode.c); this is round to nearest (RMode 0), no flush to zero
/// (FZ, FZ16), no default NaN (DN), no alternate handling (AH, FIZ, NEP) and every trap disabled
/// (IOE, DZE, OFE, UFE, IXE, IDE). The status flags are FPSR's, not touched.
pub const fn ieee_fpcr(fpcr: u32) -> u32 {
    fpcr & !((3 << 22) | (1 << 24) | (1 << 25) | (1 << 19) | 0x7 | 0x9F00)
}

/// The IEEE mode of this target's control register (MXCSR, or FPCR on aarch64).
#[inline]
pub const fn ieee_mode(csr: u32) -> u32 {
    if cfg!(target_arch = "aarch64") {
        ieee_fpcr(csr)
    } else {
        ieee(csr)
    }
}

/// MXCSR now (FPCR on aarch64; 0 on targets with neither).
#[inline]
pub fn mxcsr() -> u32 {
    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    {
        let mut v: u32 = 0;
        // SAFETY: stmxcsr stores 4 bytes into a valid local.
        unsafe { core::arch::asm!("stmxcsr [{}]", in(reg) &mut v, options(nostack, preserves_flags)) };
        v
    }
    #[cfg(target_arch = "aarch64")]
    {
        let v: u64;
        // SAFETY: reading FPCR has no side effects.
        unsafe { core::arch::asm!("mrs {}, fpcr", out(reg) v, options(nomem, nostack, preserves_flags)) };
        v as u32
    }
    #[cfg(not(any(target_arch = "x86", target_arch = "x86_64", target_arch = "aarch64")))]
    {
        0
    }
}

/// Sets MXCSR (FPCR on aarch64; nothing on targets with neither).
#[inline]
pub fn set_mxcsr(v: u32) {
    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    // SAFETY: ldmxcsr loads 4 bytes from a valid local; the values used here keep exceptions masked.
    unsafe {
        core::arch::asm!("ldmxcsr [{}]", in(reg) &v, options(nostack, readonly, preserves_flags))
    };
    #[cfg(target_arch = "aarch64")]
    // SAFETY: the values written are the caller's own FPCR or ieee_fpcr of it (traps stay off).
    unsafe {
        core::arch::asm!("msr fpcr, {}", "isb", in(reg) v as u64, options(nostack, preserves_flags))
    };
    #[cfg(not(any(target_arch = "x86", target_arch = "x86_64", target_arch = "aarch64")))]
    let _ = v;
}

thread_local! {
    /// The export this thread is in ("" outside), for the panic hook.
    static EXPORT: Cell<&'static str> = const { Cell::new("") };
}

/// The export the calling thread is in, or "".
pub fn current_export() -> &'static str {
    EXPORT.with(|e| e.get())
}

/// Entered first thing in every export (`ffi.rs`); restores MXCSR and the outer export's name on
/// drop. Nested entries (an export calling another's code) restore in order.
#[must_use]
pub struct Guard {
    csr: u32,
    outer: &'static str,
}

impl Guard {
    pub fn enter(export: &'static str) -> Guard {
        install_hooks();
        let csr = mxcsr();
        if !cfg!(feature = "no-fp-guard") {
            set_mxcsr(ieee_mode(csr));
        }
        let outer = EXPORT.with(|e| e.replace(export));
        Guard { csr, outer }
    }
}

impl Drop for Guard {
    fn drop(&mut self) {
        EXPORT.with(|e| e.set(self.outer));
        if !cfg!(feature = "no-fp-guard") {
            set_mxcsr(self.csr);
        }
    }
}

/// Logs `message` through the host and aborts the process. Never returns.
pub fn fatal(message: &str) -> ! {
    crate::host::log(message);
    std::process::abort()
}

/// The panic hook (logs, naming the export; the abort profile then ends the process) and the
/// `log` crate's logger. Once per process; `Guard::enter` calls it.
pub fn install_hooks() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        std::panic::set_hook(Box::new(|info| {
            let export = current_export();
            crate::host::log(&format!("dhd_render: panic in {}: {info}",
                                      if export.is_empty() { "(no export)" } else { export }));
        }));
        crate::log::install();
    });
}

/// Makes every uncaptured error of `device` fatal: logged through the host, then abort. The device
/// (A2) installs it right after creation; work that may fail on purpose (a probe pipeline) runs
/// inside an error scope instead.
pub fn install_error_handler(device: &wgpu::Device) {
    device.on_uncaptured_error(std::sync::Arc::new(|e: wgpu::Error| {
        let line = format!("dhd_render: wgpu error in {}: {e}", current_export());
        if matches!(e, wgpu::Error::OutOfMemory { .. }) {
            // The process ends here: the memory line (finding 281) first, whatever the count of
            // earlier failures (`memory::failed` logs only some of those).
            crate::host::log(&crate::memory::report(&format!("a wgpu call in {}", current_export()), 0));
            out_of_memory(&line);
        }
        fatal(&line)
    }));
}

/// The device ran out of memory: no wrong picture, but the game can't go on. On Windows it is
/// logged, an error box says so (not with DHD_HEADLESS or a console: tools and test programs never
/// wait on a box) and the process exits with 1; an abort would end it as a crash (fail-fast,
/// 0xC0000409). Elsewhere `fatal`.
fn out_of_memory(line: &str) {
    #[cfg(windows)]
    {
        #[link(name = "kernel32", kind = "raw-dylib")]
        extern "system" {
            fn GetConsoleWindow() -> *mut std::ffi::c_void;
            fn ExitProcess(code: u32) -> !;
        }
        #[link(name = "user32", kind = "raw-dylib")]
        extern "system" {
            fn MessageBoxW(window: *mut std::ffi::c_void, text: *const u16, caption: *const u16, kind: u32) -> i32;
        }
        crate::host::log(line);
        crate::host::log("dhd_render: the graphics card ran out of memory; the game ends");
        // SAFETY: no arguments.
        if std::env::var_os("DHD_HEADLESS").is_none() && unsafe { GetConsoleWindow() }.is_null() {
            let wide = |s: &str| s.encode_utf16().chain([0]).collect::<Vec<u16>>();
            let text = wide("Downhill Revived ran out of graphics memory and has to close.\n\n\
                             Close other programs that use the graphics card and start the game again. \
                             The details are in dhd.log, next to the game.");
            let caption = wide("Downhill Revived");
            // SAFETY: both strings are NUL-terminated and live across the call. MB_OK |
            // MB_ICONERROR | MB_SETFOREGROUND, as host_error_box.
            unsafe { MessageBoxW(std::ptr::null_mut(), text.as_ptr(), caption.as_ptr(), 0x10 | 0x10000) };
        }
        // SAFETY: ends the process.
        unsafe { ExitProcess(1) }
    }
    #[cfg(not(windows))]
    let _ = line;
}

/// One renderer object (`DhdrExact`, `DhdrNative`, `DhdrSurface`) behind a C handle: one entry at
/// a time. A second entry while one runs (another thread, or a call between a read's begin and
/// finish) aborts and names both exports. No lock is held across calls and nothing blocks.
pub struct Owned<T> {
    busy: AtomicBool,
    /// The export holding the object (set while busy).
    holder: Mutex<&'static str>,
    inner: UnsafeCell<T>,
}

// SAFETY: `inner` is only reached by the one entry that set `busy` (an atomic swap), so it is
// never touched by two threads at once; T itself moves between threads (Send).
unsafe impl<T: Send> Sync for Owned<T> {}

impl<T> Owned<T> {
    pub fn new(value: T) -> Self {
        Owned { busy: AtomicBool::new(false), holder: Mutex::new(""), inner: UnsafeCell::new(value) }
    }

    fn acquire(&self, export: &'static str) {
        if self.busy.swap(true, Ordering::Acquire) {
            let holder = *self.holder.lock().unwrap_or_else(|p| p.into_inner());
            fatal(&format!("dhd_render: {export}: the object is in use by {holder} (one call at a time; \
                            nothing between a begin and its finish)"));
        }
        *self.holder.lock().unwrap_or_else(|p| p.into_inner()) = export;
    }

    fn release(&self) {
        *self.holder.lock().unwrap_or_else(|p| p.into_inner()) = "";
        self.busy.store(false, Ordering::Release);
    }

    /// Runs `f` with the object; aborts if another entry holds it.
    pub fn with<R>(&self, export: &'static str, f: impl FnOnce(&mut T) -> R) -> R {
        self.acquire(export);
        // SAFETY: acquire made this the only entry.
        let r = f(unsafe { &mut *self.inner.get() });
        self.release();
        r
    }

    /// A begin (`read_begin`, `capture_begin`): runs `f`; when it returns `(_, true)` the object
    /// stays held by `export` until `finish`, so any other call in between aborts.
    pub fn begin<R>(&self, export: &'static str, f: impl FnOnce(&mut T) -> (R, bool)) -> R {
        self.acquire(export);
        // SAFETY: acquire made this the only entry.
        let (r, hold) = f(unsafe { &mut *self.inner.get() });
        if !hold {
            self.release();
        }
        r
    }

    /// The finish of a begin that held the object: aborts unless `begin_export` holds it, runs
    /// `f`, then releases it.
    pub fn finish<R>(&self, export: &'static str, begin_export: &'static str, f: impl FnOnce(&mut T) -> R) -> R {
        let holder = *self.holder.lock().unwrap_or_else(|p| p.into_inner());
        if !self.busy.load(Ordering::Acquire) || holder != begin_export {
            fatal(&format!("dhd_render: {export} without a successful {begin_export} (held by {:?})", holder));
        }
        *self.holder.lock().unwrap_or_else(|p| p.into_inner()) = export;
        // SAFETY: busy is set and held by this begin/finish pair; no other entry gets in.
        let r = f(unsafe { &mut *self.inner.get() });
        self.release();
        r
    }

    /// The object, for its destroy (the handle is gone). Aborts if an entry holds it.
    pub fn into_inner(self, export: &'static str) -> T {
        if self.busy.load(Ordering::Acquire) {
            let holder = *self.holder.lock().unwrap_or_else(|p| p.into_inner());
            fatal(&format!("dhd_render: {export}: the object is in use by {holder}"));
        }
        self.inner.into_inner()
    }
}
