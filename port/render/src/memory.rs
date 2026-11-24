//! What the renderer holds, for the line an out-of-memory failure logs (finding 281).
//!
//! Three parts, in bytes of their textures and buffers: the native renderer's decoded and
//! replacement textures (`textures.rs`: added when made, taken off when the last reference goes),
//! its surfaces (`surfaces.rs`: set after each change, the grown ones' old textures included) and
//! its arenas (`arenas.rs`: set after each growth). Relaxed atomics: a log line's numbers, read on
//! any thread.
//!
//! Each time the three together pass a new multiple of 64 MiB the log says so once (`memory: the
//! renderer holds ...`), so a player's log shows the growth before any failure.
//!
//! `report` makes the whole line: what was asked, the three parts, and on Apple the process's
//! memory footprint, what iOS still lets it take, and its free address space (iOS caps an app's
//! address space by the device's memory, finding 258: on a 3 GB iPhone every GPU resource, malloc
//! region and the guest share some 3.4 GiB). Elsewhere only the renderer's parts.
use std::sync::atomic::{AtomicU64, Ordering::Relaxed};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Part {
    Textures = 0,
    Surfaces = 1,
    Arenas = 2,
}

const PARTS: usize = 3;
static COUNT: [AtomicU64; PARTS] = [AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0)];
static BYTES: [AtomicU64; PARTS] = [AtomicU64::new(0), AtomicU64::new(0), AtomicU64::new(0)];
/// The next total that logs a line (`note`).
static MARK: AtomicU64 = AtomicU64::new(STEP);
const STEP: u64 = 64 << 20;

/// A line the first time the parts together reach each new multiple of `STEP`.
fn note() {
    let total: u64 = BYTES.iter().map(|b| b.load(Relaxed)).sum();
    let mark = MARK.load(Relaxed);
    if total >= mark && MARK.compare_exchange(mark, (total / STEP + 1) * STEP, Relaxed, Relaxed).is_ok() {
        crate::host::log(&format!("memory: the renderer holds {}: {}", mib(total), renderer()));
    }
}

/// One resource of `part` held while this lives (textures: shared, dropped on any thread).
#[derive(Debug)]
pub struct Held {
    part: Part,
    bytes: u64,
}

impl Held {
    pub fn new(part: Part, bytes: u64) -> Held {
        COUNT[part as usize].fetch_add(1, Relaxed);
        BYTES[part as usize].fetch_add(bytes, Relaxed);
        note();
        Held { part, bytes }
    }
}

impl Drop for Held {
    fn drop(&mut self) {
        COUNT[self.part as usize].fetch_sub(1, Relaxed);
        BYTES[self.part as usize].fetch_sub(self.bytes, Relaxed);
    }
}

/// A part whose owner knows its whole size (surfaces, arenas): `count` resources of `bytes` in all.
pub fn set(part: Part, count: u64, bytes: u64) {
    COUNT[part as usize].store(count, Relaxed);
    BYTES[part as usize].store(bytes, Relaxed);
    note();
}

/// (count, bytes) of each part, in `Part` order.
pub fn totals() -> [(u64, u64); PARTS] {
    std::array::from_fn(|i| (COUNT[i].load(Relaxed), BYTES[i].load(Relaxed)))
}

fn mib(bytes: u64) -> String {
    format!("{:.1} MiB", bytes as f64 / (1024.0 * 1024.0))
}

/// The renderer's parts: `textures 812 (93.1 MiB), surfaces 9 (61.2 MiB), arenas 3 (40.0 MiB)`.
pub fn renderer() -> String {
    let [t, s, a] = totals();
    format!("textures {} ({}), surfaces {} ({}), arenas {} ({})", t.0, mib(t.1), s.0, mib(s.1), a.0, mib(a.1))
}

/// The out-of-memory line: `memory: out of memory in WHAT (N bytes asked); renderer: ...;
/// process: ...`. `asked` 0: not known.
pub fn report(what: &str, asked: u64) -> String {
    let asked = if asked != 0 { format!(" ({asked} bytes asked)") } else { String::new() };
    let process = process();
    let process = if process.is_empty() { String::new() } else { format!("; process: {process}") };
    format!("memory: out of memory in {what}{asked}; renderer: {}{process}", renderer())
}

/// Failures logged so far (`failed`).
static FAILURES: AtomicU64 = AtomicU64::new(0);

/// An allocation that failed: `report`'s line for the first 3 of a run, then for every 100th (a
/// shop that streams textures can fail many in a row).
pub fn failed(what: &str, asked: u64) {
    let n = FAILURES.fetch_add(1, Relaxed);
    if n < 3 || n % 100 == 0 {
        crate::host::log(&report(what, asked));
    }
}

/// `dhdr_memory_report` (`dhd_render_ext.h`): `report`'s line in the log, for C's failures (the
/// native renderer's CPU arrays, `port/gs/native_core.inc` native_oom). `what` NULL: "an allocation".
///
/// # Safety
/// `what` is NULL or a NUL-terminated string alive for the call.
#[no_mangle]
pub unsafe extern "C" fn dhdr_memory_report(what: *const std::ffi::c_char, asked: u64) {
    let what = if what.is_null() {
        std::borrow::Cow::Borrowed("an allocation")
    } else {
        // SAFETY: the caller's contract.
        unsafe { std::ffi::CStr::from_ptr(what) }.to_string_lossy()
    };
    crate::host::log(&report(&what, asked));
}

/// The process's memory as the OS tells it cheaply; empty where it isn't asked.
#[cfg(target_vendor = "apple")]
fn process() -> String {
    let mut out = Vec::new();
    if let Some(vm) = apple::vm_info() {
        out.push(format!("footprint {}", mib(vm.footprint)));
        if let Some(left) = apple::available() {
            out.push(format!("iOS allows {} more", mib(left)));
        }
        let (free, largest, ranges) = apple::free_space(vm.min_address, vm.max_address);
        out.push(format!("address space [{:#x}, {:#x}): {} free in {ranges} ranges, the largest {}",
                         vm.min_address, vm.max_address, mib(free), mib(largest)));
    }
    out.join(", ")
}

#[cfg(not(target_vendor = "apple"))]
fn process() -> String {
    String::new()
}

/// Mach's task and region queries (the same ones `port/host64/guest_memory_posix.c` makes).
#[cfg(target_vendor = "apple")]
mod apple {
    type KernReturn = i32;
    type MachPort = u32;
    extern "C" {
        static mach_task_self_: MachPort;
        fn task_info(task: MachPort, flavor: i32, info: *mut i32, count: *mut u32) -> KernReturn;
        fn mach_vm_region(task: MachPort, address: *mut u64, size: *mut u64, flavor: i32, info: *mut i32,
                          count: *mut u32, object: *mut MachPort) -> KernReturn;
        fn mach_port_deallocate(task: MachPort, name: MachPort) -> KernReturn;
    }
    #[cfg(target_os = "ios")]
    extern "C" {
        fn os_proc_available_memory() -> usize;
    }

    const TASK_VM_INFO: i32 = 22;
    /// task_vm_info_data_t's 32-bit words up to max_address (TASK_VM_INFO_REV2_COUNT).
    const REV2_WORDS: u32 = 42;
    const VM_REGION_BASIC_INFO_64: i32 = 9;
    const VM_REGION_BASIC_INFO_COUNT_64: u32 = 9;

    pub struct VmInfo {
        pub footprint: u64,
        pub min_address: u64,
        pub max_address: u64,
    }

    fn word64(words: &[i32], byte: usize) -> u64 {
        let (lo, hi) = (words[byte / 4] as u32 as u64, words[byte / 4 + 1] as u32 as u64);
        lo | hi << 32
    }

    /// TASK_VM_INFO rev 2: phys_footprint (byte 144), min_address (152), max_address (160).
    pub fn vm_info() -> Option<VmInfo> {
        let mut words = [0i32; 128];
        let mut count = words.len() as u32;
        // SAFETY: a buffer of `count` words; the kernel writes at most that many and sets count.
        let r = unsafe { task_info(mach_task_self_, TASK_VM_INFO, words.as_mut_ptr(), &mut count) };
        if r != 0 || count < REV2_WORDS {
            return None;
        }
        Some(VmInfo { footprint: word64(&words, 144), min_address: word64(&words, 152), max_address: word64(&words, 160) })
    }

    /// What iOS still lets the app take before it ends it (iOS 13 and later; None elsewhere or 0).
    pub fn available() -> Option<u64> {
        #[cfg(target_os = "ios")]
        {
            // SAFETY: no arguments; libSystem's function.
            let left = unsafe { os_proc_available_memory() } as u64;
            if left != 0 {
                return Some(left);
            }
        }
        None
    }

    /// The gaps between the task's regions in [bottom, top): (free bytes, the largest gap, gaps).
    pub fn free_space(bottom: u64, top: u64) -> (u64, u64, u32) {
        let (mut free, mut largest, mut gaps) = (0u64, 0u64, 0u32);
        let mut gap = |lo: u64, hi: u64| {
            if hi > lo {
                free += hi - lo;
                largest = largest.max(hi - lo);
                gaps += 1;
            }
        };
        let mut prev = bottom.max(0x4000);
        let mut at = 0u64;
        loop {
            let (mut a, mut size) = (at, 0u64);
            let mut info = [0i32; VM_REGION_BASIC_INFO_COUNT_64 as usize];
            let mut count = VM_REGION_BASIC_INFO_COUNT_64;
            let mut object: MachPort = 0;
            // SAFETY: out-parameters of the documented sizes.
            let r = unsafe {
                mach_vm_region(mach_task_self_, &mut a, &mut size, VM_REGION_BASIC_INFO_64, info.as_mut_ptr(),
                               &mut count, &mut object)
            };
            if r != 0 {
                break;
            }
            if object != 0 {
                // SAFETY: a send right the call gave us.
                unsafe { mach_port_deallocate(mach_task_self_, object) };
            }
            if a >= top {
                break;
            }
            gap(prev, a);
            prev = prev.max(a + size);
            if a + size <= at {
                break;
            }
            at = a + size;
        }
        gap(prev, top);
        (free, largest, gaps)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn held_counts_and_report() {
        let before = totals()[Part::Textures as usize];
        let a = Held::new(Part::Textures, 4096);
        let b = Held::new(Part::Textures, 1 << 20);
        let mid = totals()[Part::Textures as usize];
        assert_eq!(mid.0, before.0 + 2);
        assert_eq!(mid.1, before.1 + 4096 + (1 << 20));
        drop((a, b));
        assert_eq!(totals()[Part::Textures as usize], before);
        set(Part::Arenas, 3, 40 << 20);
        let line = report("native texture", 262144);
        assert!(line.starts_with("memory: out of memory in native texture (262144 bytes asked); renderer: textures "), "{line}");
        assert!(line.contains("arenas 3 (40.0 MiB)"), "{line}");
        #[cfg(target_vendor = "apple")]
        assert!(line.contains("; process: footprint ") && line.contains(" free in "), "{line}");
    }
}
