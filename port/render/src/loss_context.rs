//! Issue 91: CPU call attribution after startup diagnostics retire. No GPU waits or recovery.
#[derive(Clone, Copy)]
pub enum Call { Submit = 0, Acquire = 1, Present = 2 }

#[cfg(any(target_os = "android", test))]
mod tracking {
    use super::Call;
    use std::{cell::Cell, sync::atomic::{AtomicBool, AtomicU64, Ordering}};
    pub fn affected(backend: wgpu::Backend, gpu: &str, driver: &str) -> bool {
        backend == wgpu::Backend::Vulkan
            && matches!(gpu, "Adreno (TM) 505" | "Adreno 505")
            && driver.contains("9d44e36") && driver.contains("Ie3bb699d95")
            && driver.contains("EV031.27.05.02") && driver.contains("09/29/20")
    }
    pub static ENABLED: AtomicBool = AtomicBool::new(false);
    pub struct Counts {
        started: [AtomicU64; 3],
        returned: [AtomicU64; 3],
    }
    impl Counts {
        pub const fn new() -> Self {
            Self { started: [const { AtomicU64::new(0) }; 3], returned: [const { AtomicU64::new(0) }; 3] }
        }
        pub fn enter(&self, call: Call) { self.started[call as usize].fetch_add(1, Ordering::Relaxed); }
        pub fn leave(&self, call: Call) { self.returned[call as usize].fetch_add(1, Ordering::Relaxed); }
        pub fn text(&self) -> String {
            ["submit", "acquire", "present"].iter().enumerate().map(|(i, name)| format!(
                "{name}={}/{}", self.started[i].load(Ordering::Relaxed), self.returned[i].load(Ordering::Relaxed)
            )).collect::<Vec<_>>().join(" ")
        }
    }
    pub static COUNTS: Counts = Counts::new();
    thread_local! { pub static CURRENT: Cell<&'static str> = const { Cell::new("untracked/asynchronous") }; }
    pub fn name(call: Call) -> &'static str {
        match call { Call::Submit => "queue.submit", Call::Acquire => "surface.get_current_texture", Call::Present => "surface.present" }
    }
}

pub struct Operation {
    #[cfg(any(target_os = "android", test))]
    active: Option<(Call, &'static str)>,
}
#[inline]
pub fn operation(call: Call) -> Operation {
    #[cfg(any(target_os = "android", test))]
    {
        use std::sync::atomic::Ordering;
        let active = tracking::ENABLED.load(Ordering::Relaxed).then(|| {
            tracking::COUNTS.enter(call);
            (call, tracking::CURRENT.with(|c| c.replace(tracking::name(call))))
        });
        Operation { active }
    }
    #[cfg(not(any(target_os = "android", test)))]
    { let _ = call; Operation {} }
}
impl Drop for Operation {
    fn drop(&mut self) {
        #[cfg(any(target_os = "android", test))]
        if let Some((call, outer)) = self.active {
            tracking::CURRENT.with(|c| c.set(outer));
            tracking::COUNTS.leave(call);
        }
    }
}

pub fn init(info: &wgpu::AdapterInfo) {
    #[cfg(target_os = "android")]
    if tracking::affected(info.backend, &info.name, &info.driver_info) {
        tracking::ENABLED.store(true, std::sync::atomic::Ordering::Relaxed);
        crate::host::log("gs: Adreno 505 loss diagnostics enabled: CPU submit/acquire/present calls (not GPU completion)");
    }
    #[cfg(not(target_os = "android"))]
    let _ = info;
}

pub fn report() {
    #[cfg(target_os = "android")]
    if tracking::ENABLED.load(std::sync::atomic::Ordering::Relaxed) {
        crate::host::log(&format!("gs: loss context: CPU call={} export={} thread={:?}; started/returned {} (not GPU completion or proof of the faulting draw)",
            tracking::CURRENT.with(|c| c.get()), crate::guard::current_export(), std::thread::current().id(), tracking::COUNTS.text()));
    }
}

#[cfg(test)]
mod tests {
    use super::{tracking::*, Call};
    #[test]
    fn only_reported_driver() {
        let driver = "Driver Build: 9d44e36, Ie3bb699d95, 1601381439\nDate: 09/29/20\nCompiler Version: EV031.27.05.02";
        assert!(affected(wgpu::Backend::Vulkan, "Adreno (TM) 505", driver));
        for (backend, gpu, driver) in [(wgpu::Backend::Vulkan, "Adreno (TM) 506", driver),
            (wgpu::Backend::Gl, "Adreno (TM) 505", driver), (wgpu::Backend::Vulkan, "Adreno (TM) 505", "new driver")] {
            assert!(!affected(backend, gpu, driver));
        }
        for marker in ["9d44e36", "Ie3bb699d95", "09/29/20", "EV031.27.05.02"] {
            assert!(!affected(wgpu::Backend::Vulkan, "Adreno (TM) 505", &driver.replace(marker, "other")));
        }
    }
    #[test]
    fn cpu_counts_do_not_claim_gpu_completion() {
        let counts = Counts::new();
        counts.enter(Call::Submit);
        counts.leave(Call::Submit);
        counts.enter(Call::Acquire);
        assert_eq!(counts.text(), "submit=1/1 acquire=1/0 present=0/0");
        counts.leave(Call::Acquire);
        assert_eq!(name(Call::Present), "surface.present");
    }
    #[test]
    fn scopes_restore_the_call_and_inactive_calls_are_not_counted() {
        use std::sync::atomic::Ordering;
        let before = COUNTS.text();
        drop(super::operation(Call::Present));
        assert_eq!(COUNTS.text(), before);
        ENABLED.store(true, Ordering::Relaxed);
        {
            let _submit = super::operation(Call::Submit);
            assert_eq!(CURRENT.with(|c| c.get()), "queue.submit");
            {
                let _acquire = super::operation(Call::Acquire);
                assert_eq!(CURRENT.with(|c| c.get()), "surface.get_current_texture");
                assert_eq!(COUNTS.text(), "submit=1/0 acquire=1/0 present=0/0");
            }
            assert_eq!(CURRENT.with(|c| c.get()), "queue.submit");
        }
        ENABLED.store(false, Ordering::Relaxed);
        assert_eq!(CURRENT.with(|c| c.get()), "untracked/asynchronous");
        assert_eq!(COUNTS.text(), "submit=1/1 acquire=1/1 present=0/0");
    }
}
