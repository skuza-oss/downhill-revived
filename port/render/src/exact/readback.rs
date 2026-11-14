//! Reads: `read_begin` / `read_finish` over READBACK or STAGED (task A3; the wasm path is B1's).
//!
//! begin submits, then maps the buffer over the span from the first to the last wanted page. It
//! only maps, never copies: C has put the pages there through `copy` (right before for READBACK,
//! earlier for STAGED). Natively it blocks until mapped (`map_async` + `device.poll(Wait)`); in the
//! browser it returns at once and the map's promise waits in `web/wait.rs`'s slot. finish copies
//! every wanted page to `image[page * 8192..]`, leaves the others, and unmaps. 0 from begin = could
//! not start; 0 from finish = the map failed (both logged here, through `host::log`).
//! `ffi.rs` keeps the object held between a begin that returned 1 and its finish (`Owned`).
//!
//! As the retired `gpu_wgpu.c`'s op_read and `gpu.js`'s web_gpu_read (last at render/rust
//! 3882d5576): a `want` with no page set maps nothing
//! and copies nothing, and both halves succeed. A map that fails natively makes begin return 1 and
//! finish 0 (the browser can only learn it at finish, so both paths report it there); C then
//! exits (`gpu_rust.c`'s op_read; the browser's op_read traps).
//!
//! Referee: optrace_replay (A3).
use crate::abi::*;
#[cfg(not(target_arch = "wasm32"))]
use crate::device::Gpu;
use crate::exact::Exact;
use std::sync::{Arc, Mutex};

/// What a map's callback reports: None until it runs.
pub type MapStatus = Arc<Mutex<Option<Result<(), wgpu::BufferAsyncError>>>>;

/// The read in flight between begin and finish, if any.
pub struct Reading {
    /// `(buffer, first page, last page + 1, the map's status)`; an empty span maps nothing.
    pub flight: Option<(u32, u32, u32, MapStatus)>,
    /// When begin started (natively), for `DhdrExactStats::read_ticks` (nanoseconds).
    #[cfg(not(target_arch = "wasm32"))]
    pub started: Option<std::time::Instant>,
}

impl Reading {
    pub fn new() -> Reading {
        Reading {
            flight: None,
            #[cfg(not(target_arch = "wasm32"))]
            started: None,
        }
    }
}

/// The first wanted page and one past the last (`first >= last`: none wanted).
pub fn span(want: &[u32; 16]) -> (u32, u32) {
    let (mut first, mut last) = (DHDR_PAGES, 0);
    for page in 0..DHDR_PAGES {
        if wanted(want, page) {
            first = first.min(page);
            last = page + 1;
        }
    }
    (first, last)
}

/// Page `page` is set in `want` (bit `page & 31` of word `page >> 5`).
pub fn wanted(want: &[u32; 16], page: u32) -> bool {
    want[(page >> 5) as usize] >> (page & 31) & 1 != 0
}

/// Starts mapping `range` of `buffer` for reading; the status fills in when the callback runs.
/// In the browser (B1) the map goes through `web::wait::map_read`, which opens the pending
/// promise `dhdr_web_pending()` hands to the reactor's `web_gpu_await()` and settles it right
/// after this status is set.
pub fn map_start(buffer: &wgpu::Buffer, range: std::ops::Range<u64>) -> MapStatus {
    let status: MapStatus = Arc::new(Mutex::new(None));
    let s = status.clone();
    let on_mapped = move |r| {
        *s.lock().unwrap_or_else(|p| p.into_inner()) = Some(r);
    };
    #[cfg(target_arch = "wasm32")]
    crate::web::wait::map_read(buffer, range, on_mapped);
    #[cfg(not(target_arch = "wasm32"))]
    buffer.map_async(wgpu::MapMode::Read, range, on_mapped);
    status
}

/// Natively: waits until every submitted command has finished, so a started map has resolved
/// (its callback runs inside the poll). Err when the poll fails.
#[cfg(not(target_arch = "wasm32"))]
pub fn wait(gpu: &Gpu) -> Result<(), String> {
    let _spike = crate::native::pipelines::SpikeTimer::new("readback", 0);
    let _startup = crate::startup::step("exact readback fence wait");
    gpu.device.poll(wgpu::PollType::wait_indefinitely()).map(|_| ()).map_err(|e| format!("poll: {e}"))
}

/// Natively: maps `range` of `buffer` for reading and blocks until it is mapped (the display
/// capture's path too). Err when the map or the poll fails; the buffer is then not mapped.
#[cfg(not(target_arch = "wasm32"))]
pub fn map_blocking(gpu: &Gpu, buffer: &wgpu::Buffer, range: std::ops::Range<u64>) -> Result<(), String> {
    let status = map_start(buffer, range);
    wait(gpu)?;
    let r = status.lock().unwrap_or_else(|p| p.into_inner()).take();
    match r {
        Some(Ok(())) => Ok(()),
        Some(Err(e)) => Err(format!("map failed: {e}")),
        None => Err("the map's callback did not run".into()),
    }
}

impl Exact {
    /// `dhdr_exact_read_begin`: 1 started (the object stays held until finish), 0 could not start.
    pub fn read_begin(&mut self, buffer: u32, want: &[u32; 16]) -> i32 {
        if buffer != DHDR_B_READBACK && buffer != DHDR_B_STAGED {
            crate::host::log(&format!("dhd_render: dhdr_exact_read_begin: buffer {buffer} is not READBACK or STAGED"));
            return 0;
        }
        if self.reading.flight.is_some() {
            crate::host::log("dhd_render: dhdr_exact_read_begin: a read is already in flight");
            return 0;
        }
        // In the browser a lost device would leave the reactor awaiting a map that never comes:
        // refuse instead (B1; the spike's device-loss path).
        #[cfg(target_arch = "wasm32")]
        if crate::web::wait::lost() {
            crate::host::log("dhd_render: dhdr_exact_read_begin: the WebGPU device is lost");
            return 0;
        }
        #[cfg(not(target_arch = "wasm32"))]
        {
            self.reading.started = Some(std::time::Instant::now());
        }
        let (first, last) = span(want);
        let status: MapStatus = Arc::new(Mutex::new(Some(Ok(()))));
        if first >= last {
            self.reading.flight = Some((buffer, first, last, status));
            return 1;
        }
        // The pages' copies are in the open encoder or already submitted: everything goes first.
        if self.encoder.submit(self.gpu) {
            self.stats.submits += 1;
        }
        let page = DHDR_PAGE_BYTES as u64;
        let status = map_start(self.buffers.get(buffer), first as u64 * page..last as u64 * page);
        #[cfg(not(target_arch = "wasm32"))]
        if let Err(e) = wait(self.gpu) {
            crate::host::log(&format!("dhd_render: dhdr_exact_read_begin: {e}"));
            *status.lock().unwrap_or_else(|p| p.into_inner()) = None;
        }
        // In the browser the map resolves on the event loop; B1's web/wait.rs hands JS the promise.
        self.reading.flight = Some((buffer, first, last, status));
        1
    }

    /// `dhdr_exact_read_finish`: 1 ok, 0 the map failed. `image` is the 4 MiB page image.
    pub fn read_finish(&mut self, buffer: u32, want: &[u32; 16], image: &mut [u8]) -> i32 {
        let Some((began, first, last, status)) = self.reading.flight.take() else {
            crate::host::log("dhd_render: dhdr_exact_read_finish: no read in flight");
            return 0;
        };
        let b = self.buffers.get(began);
        let result = status.lock().unwrap_or_else(|p| p.into_inner()).take();
        // Mapped, or a map still pending (unmap cancels it); a failed map left nothing to unmap.
        let mapped = first < last && !matches!(result, Some(Err(_)));
        let fail = |why: &str| {
            crate::host::log(&format!("dhd_render: dhdr_exact_read_finish: {why}"));
            if mapped {
                b.unmap();
            }
            0
        };
        if buffer != began || span(want) != (first, last) {
            return fail(&format!("buffer {buffer} or want differs from the begin's (buffer {began})"));
        }
        let page = DHDR_PAGE_BYTES as usize;
        if image.len() < DHDR_PAGES as usize * page {
            return fail("the image is smaller than 512 pages");
        }
        match result {
            Some(Ok(())) => {}
            Some(Err(e)) => return fail(&format!("map failed: {e}")),
            None => return fail("the map did not complete"),
        }
        if mapped {
            {
                let view = b.get_mapped_range(first as u64 * page as u64..last as u64 * page as u64);
                for p in first..last {
                    if wanted(want, p) {
                        let from = (p - first) as usize * page;
                        let to = p as usize * page;
                        // the browser copies from the mapped ArrayBuffer (web::wait::copy_mapped)
                        #[cfg(target_arch = "wasm32")]
                        crate::web::wait::copy_mapped(&view, from, &mut image[to..to + page]);
                        #[cfg(not(target_arch = "wasm32"))]
                        image[to..to + page].copy_from_slice(&view[from..from + page]);
                    }
                }
            } // the view drops before the unmap
            b.unmap();
        }
        self.stats.reads += 1;
        #[cfg(not(target_arch = "wasm32"))]
        if let Some(t) = self.reading.started.take() {
            self.stats.read_ticks += t.elapsed().as_nanos() as u64;
        }
        1
    }
}
