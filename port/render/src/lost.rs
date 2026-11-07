//! A lost device (finding 171): the driver reset or removed the GPU device (on D3D12
//! DXGI_ERROR_DEVICE_REMOVED after a GPU timeout, a driver crash or update; on Vulkan
//! VK_ERROR_DEVICE_LOST; on Metal a GPU restart). wgpu then turns every call on the device into a
//! silent no-op (WebGPU's rule: no errors from a lost device), so the game ran on with sound and
//! input and a frozen picture (issue #58).
//!
//! Nothing is made again: the device is the process's one (`device.rs`), every renderer object
//! borrows it for the process's life, and the exact renderer's GS memory, which answers the game's
//! Z probe, lives on it. A device lost the way #58's was (a GPU hang at the first 3D picture, every
//! start) would also hang again on the same work. So the C side ends the game with a message
//! (`port/gs/present.c`, graphics_lost).
//!
//! - `watch`: wgpu's device-lost callback on the native device. wgpu calls it inside the failing
//!   call (any thread, its own locks held), so it only logs wgpu's message and sets the flag.
//! - `lost`: whether it happened; a surface's acquire fails at once then (`present/surface.rs`).
//! - `dhdr_device_lost`: C's question after a present failed. The first yes logs why, once: on
//!   D3D12 the device's removed reason (GetDeviceRemovedReason) and, with `DHD_GPU_DRED=1`, DRED's
//!   auto-breadcrumbs (each command list in flight and how far the GPU got) and its page fault.
//! - `DHD_TEST_DEVICE_LOST=N` (tests only): at the N-th frame a surface acquires, the D3D12 device
//!   is removed (`ID3D12Device5::RemoveDevice`): this process's device only, no GPU reset and no
//!   other program touched. A forced TDR (`dxcap -forcetdr`) would reset the GPU under every
//!   program on it, so the tests never use one. Elsewhere than D3D12 it only logs that it can't.
//!
//! The browser keeps its own callback (`web/init.rs`); there `lost` stays false.
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::OnceLock;

static LOST: AtomicBool = AtomicBool::new(false);
static REPORTED: AtomicBool = AtomicBool::new(false);

/// Whether the device was lost (any thread).
pub fn lost() -> bool {
    LOST.load(Ordering::Acquire)
}

/// Notes a loss: wgpu's reason and message in the log, and the flag. `Destroyed` (the device's own
/// `destroy`, never called by the game) is not a loss.
pub fn note(reason: wgpu::DeviceLostReason, message: &str) {
    if reason == wgpu::DeviceLostReason::Destroyed {
        return;
    }
    if !LOST.swap(true, Ordering::AcqRel) {
        crate::host::log(&format!("gs: the GPU device was lost ({reason:?}): {}",
                                  if message.is_empty() { "no message" } else { message }));
    }
}

/// Installs the device-lost callback (the native device, right after it is made).
pub fn watch(device: &wgpu::Device) {
    device.set_device_lost_callback(|reason, message| note(reason, &message));
}

/// `dhdr_device_lost` (`dhd_render_ext.h`): 1 when the device was lost, else 0. The first 1 logs
/// the backend's reason once.
#[no_mangle]
pub extern "C" fn dhdr_device_lost() -> i32 {
    let _g = crate::guard::Guard::enter("dhdr_device_lost");
    if !lost() {
        return 0;
    }
    if !REPORTED.swap(true, Ordering::AcqRel) {
        if let Some(gpu) = crate::device::gpu() {
            report(gpu);
        }
    }
    1
}

/// The backend's reason in the log.
fn report(gpu: &crate::device::Gpu) {
    #[cfg(all(windows, feature = "native"))]
    if gpu.info.backend == wgpu::Backend::Dx12 {
        d3d12::report(gpu);
        return;
    }
    crate::host::log(&format!("gs: the device was on {:?}, {} (driver {} {})", gpu.info.backend, gpu.info.name,
                              gpu.info.driver, gpu.info.driver_info));
}

/// `DHD_TEST_DEVICE_LOST=N`'s count: called for every frame a surface acquired.
pub fn frame_acquired(gpu: &crate::device::Gpu) {
    static AT: OnceLock<Option<u64>> = OnceLock::new();
    static FRAMES: AtomicU64 = AtomicU64::new(0);
    let at = *AT.get_or_init(|| std::env::var("DHD_TEST_DEVICE_LOST").ok().and_then(|v| v.trim().parse().ok()));
    let Some(at) = at else { return };
    if FRAMES.fetch_add(1, Ordering::Relaxed) + 1 != at {
        return;
    }
    #[cfg(all(windows, feature = "native"))]
    if gpu.info.backend == wgpu::Backend::Dx12 {
        d3d12::remove(gpu);
        return;
    }
    crate::host::log(&format!("test: DHD_TEST_DEVICE_LOST: no device removal on {:?}", gpu.info.backend));
}

/// D3D12's side: DRED before the device is made, the removed reason and DRED's output after.
#[cfg(all(windows, feature = "native"))]
pub mod d3d12 {
    use crate::device::Gpu;
    use windows::core::Interface;
    use windows::Win32::Graphics::Direct3D12::*;

    /// `DHD_GPU_DRED=1`: DRED's auto-breadcrumbs and page fault reports for devices made from now
    /// on (before the instance: wgpu-hal makes the D3D12 device when it enumerates adapters). They cost the GPU a little on every command, so only when
    /// asked: a player whose game loses its device can start it once with them.
    pub fn dred_enable() {
        if std::env::var("DHD_GPU_DRED").map_or(true, |v| v.trim() != "1") {
            return;
        }
        match dred_settings() {
            Ok(s) => {
                // SAFETY: a live settings object; the calls take plain values.
                unsafe {
                    s.SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                    s.SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                }
                crate::host::log("gs: D3D12 DRED on (DHD_GPU_DRED=1): breadcrumbs and page faults");
            }
            Err(why) => crate::host::log(&format!("gs: D3D12 DRED unavailable: {why}")),
        }
    }

    windows_link::link!("kernel32.dll" "system" fn LoadLibraryExW(name: *const u16, file: *mut core::ffi::c_void, flags: u32) -> *mut core::ffi::c_void);
    windows_link::link!("kernel32.dll" "system" fn GetProcAddress(module: *mut core::ffi::c_void, name: *const u8) -> *mut core::ffi::c_void);

    /// DRED's settings through d3d12.dll's `D3D12GetDebugInterface`, looked up only when asked: dhd.exe
    /// imports no d3d12.dll (wgpu loads it itself), so its imports stay Windows' core DLLs that
    /// package_release.py allows.
    fn dred_settings() -> Result<ID3D12DeviceRemovedExtendedDataSettings, String> {
        type GetDebugInterface =
            unsafe extern "system" fn(*const windows::core::GUID, *mut *mut core::ffi::c_void) -> windows::core::HRESULT;
        const LOAD_LIBRARY_SEARCH_SYSTEM32: u32 = 0x800;
        let name: Vec<u16> = "d3d12.dll\0".encode_utf16().collect();
        // SAFETY: a NUL-terminated name; the module stays loaded for the process (never freed).
        let module = unsafe { LoadLibraryExW(name.as_ptr(), std::ptr::null_mut(), LOAD_LIBRARY_SEARCH_SYSTEM32) };
        if module.is_null() {
            return Err("no d3d12.dll".into());
        }
        // SAFETY: a loaded module and a NUL-terminated export name.
        let proc_ = unsafe { GetProcAddress(module, b"D3D12GetDebugInterface\0".as_ptr()) };
        if proc_.is_null() {
            return Err("no D3D12GetDebugInterface".into());
        }
        // SAFETY: the export has this signature (d3d12.h).
        let get: GetDebugInterface = unsafe { std::mem::transmute(proc_) };
        let mut raw = std::ptr::null_mut();
        // SAFETY: the interface's IID and an out pointer for it.
        unsafe { get(&ID3D12DeviceRemovedExtendedDataSettings::IID, &mut raw) }.ok().map_err(|e| format!("{e:?}"))?;
        if raw.is_null() {
            return Err("no settings object".into());
        }
        // SAFETY: a live interface pointer the call handed over with its reference.
        Ok(unsafe { ID3D12DeviceRemovedExtendedDataSettings::from_raw(raw) })
    }

    /// What a DXGI removed reason means.
    fn reason_name(code: u32) -> &'static str {
        match code {
            0 => "S_OK (the device is not removed)",
            0x887A0005 => "DXGI_ERROR_DEVICE_REMOVED: the GPU was removed or its driver was updated or restarted",
            0x887A0006 => "DXGI_ERROR_DEVICE_HUNG: the GPU stopped responding (a timeout; Windows reset the driver)",
            0x887A0007 => "DXGI_ERROR_DEVICE_RESET: the device was reset after a badly formed command",
            0x887A0020 => "DXGI_ERROR_DRIVER_INTERNAL_ERROR: the graphics driver failed",
            0x8007000E => "E_OUTOFMEMORY: out of video memory",
            _ => "another reason",
        }
    }

    /// The breadcrumb operations' names (D3D12_AUTO_BREADCRUMB_OP).
    fn op_name(op: i32) -> &'static str {
        const NAMES: [&str; 21] = ["SetMarker", "BeginEvent", "EndEvent", "DrawInstanced", "DrawIndexedInstanced",
                                   "ExecuteIndirect", "Dispatch", "CopyBufferRegion", "CopyTextureRegion",
                                   "CopyResource", "CopyTiles", "ResolveSubresource", "ClearRenderTargetView",
                                   "ClearUnorderedAccessView", "ClearDepthStencilView", "ResourceBarrier",
                                   "ExecuteBundle", "Present", "ResolveQueryData", "BeginSubmission",
                                   "EndSubmission"];
        match op {
            0..=20 => NAMES[op as usize],
            26 => "WriteBufferImmediate",
            45 => "Barrier",
            46 => "BeginCommandList",
            _ => "other",
        }
    }

    /// A NUL-terminated ANSI name, or "".
    ///
    /// # Safety
    /// `p` is null or a NUL-terminated string alive for the call.
    unsafe fn name(p: *const u8) -> String {
        if p.is_null() {
            return String::new();
        }
        // SAFETY: the caller's contract.
        unsafe { std::ffi::CStr::from_ptr(p.cast()) }.to_string_lossy().into_owned()
    }

    pub fn report(gpu: &Gpu) {
        // SAFETY: the raw device is only read here; wgpu keeps it alive while borrowed.
        let Some(hal) = (unsafe { gpu.device.as_hal::<wgpu_hal::api::Dx12>() }) else { return };
        let raw = hal.raw_device();
        // SAFETY: a live device; GetDeviceRemovedReason takes nothing.
        let code = match unsafe { raw.GetDeviceRemovedReason() } {
            Ok(()) => 0,
            Err(e) => e.code().0 as u32,
        };
        crate::host::log(&format!("gs: D3D12 removed reason 0x{code:08X}: {} ({}, driver {})", reason_name(code),
                                  gpu.info.name, gpu.info.driver));
        let Ok(dred) = raw.cast::<ID3D12DeviceRemovedExtendedData>() else {
            crate::host::log("gs: no DRED on this system");
            return;
        };
        // SAFETY: a live DRED interface; its output lives as long as the device.
        let crumbs = match unsafe { dred.GetAutoBreadcrumbsOutput() } {
            Ok(c) => c,
            Err(e) => {
                crate::host::log(&format!("gs: no DRED breadcrumbs ({e}; start the game with DHD_GPU_DRED=1 for them)"));
                return;
            }
        };
        let mut node = crumbs.pHeadAutoBreadcrumbNode;
        let (mut lists, mut unfinished) = (0u32, 0u32);
        while !node.is_null() && lists < 4096 {
            // SAFETY: DRED's list of nodes, alive with the device.
            let n = unsafe { &*node };
            let done = if n.pLastBreadcrumbValue.is_null() { 0 } else { unsafe { *n.pLastBreadcrumbValue } };
            if done < n.BreadcrumbCount && !n.pCommandHistory.is_null() && unfinished < 16 {
                unfinished += 1;
                // SAFETY: as above; the history holds BreadcrumbCount operations.
                let ops = unsafe { std::slice::from_raw_parts(n.pCommandHistory, n.BreadcrumbCount as usize) };
                let from = done.saturating_sub(2) as usize;
                let to = (done as usize + 3).min(ops.len());
                let around: Vec<String> = (from..to)
                    .map(|i| format!("{}{}", if i == done as usize { ">" } else { "" }, op_name(ops[i].0)))
                    .collect();
                // SAFETY: names are NUL-terminated or null.
                let list = unsafe { name(n.pCommandListDebugNameA) };
                crate::host::log(&format!("gs: DRED: command list \"{list}\" stopped at operation {done} of {}: {}",
                                          n.BreadcrumbCount, around.join(" ")));
            }
            node = n.pNext;
            lists += 1;
        }
        crate::host::log(&format!("gs: DRED: {lists} command lists recorded, {unfinished} of them not finished"));
        // SAFETY: as above.
        if let Ok(fault) = unsafe { dred.GetPageFaultAllocationOutput() } {
            if fault.PageFaultVA != 0 {
                let mut names = Vec::new();
                let mut a = fault.pHeadExistingAllocationNode;
                while !a.is_null() && names.len() < 4 {
                    // SAFETY: DRED's allocation list, alive with the device.
                    let an = unsafe { &*a };
                    names.push(unsafe { name(an.ObjectNameA) });
                    a = an.pNext;
                }
                crate::host::log(&format!("gs: DRED: GPU page fault at 0x{:X} (allocations there: {})",
                                          fault.PageFaultVA, names.join(", ")));
            }
        }
    }

    /// `DHD_TEST_DEVICE_LOST`: removes this process's D3D12 device (ID3D12Device5::RemoveDevice).
    pub fn remove(gpu: &Gpu) {
        // SAFETY: as report.
        let Some(hal) = (unsafe { gpu.device.as_hal::<wgpu_hal::api::Dx12>() }) else { return };
        match hal.raw_device().cast::<ID3D12Device5>() {
            Ok(d5) => {
                crate::host::log("test: DHD_TEST_DEVICE_LOST: removing the D3D12 device (RemoveDevice)");
                // SAFETY: a live device; RemoveDevice marks this device removed, nothing else.
                unsafe { d5.RemoveDevice() };
            }
            Err(e) => crate::host::log(&format!("test: DHD_TEST_DEVICE_LOST: no ID3D12Device5: {e}")),
        }
    }
}
