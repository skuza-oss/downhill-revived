//! Android startup breadcrumbs only, retired after three presents (finding 231).
//! A timeout is evidence of a slow/unreturned call, not proof of a permanent GPU hang.
#[cfg(target_os = "android")]
pub use android::{note, presented, start, step, Step};

#[cfg(target_os = "android")]
#[no_mangle]
pub extern "C" fn dhdr_startup_end(code: i32) {
    android::ended(code);
}

/// Android's host passes its absolute files folder before renderer startup.
/// The NUL-terminated UTF-8 string is copied; the host keeps ownership of the original.
#[cfg(target_os = "android")]
#[no_mangle]
pub unsafe extern "C" fn dhdr_startup_files(directory: *const std::ffi::c_char) {
    if directory.is_null() {
        return;
    }
    // SAFETY: the host passes a live NUL-terminated path for this call.
    if let Ok(directory) = unsafe { std::ffi::CStr::from_ptr(directory) }.to_str() {
        android::files(std::path::Path::new(directory));
    }
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
    use std::{
        io::{self, Write},
        path::{Path, PathBuf},
        time::{Duration, Instant},
    };
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
        directory: PathBuf,
        write_failed: bool,
    }
    impl Record {
        pub fn new(directory: &Path) -> Self {
            Self {
                calls: Vec::new(),
                waiting: Instant::now(),
                pictures: 0,
                serial: 0,
                notices: 0,
                directory: directory.to_owned(),
                write_failed: false,
            }
        }
        fn paths(&self) -> io::Result<(PathBuf, PathBuf)> {
            if !self.directory.is_absolute() {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    "the host did not provide an absolute startup files folder",
                ));
            }
            Ok((
                self.directory.join("dhd-startup.txt"),
                self.directory.join("dhd-startup.tmp"),
            ))
        }
        pub fn previous(&self) -> Option<String> {
            let (file, _) = self.paths().ok()?;
            let text = std::fs::read_to_string(file).ok()?;
            // Ignore terminal markers left by an older version too.
            (text.starts_with("died at ") || text.starts_with("hung at ")).then_some(text)
        }
        fn report(&mut self, result: io::Result<()>, mut log: impl FnMut(&str)) {
            if let Err(e) = result {
                if !self.write_failed {
                    self.write_failed = true;
                    log(&format!("gs startup: record write failed: {e}"));
                }
            }
        }
        pub fn save(&mut self, text: &str, log: impl FnMut(&str)) {
            let result = (|| -> io::Result<()> {
                let (file, temporary) = self.paths()?;
                let mut f = std::fs::File::create(&temporary)?;
                f.write_all(text.as_bytes())?;
                f.sync_all()?;
                // Close before replacing, also exercised by the Windows unit tests.
                drop(f);
                std::fs::rename(temporary, file)
            })();
            self.report(result, log);
        }
        pub fn save_stage(&mut self, log: impl FnMut(&str)) {
            self.save(&self.text(Instant::now()), log);
        }
        pub fn retire(&mut self, log: impl FnMut(&str)) {
            let result = self
                .paths()
                .and_then(|(file, _)| match std::fs::remove_file(file) {
                    Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(()),
                    result => result,
                });
            self.report(result, log);
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
        let mut r = Record::new(Path::new(""));
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
    #[cfg(test)]
    fn test_directory(name: &str) -> PathBuf {
        let base = std::env::var_os("DHD_SCRATCH")
            .map(PathBuf::from)
            .unwrap_or_else(std::env::temp_dir);
        let dir = base.join(format!(
            "dhd-startup-{name}-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        assert!(dir.is_absolute());
        std::fs::create_dir(&dir).unwrap();
        dir
    }
    #[test]
    fn absolute_folder_and_record_lifecycle() {
        let dir = test_directory("path with spaces");
        assert_ne!(dir, std::env::current_dir().unwrap());
        let mut r = Record::new(&dir);
        let (file, temporary) = r.paths().unwrap();
        assert_eq!(file.parent(), Some(dir.as_path()));
        assert_eq!(temporary.parent(), Some(dir.as_path()));
        let mut errors = Vec::new();
        r.calls.push(Call {
            id: 10,
            name: "pipeline test".into(),
            since: Instant::now() - SLOW,
        });
        r.save_stage(|s| errors.push(s.to_owned()));
        assert!(errors.is_empty());
        assert!(file.is_file());
        assert!(!temporary.exists());
        // Dropping an interrupted run leaves its stage for the next process.
        drop(r);
        let mut next = Record::new(&dir);
        assert_eq!(
            next.previous().as_deref(),
            Some("hung at pipeline test [call 10]")
        );
        // Old terminal markers are not diagnoses of an unexpected exit.
        for done in [
            "complete: three presents returned",
            "ended by host during startup (code 0)",
        ] {
            next.save(done, |s| errors.push(s.to_owned()));
            assert!(next.previous().is_none());
        }
        next.save("died at next pipeline", |s| errors.push(s.to_owned()));
        assert!(Record::new(&dir).previous().is_some());
        next.retire(|s| errors.push(s.to_owned()));
        assert!(!file.exists());
        assert!(Record::new(&dir).previous().is_none());
        // Normal exit after retirement must remain harmless with no record.
        next.retire(|s| errors.push(s.to_owned()));
        assert!(errors.is_empty());
        std::fs::remove_dir(dir).unwrap();
    }
    #[test]
    fn record_write_failure_logged_once_and_can_recover() {
        let dir = test_directory("failure");
        let missing = dir.join("missing");
        let mut r = Record::new(&missing);
        let mut errors = Vec::new();
        for _ in 0..3 {
            r.save("died at pipeline", |s| errors.push(s.to_owned()));
        }
        assert_eq!(errors.len(), 1);
        assert!(errors[0].starts_with("gs startup: record write failed: "));
        std::fs::create_dir(&missing).unwrap();
        r.save("died at pipeline", |s| errors.push(s.to_owned()));
        assert_eq!(r.previous().as_deref(), Some("died at pipeline"));
        r.retire(|s| errors.push(s.to_owned()));
        std::fs::remove_dir(&missing).unwrap();
        r.save("died at pipeline", |s| errors.push(s.to_owned()));
        assert_eq!(errors.len(), 1);
        std::fs::remove_dir(dir).unwrap();
    }
    #[test]
    fn relative_folder_is_rejected_without_a_cwd_fallback() {
        let mut r = Record::new(Path::new("relative-files"));
        let mut errors = Vec::new();
        r.save("died at pipeline", |s| errors.push(s.to_owned()));
        r.save("died at pipeline", |s| errors.push(s.to_owned()));
        assert_eq!(errors.len(), 1);
        assert!(errors[0].contains("absolute startup files folder"));
        assert!(r.previous().is_none());
    }
}

#[cfg(target_os = "android")]
mod android {
    use super::record::{Call, Record, SLOW};
    use std::{
        sync::{
            atomic::{AtomicBool, Ordering},
            Mutex, OnceLock,
        },
        time::Instant,
    };
    static ACTIVE: AtomicBool = AtomicBool::new(false);
    static STATE: OnceLock<Mutex<Record>> = OnceLock::new();
    static STARTED: std::sync::Once = std::sync::Once::new();

    pub fn files(directory: &std::path::Path) {
        // The host's existing files folder, copied once into the existing startup state.
        STATE.get_or_init(|| Mutex::new(Record::new(directory)));
    }
    pub fn ended(_code: i32) {
        if let Some(state) = STATE.get() {
            if let Ok(mut s) = state.try_lock() {
                if ACTIVE.swap(false, Ordering::Relaxed) {
                    // A known host shutdown is not an interrupted startup.
                    s.retire(crate::host::log);
                }
            }
        }
    }
    pub fn start() {
        STARTED.call_once(|| {
            // No relative-path fallback if a host ever misses the path handoff.
            let state = STATE.get_or_init(|| Mutex::new(Record::new(std::path::Path::new(""))));
            let mut s = state.lock().unwrap();
            if let Some(previous) = s.previous() {
                crate::host::log(&format!(
                    "previous run: startup: {} (hung means >=5s observed; died means unexpected exit, including a kill)",
                    previous.trim()));
            }
            s.waiting = Instant::now();
            s.save("died at renderer startup", crate::host::log);
            drop(s);
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
            let mut s = STATE.get().unwrap().lock().unwrap();
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
                s.save(&text, crate::host::log);
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
        s.save_stage(crate::host::log);
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
            s.save_stage(crate::host::log);
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
            s.retire(crate::host::log);
        } else {
            s.save_stage(crate::host::log);
        }
    }
}
