//! Android startup breadcrumbs only, retired after three presents (finding 231).
//! A timeout is evidence of a slow/unreturned call, not proof of a permanent GPU hang.
#[cfg(target_os = "android")]
pub use android::{note, presented, start, step, Step};

#[cfg(target_os = "android")]
#[no_mangle]
pub extern "C" fn dhdr_startup_end(code: i32) {
    android::ended(code);
}

#[cfg(not(target_os = "android"))]
pub struct Step;
#[cfg(not(target_os = "android"))]
#[inline]
pub fn start() {}
#[cfg(not(target_os = "android"))]
#[inline]
pub fn step(_: &str) -> Step {
    Step
}
#[cfg(not(target_os = "android"))]
#[inline]
pub fn presented() {}
#[cfg(not(target_os = "android"))]
#[inline]
pub fn note(_: &str) {}

/// Every renderer submit, including uploads and overlays. Arguments finish their encoders
/// before this scope starts, so the breadcrumb isolates Queue::submit itself.
#[inline]
pub fn submit(
    queue: &wgpu::Queue,
    commands: impl IntoIterator<Item = wgpu::CommandBuffer>,
) -> wgpu::SubmissionIndex {
    let _startup = step("queue submit");
    queue.submit(commands)
}

#[cfg(any(target_os = "android", test))]
mod record {
    use std::time::{Duration, Instant};
    pub const SLOW: Duration = Duration::from_secs(5);
    pub struct Call {
        pub id: u64,
        pub name: String,
        pub since: Instant,
    }
    pub struct Record {
        pub calls: Vec<Call>,
        pub waiting: Instant,
        pub pictures: u32,
        pub serial: u64,
        pub notices: u32,
    }
    impl Record {
        pub fn new() -> Self {
            Self {
                calls: Vec::new(),
                waiting: Instant::now(),
                pictures: 0,
                serial: 0,
                notices: 0,
            }
        }
        pub fn text(&self, now: Instant) -> String {
            if self.calls.is_empty() {
                return format!(
                    "{} at waiting for picture {} (no active renderer call)",
                    if now.duration_since(self.waiting) >= SLOW {
                        "hung"
                    } else {
                        "died"
                    },
                    self.pictures + 1
                );
            }
            self.calls
                .iter()
                .map(|c| {
                    format!(
                        "{} at {} [call {}]",
                        if now.duration_since(c.since) >= SLOW {
                            "hung"
                        } else {
                            "died"
                        },
                        c.name,
                        c.id
                    )
                })
                .collect::<Vec<_>>()
                .join("; ")
        }
    }
    #[test]
    fn concurrent_calls_and_recovery() {
        let mut r = Record::new();
        assert_eq!((r.serial, r.notices), (0, 0));
        let now = Instant::now();
        r.calls.push(Call {
            id: 1,
            name: "pipeline main".into(),
            since: now,
        });
        r.calls.push(Call {
            id: 2,
            name: "submit".into(),
            since: now + SLOW,
        });
        assert_eq!(
            r.text(now + SLOW),
            "hung at pipeline main [call 1]; died at submit [call 2]"
        );
        r.calls.remove(0);
        assert_eq!(r.text(now + SLOW), "died at submit [call 2]");
        r.calls.clear();
        r.waiting = now + SLOW;
        assert!(r
            .text(now + SLOW)
            .starts_with("died at waiting for picture 1"));
        assert!(r
            .text(now + SLOW + SLOW)
            .starts_with("hung at waiting for picture 1"));
    }
}

#[cfg(target_os = "android")]
mod android {
    use super::record::{Call, Record, SLOW};
    use std::{
        io::Write,
        sync::{
            atomic::{AtomicBool, Ordering},
            Mutex, OnceLock,
        },
        time::Instant,
    };
    static ACTIVE: AtomicBool = AtomicBool::new(false);
    static STATE: OnceLock<Mutex<Record>> = OnceLock::new();
    const FILE: &str = "dhd-startup.txt"; // Android's private files cwd, as dhd.log
    static STARTED: std::sync::Once = std::sync::Once::new();

    fn save(text: &str) {
        let result = (|| -> std::io::Result<()> {
            let mut f = std::fs::File::create("dhd-startup.tmp")?;
            f.write_all(text.as_bytes())?;
            f.sync_all()?;
            std::fs::rename("dhd-startup.tmp", FILE)
        })();
        if let Err(e) = result {
            crate::host::log(&format!("gs startup: record write failed: {e}"));
        }
    }
    pub fn ended(code: i32) {
        if let Some(state) = STATE.get() {
            if let Ok(s) = state.try_lock() {
                if ACTIVE.swap(false, Ordering::Relaxed) {
                    save(&format!(
                        "ended by host during startup (code {code}); last state: {}",
                        s.text(Instant::now())
                    ));
                }
            }
        }
    }
    pub fn start() {
        STARTED.call_once(|| {
            if let Ok(previous) = std::fs::read_to_string(FILE) {
                crate::host::log(&format!(
                    "previous run: startup: {} (hung means >=5s observed; died means unexpected exit, including a kill)",
                    previous.trim()));
            }
            STATE.get_or_init(|| Mutex::new(Record::new()));
            save("died at renderer startup");
            ACTIVE.store(true, Ordering::Relaxed);
            if let Err(e) = std::thread::Builder::new().name("gs-startup".into()).spawn(watch) {
                crate::host::log(&format!("gs startup: watchdog unavailable: {e}"));
            }
        });
    }
    fn watch() {
        let mut warned = Instant::now();
        while ACTIVE.load(Ordering::Relaxed) {
            std::thread::sleep(std::time::Duration::from_secs(1));
            let s = STATE.get().unwrap().lock().unwrap();
            if !ACTIVE.load(Ordering::Relaxed) {
                break;
            }
            let now = Instant::now();
            if warned.elapsed() >= SLOW
                && (s.calls.iter().any(|c| now.duration_since(c.since) >= SLOW)
                    || now.duration_since(s.waiting) >= SLOW)
            {
                warned = now;
                let text = s.text(now);
                save(&text);
                crate::host::log(&format!("gs startup: watchdog: {text}; {} presents returned; waiting {:.1}s for picture {}",
                    s.pictures, s.waiting.elapsed().as_secs_f64(), s.pictures + 1));
            }
        }
    }
    pub struct Step(Option<u64>);
    pub fn step(name: &str) -> Step {
        if !ACTIVE.load(Ordering::Relaxed) {
            return Step(None);
        }
        let mut s = STATE.get().unwrap().lock().unwrap();
        if !ACTIVE.load(Ordering::Relaxed) {
            return Step(None);
        }
        s.serial += 1;
        let id = s.serial;
        let name = format!("{name} ({:?})", std::thread::current().id());
        s.calls.push(Call {
            id,
            name: name.clone(),
            since: Instant::now(),
        });
        save(&s.text(Instant::now()));
        crate::host::log(&format!("gs startup: begin {id}: {name}"));
        Step(Some(id))
    }
    impl Drop for Step {
        fn drop(&mut self) {
            let Some(id) = self.0 else { return };
            let mut s = STATE.get().unwrap().lock().unwrap();
            if !ACTIVE.load(Ordering::Relaxed) {
                return;
            }
            if let Some(at) = s.calls.iter().position(|c| c.id == id) {
                let call = s.calls.remove(at);
                crate::host::log(&format!(
                    "gs startup: returned {id}: {} ({:.3}s)",
                    call.name,
                    call.since.elapsed().as_secs_f64()
                ));
            }
            save(&s.text(Instant::now()));
        }
    }
    pub fn note(text: &str) {
        if !ACTIVE.load(Ordering::Relaxed) {
            return;
        }
        let mut s = STATE.get().unwrap().lock().unwrap();
        if ACTIVE.load(Ordering::Relaxed) && s.notices < 24 {
            s.notices += 1;
            crate::host::log(&format!("gs startup: {text}"));
            if s.notices == 24 {
                crate::host::log("gs startup: acquire detail limit reached; watchdog stays on");
            }
        }
    }
    pub fn presented() {
        if !ACTIVE.load(Ordering::Relaxed) {
            return;
        }
        let mut s = STATE.get().unwrap().lock().unwrap();
        if !ACTIVE.load(Ordering::Relaxed) {
            return;
        }
        s.pictures += 1;
        s.waiting = Instant::now();
        crate::host::log(&format!(
            "gs startup: picture {} present returned (not proof of visible pixels)",
            s.pictures
        ));
        if s.pictures == 3 {
            ACTIVE.store(false, Ordering::Relaxed);
            save("complete: three presents returned");
        } else {
            save(&s.text(Instant::now()));
        }
    }
}
