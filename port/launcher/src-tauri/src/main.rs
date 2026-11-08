//! Downhill Revived launcher (finding 093): the window players see before the game. It writes the
//! game's own settings file (dhd.ini), keeps the game up to date, sets the rider name, and starts
//! the game. The front end is ../ui; everything that touches files, network or processes is here.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod controls;
mod disc;
mod game;
mod log;
mod scale;
mod update;

use serde::Serialize;
use std::collections::BTreeMap;
use std::path::PathBuf;
use std::process::Child;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant};
use tauri::{AppHandle, Emitter, Manager, State};
use tauri_plugin_dialog::DialogExt;
use tauri_plugin_opener::OpenerExt;

const VERSION: &str = env!("DHD_VERSION");
const DISCORD: &str = "https://discord.gg/XrSeTyzfr2";
const KOFI: &str = "https://ko-fi.com/skuza";

struct App {
    place: game::Place,
    game: Mutex<Option<Child>>,
    /// PLAY was pressed and the game's window hasn't taken the focus yet: the launcher stays in
    /// sight while the game starts, and minimizes once it loses the focus.
    starting: AtomicBool,
    /// The newest online check of the disc image (check_disc): an older one still hashing stops.
    disc_check: AtomicU64,
}

#[derive(Serialize)]
struct Snapshot {
    version: &'static str,
    native_window_controls: bool,
    /// Every setting's value: dhd.ini's, else the player's default (dhd_settings).
    settings: BTreeMap<&'static str, String>,
    game_found: bool,
    running: bool,
    /// Why dhd.ini's disc image can't be used, when it names one and the game would refuse it.
    disc_problem: Option<String>,
    /// A texture pack is there: texture_pack's folder, or textures/SCUS-97177/replacements.
    texture_pack: bool,
    /// Seconds played in all (saves/playtime.txt).
    played: u64,
    /// The resolution scale the last run drew at and how far its GPU goes (dhd.log); None when no
    /// run has said yet.
    scale: Option<scale::ScaleReport>,
}

/// A setting as the settings tabs offer it (dhd_settings' table): its default and its choices as
/// [value, label] pairs (none for a text setting).
#[derive(Serialize)]
struct SettingInfo {
    key: &'static str,
    default: &'static str,
    choices: Vec<[&'static str; 2]>,
    /// A text setting's own rule: the rider name's (dhd_settings::name), else none.
    rule: Option<TextRule>,
}

/// What a text field keeps while the player types, and the rule in words for its hint.
#[derive(Serialize)]
struct TextRule {
    letters: &'static str,
    max: usize,
    hint: &'static str,
}

#[derive(Serialize)]
struct ServerStatus {
    /// Public races open now and the riders in them (private rooms aren't listed).
    races: usize,
    riders: u64,
    /// The oldest version the server lets race.
    min_version: String,
}

#[derive(Serialize)]
struct UpdateCheck {
    /// None: the newest release has no build for this platform.
    release: Option<update::Release>,
    newer: bool,
}

#[derive(Clone, Serialize)]
struct Progress {
    done: u64,
    total: u64,
}

/// The online check's result for the disc image at path.
#[derive(Serialize)]
struct OnlineCheck {
    path: String,
    /// The USA disc's game files: online races take them.
    usa: bool,
}

/// The disc image being hashed for the online check.
#[derive(Clone, Serialize)]
struct DiscProgress {
    path: String,
    done: u64,
    total: u64,
}

#[derive(Clone, Serialize)]
struct Exited {
    code: Option<u32>,
    wants_update: bool,
    played: u64,
}

/// Time played (an idea of asio's): seconds in saves/playtime.txt, counted while a game the
/// launcher started runs and saved each minute, so a crash or a killed launcher loses at most one.
fn playtime(place: &game::Place) -> PathBuf {
    place.data.join("saves").join("playtime.txt")
}

fn played(place: &game::Place) -> u64 {
    std::fs::read_to_string(playtime(place)).ok().and_then(|s| s.trim().parse().ok()).unwrap_or(0)
}

fn add_played(place: &game::Place, since: &mut Instant) {
    let seconds = since.elapsed().as_secs();
    if seconds == 0 {
        return;
    }
    *since += Duration::from_secs(seconds);
    let path = playtime(place);
    let _ = std::fs::create_dir_all(path.parent().unwrap());
    let _ = std::fs::write(&path, (played(place) + seconds).to_string());
}

#[tauri::command]
fn snapshot(app: State<App>) -> Snapshot {
    let ini = dhd_settings::read(&app.place.settings()).unwrap_or_default();
    let settings: BTreeMap<_, _> = dhd_settings::SETTINGS.iter().map(|s| (s.key, ini.value(s.key))).collect();
    let iso = ini.written("iso");
    let pack = ini
        .written("texture_pack")
        .map(PathBuf::from)
        .unwrap_or_else(|| app.place.data.join("textures").join("SCUS-97177").join("replacements"));
    Snapshot {
        version: VERSION,
        native_window_controls: cfg!(target_os = "macos"),
        disc_problem: iso.and_then(|p| disc::problem(std::path::Path::new(&p))),
        texture_pack: std::fs::read_dir(pack).is_ok_and(|mut d| d.next().is_some()),
        game_found: app.place.exe().is_file(),
        running: app.game.lock().unwrap().is_some(),
        played: played(&app.place),
        scale: scale::last_run(&app.place.data.join("dhd.log")),
        settings,
    }
}

/// Every setting's default and choices, for the settings tabs.
#[tauri::command]
fn settings_schema() -> Vec<SettingInfo> {
    dhd_settings::SETTINGS
        .iter()
        .map(|s| SettingInfo {
            key: s.key,
            default: s.default,
            choices: s.choices.iter().map(|c| [c.value, c.label]).collect(),
            rule: (s.key == "name").then_some(TextRule {
                letters: dhd_settings::name::LETTERS,
                max: dhd_settings::name::MAX,
                hint: dhd_settings::name::RULE,
            }),
        })
        .collect()
}

/// The rider name saved for what the player typed (dhd_settings::name::normalise; "": none).
#[tauri::command]
fn rider_name(text: String) -> String {
    dhd_settings::name::normalise(&text)
}

/// Keys are the game's (letters, digits, _); a value is one line (dhd_settings::check).
#[tauri::command]
fn save_setting(app: State<App>, key: String, value: String) -> Result<(), String> {
    save(&app.place, &key, &value)
}

/// The Controls tab's bindings (controls.rs): the keys, controller buttons and PS2 buttons.
#[tauri::command]
fn controls_schema() -> controls::Schema {
    controls::schema()
}

/// The keyboard's and both controllers' layouts, as dhd.ini has them.
#[tauri::command]
fn controls_state(app: State<App>) -> controls::State {
    controls::state(&app.place.settings())
}

/// One change to a layout, by the game's rules; saved when it changes something.
#[tauri::command]
fn controls_edit(app: State<App>, device: usize, edit: controls::Request, kind: i32) -> Result<controls::Answer, String> {
    controls::edit(&app.place.settings(), device, &edit, kind, |k, v| save(&app.place, k, v))
}

fn save(place: &game::Place, key: &str, value: &str) -> Result<(), String> {
    match dhd_settings::save(&place.settings(), key, value) {
        Ok(true) => Ok(()),
        Ok(false) => Err("settings are off (DHD_SETTINGS)".into()),
        Err(e) => Err(format!("can't save dhd.ini: {e}")),
    }
}

/// A setting as the game will have it: its DHD_* variable when the environment sets it, else
/// dhd.ini's value, else the default.
fn effective(place: &game::Place, key: &str) -> String {
    dhd_settings::read(&place.settings()).unwrap_or_default().effective(key, |n| std::env::var(n).ok())
}

/// The update check (update::check): dhd.ini's update_url when the player set one, else GitHub,
/// else the online server's backup. A failure is the bottom bar's one line; launcher.log gets the
/// technical errors, and a note when the backup answered.
fn check_for_update(place: &game::Place) -> Result<Option<update::Release>, String> {
    let own = effective(place, "update_url");
    match update::check(Some(own.as_str()), update::platform(place), VERSION) {
        Ok(checked) => {
            if let Some(note) = checked.note {
                log::line(&place.data, &note);
            }
            Ok(checked.release)
        }
        Err(e) => {
            log::line(&place.data, &e.detail);
            Err(e.say)
        }
    }
}

#[tauri::command]
async fn check_update(handle: AppHandle) -> Result<UpdateCheck, String> {
    let h = handle.clone();
    let release = tauri::async_runtime::spawn_blocking(move || check_for_update(&h.state::<App>().place))
        .await
        .map_err(|e| e.to_string())??;
    let newer = release.as_ref().is_some_and(|r| update::newer(&r.version, VERSION));
    Ok(UpdateCheck { release, newer })
}

/// Installs the release, then starts the new launcher and ends this one.
#[tauri::command]
async fn install_update(handle: AppHandle, release: update::Release) -> Result<(), String> {
    let h = handle.clone();
    tauri::async_runtime::spawn_blocking(move || {
        let mut last = 0u64;
        let place = &h.state::<App>().place;
        update::install(&release, place, |done, total| {
            if done - last >= 256 * 1024 || done == total {
                last = done;
                let _ = h.emit("update-progress", Progress { done, total });
            }
        })
        .map_err(|e| {
            log::line(&place.data, &format!("update to {} failed: {}", release.version, e.detail));
            e.say
        })
    })
    .await
    .map_err(|e| e.to_string())??;
    handle.state::<App>().place.relaunch().map_err(|e| format!("updated, but can't restart: {e}"))?;
    handle.exit(0);
    Ok(())
}

#[tauri::command]
fn play(handle: AppHandle, app: State<App>) -> Result<(), String> {
    let mut slot = app.game.lock().unwrap();
    if slot.is_some() {
        return Ok(());
    }
    let child = app.place.start().map_err(|e| format!("can't start {}: {e}", game::GAME_EXE))?;
    *slot = Some(child);
    app.starting.store(true, Ordering::Relaxed);
    let h = handle.clone();
    let mut since = Instant::now();
    std::thread::spawn(move || loop {
        std::thread::sleep(Duration::from_millis(400));
        let app = h.state::<App>();
        if since.elapsed() >= Duration::from_secs(60) {
            add_played(&app.place, &mut since);
        }
        let mut slot = app.game.lock().unwrap();
        let status = match slot.as_mut().map(|c| c.try_wait()) {
            Some(Ok(Some(s))) => Some(s),
            Some(Ok(None)) => continue,
            _ => None,
        };
        *slot = None;
        drop(slot);
        app.starting.store(false, Ordering::Relaxed);
        add_played(&app.place, &mut since);
        let code = status.and_then(game::exit_code);
        let played = played(&app.place);
        let _ = h.emit("game-exited", Exited { code, wants_update: code == Some(game::UPDATE_EXIT_CODE), played });
        if let Some(w) = h.get_webview_window("main") {
            // show() first: on Wayland a minimized window can't be restored by unminimize()
            // alone (from n0buholic, PR #13).
            let _ = w.show();
            let _ = w.unminimize();
            let _ = w.set_focus();
        }
        return;
    });
    Ok(())
}

#[tauri::command]
fn stop_game(app: State<App>) {
    if let Some(c) = app.game.lock().unwrap().as_mut() {
        let _ = c.kill();
    }
}

/// The race server's public lobby list (online/server: GET /v1/lobbies), from dhd.ini's server.
#[tauri::command]
async fn server_status(app: State<'_, App>) -> Result<ServerStatus, String> {
    let server = effective(&app.place, "server");
    let base = server.trim_end_matches('/');
    let url = format!("{}/v1/lobbies", base.replacen("wss://", "https://", 1).replacen("ws://", "http://", 1));
    let body = tauri::async_runtime::spawn_blocking(move || update::get_json(&url))
        .await
        .map_err(|e| e.to_string())??;
    let lobbies = body.get("lobbies").and_then(|l| l.as_array()).cloned().unwrap_or_default();
    Ok(ServerStatus {
        races: lobbies.len(),
        riders: lobbies.iter().filter_map(|l| l.get("players")?.as_u64()).sum(),
        min_version: body.get("minVersion").and_then(|v| v.as_str()).unwrap_or_default().to_string(),
    })
}

/// The game's folder in the file manager (the fix the "the game isn't here" message offers).
#[tauri::command]
fn open_folder(handle: AppHandle) -> Result<(), String> {
    let dir = handle.state::<App>().place.game.clone();
    handle.opener().open_path(dir.to_string_lossy(), None::<&str>).map_err(|e| e.to_string())
}

/// The disc image, chosen in a file dialog, checked as the game checks it and saved as iso; None
/// when the player cancels.
#[tauri::command]
async fn pick_disc(handle: AppHandle) -> Result<Option<String>, String> {
    let picked = handle
        .dialog()
        .file()
        .set_title("Choose your Downhill Domination (USA) disc image")
        .add_filter("Disc images", &["iso"])
        .blocking_pick_file();
    let Some(path) = picked.and_then(|p| p.into_path().ok()) else { return Ok(None) };
    if let Some(problem) = disc::problem(&path) {
        return Err(problem);
    }
    let text = path.to_string_lossy().into_owned();
    save(&handle.state::<App>().place, "iso", &text)?;
    Ok(Some(text))
}

/// The online check of dhd.ini's disc image (finding 098): online races need the USA disc's game
/// files. The answer is iso-check.txt's when it holds this file's, else the files are hashed now on a
/// thread (progress in "disc-progress") and the result written there for the game. A newer check
/// (another disc chosen) stops this one: its answer is an error the page ignores.
#[tauri::command]
async fn check_disc(handle: AppHandle) -> Result<OnlineCheck, String> {
    let app = handle.state::<App>();
    let data = app.place.data.clone();
    let settings = dhd_settings::read(&app.place.settings()).unwrap_or_default();
    let iso = settings.written("iso").filter(|s| !s.is_empty()).ok_or("No disc image is chosen.")?;
    let job = app.disc_check.fetch_add(1, Ordering::Relaxed) + 1;
    let (h, path) = (handle.clone(), iso.clone());
    let usa = tauri::async_runtime::spawn_blocking(move || {
        let mut last = 0u64;
        disc::online_check(&data, std::path::Path::new(&path), |done, total| {
            if done == 0 || done - last >= 16 << 20 || done == total {
                last = done;
                let _ = h.emit("disc-progress", DiscProgress { path: path.clone(), done, total });
            }
            h.state::<App>().disc_check.load(Ordering::Relaxed) == job
        })
    })
    .await
    .map_err(|e| e.to_string())?
    .map_err(|e| match e {
        None => "replaced by a newer check".to_string(),
        Some(e) => format!("The disc image couldn't be read for the online check: {e}"),
    })?;
    Ok(OnlineCheck { path: iso, usa })
}

/// Only the project's own pages open from the launcher.
#[tauri::command]
fn open_link(handle: AppHandle, url: String) -> Result<(), String> {
    let url = match url.as_str() { "discord" => DISCORD.to_string(), "kofi" => KOFI.to_string(), _ => url };
    let allowed = ["https://discord.gg/", "https://ko-fi.com/skuza", "https://github.com/skuza-oss/", "https://downhill-revived.pages.dev/"];
    if !allowed.iter().any(|a| url.starts_with(a)) {
        return Err(format!("not opening {url}"));
    }
    handle.opener().open_url(url, None::<&str>).map_err(|e| e.to_string())
}

/// `--play` (finding 134; a Steam shortcut's launch option, a desktop shortcut): the game starts at
/// once with the saved settings, without the window, and the launcher ends when the game does; play
/// time is counted as from PLAY. The game is started as PLAY starts it (Place::start), so it gets
/// the same data folder, dhd.ini and saves. The window opens instead when the game can't start from
/// here: no game beside the launcher, no disc image chosen yet or one the game would refuse, or the
/// game ended asking for an update (the online menu's Update Now: the window offers it).
/// Updates: per auto_update, a check runs on a thread while the game plays, so the start never
/// waits for the network; with "on" a newer release is installed after the game ends (the next
/// start is the new version), with "ask" the next time the window opens offers it, with "off" no
/// check. Returns the place back when the window should open.
fn play_at_once(place: game::Place) -> Option<game::Place> {
    if !place.exe().is_file() {
        return Some(place);
    }
    // the disc the game will use: DHD_ISO before dhd.ini's iso, as the game takes it
    let iso = Some(effective(&place, "iso")).filter(|s| !s.is_empty());
    if iso.is_none_or(|p| disc::problem(std::path::Path::new(&p)).is_some()) {
        return Some(place);
    }
    let Ok(mut child) = place.start() else { return Some(place) };
    let auto = effective(&place, "auto_update");
    let check = (auto != "off").then(|| {
        let place = place.clone();
        std::thread::spawn(move || check_for_update(&place))
    });
    let mut since = Instant::now();
    let status = loop {
        match child.try_wait() {
            Ok(Some(s)) => break Some(s),
            Ok(None) => {}
            Err(_) => break None,
        }
        std::thread::sleep(Duration::from_millis(400));
        if since.elapsed() >= Duration::from_secs(60) {
            add_played(&place, &mut since);
        }
    };
    add_played(&place, &mut since);
    if status.and_then(game::exit_code) == Some(game::UPDATE_EXIT_CODE) {
        return Some(place);
    }
    // with "on", a newer release found while the game ran installs now; a check that hasn't
    // answered within 15 s is left (the next start checks again): a stalled network must not keep
    // a windowless launcher running after the game, which Steam would show as still playing
    let wait_until = Instant::now() + Duration::from_secs(15);
    let check = check.filter(|c| {
        while !c.is_finished() && Instant::now() < wait_until {
            std::thread::sleep(Duration::from_millis(200));
        }
        c.is_finished()
    });
    if auto == "on" {
        if let Some(Ok(Ok(Some(release)))) = check.map(|c| c.join()) {
            if update::newer(&release.version, VERSION) {
                if let Err(e) = update::install(&release, &place, |_, _| {}) {
                    log::line(&place.data, &format!("update to {} failed: {}", release.version, e.detail));
                }
            }
        }
    }
    None
}

/// SIGTERM and SIGINT (`kill`, a logout, Ctrl+C in a terminal) end the launcher as closing its
/// window does, with 128 + the signal as its exit code (143, 130), as a shell reports a process the
/// signal killed: the same on every Unix, whatever the windowing toolkit does with them. A handler
/// that only writes the signal's number to a pipe, read by a thread that asks the event loop to end
/// (and ends the process 2 s later if it hasn't). Installed once the event loop exists, so no
/// toolkit's own handler replaces it; a signal the launcher was started with ignored (nohup, a
/// shell's background job) stays ignored. A running game is left running, as before. Windows has
/// no such signals for a windowed program.
#[cfg(unix)]
mod quit_signals {
    use std::sync::atomic::{AtomicI32, AtomicU32, Ordering};
    use std::time::Duration;
    use tauri::AppHandle;

    const SIGNALS: [libc::c_int; 2] = [libc::SIGTERM, libc::SIGINT];
    static PIPE_WRITE: AtomicI32 = AtomicI32::new(-1);
    /// The SIGNALS the launcher was started with ignored, by index.
    static IGNORED: AtomicU32 = AtomicU32::new(0);

    /// First thing in main: which signals the starter left ignored.
    pub fn note_start() {
        for (i, signal) in SIGNALS.into_iter().enumerate() {
            let mut old: libc::sigaction = unsafe { std::mem::zeroed() };
            if unsafe { libc::sigaction(signal, std::ptr::null(), &mut old) } == 0 && old.sa_sigaction == libc::SIG_IGN {
                IGNORED.fetch_or(1 << i, Ordering::Relaxed);
            }
        }
    }

    extern "C" fn on_signal(signal: libc::c_int) {
        let byte = signal as u8;
        // write(2) is async-signal-safe; nothing else is done here
        unsafe { libc::write(PIPE_WRITE.load(Ordering::Relaxed), (&byte as *const u8).cast(), 1) };
    }

    pub fn install(handle: AppHandle) {
        let mut fds = [-1; 2];
        if unsafe { libc::pipe(fds.as_mut_ptr()) } != 0 {
            return;
        }
        for fd in fds {
            unsafe { libc::fcntl(fd, libc::F_SETFD, libc::FD_CLOEXEC) };  // the game doesn't get them
        }
        PIPE_WRITE.store(fds[1], Ordering::Relaxed);
        for (i, signal) in SIGNALS.into_iter().enumerate() {
            if IGNORED.load(Ordering::Relaxed) & (1 << i) != 0 {
                continue;
            }
            unsafe {
                let mut action: libc::sigaction = std::mem::zeroed();
                action.sa_sigaction = on_signal as extern "C" fn(libc::c_int) as libc::sighandler_t;
                action.sa_flags = libc::SA_RESTART;
                libc::sigemptyset(&mut action.sa_mask);
                libc::sigaction(signal, &action, std::ptr::null_mut());
            }
        }
        std::thread::spawn(move || {
            let mut byte = 0u8;
            loop {
                match unsafe { libc::read(fds[0], (&mut byte as *mut u8).cast(), 1) } {
                    1 => break,
                    n if n < 0 && std::io::Error::last_os_error().kind() == std::io::ErrorKind::Interrupted => continue,
                    _ => return,
                }
            }
            let code = 128 + byte as i32;
            handle.exit(code);
            std::thread::sleep(Duration::from_secs(2));
            std::process::exit(code);
        });
    }
}

fn main() {
    #[cfg(unix)]
    quit_signals::note_start();
    // Linux defaults for WebKitGTK, only where the player hasn't chosen (from n0buholic, PR #13):
    // on Wayland, GTK can end this undecorated window with "protocol error 71", so it runs
    // through XWayland; the dmabuf renderer shows a blank page on some drivers (NVIDIA).
    #[cfg(target_os = "linux")]
    {
        if std::env::var_os("GDK_BACKEND").is_none() && std::env::var_os("WAYLAND_DISPLAY").is_some() {
            std::env::set_var("GDK_BACKEND", "x11");
        }
        if std::env::var_os("WEBKIT_DISABLE_DMABUF_RENDERER").is_none() {
            std::env::set_var("WEBKIT_DISABLE_DMABUF_RENDERER", "1");
        }
    }
    let place = game::Place::find();
    update::clean_up(&place);
    let _ = dhd_settings::create(&place.settings());
    let place = if std::env::args().skip(1).any(|a| a == "--play") {
        match play_at_once(place) {
            Some(place) => place,
            None => return,
        }
    } else {
        place
    };
    let context = tauri::generate_context!();
    #[cfg(target_os = "macos")]
    let context = {
        let mut context = context;
        let window = context.config_mut().app.windows.iter_mut().find(|w| w.label == "main")
            .expect("the launcher window config");
        window.decorations = true;
        window.title_bar_style = tauri::TitleBarStyle::Overlay;
        window.hidden_title = true;
        context
    };
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .setup(|_app| {
            #[cfg(unix)]
            quit_signals::install(_app.handle().clone());
            Ok(())
        })
        .manage(App { place, game: Mutex::new(None), starting: AtomicBool::new(false), disc_check: AtomicU64::new(0) })
        .on_window_event(|window, event| {
            // The game's window took the focus: it is up.
            if let tauri::WindowEvent::Focused(false) = event {
                if window.state::<App>().starting.swap(false, Ordering::Relaxed) {
                    let _ = window.minimize();
                }
            }
        })
        .invoke_handler(tauri::generate_handler![
            snapshot,
            settings_schema,
            controls_schema,
            controls_state,
            controls_edit,
            rider_name,
            save_setting,
            check_update,
            install_update,
            play,
            stop_game,
            pick_disc,
            check_disc,
            server_status,
            open_folder,
            open_link
        ])
        .run(context)
        .expect("the launcher window");
}
