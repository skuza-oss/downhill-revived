//! The Discord Social SDK (finding 121), when its library sits next to the game: the presence goes
//! through it, and with ASK_TO_JOIN it hands over a friend's join from Discord. Without the library
//! the worker keeps to the plain IPC (worker.rs), as before.
//!
//! The library is Discord's, closed and not in this repository: `discord_partner_sdk.dll`
//! (`libdiscord_partner_sdk.so`, `.dylib`), from the developer portal. It is loaded at run time from
//! the executable's own folder only (never the working folder or PATH), so nothing links against it
//! and a game without it is unchanged. Rich Presence needs no sign-in: without `Connect` the SDK
//! talks to the running Discord client.
//!
//! The C API is cdiscord.h's: every object is one opaque pointer set up by `_Init` and released by
//! `_Drop`; setters copy the strings they are given; a string or a ClientResult handed to a callback
//! belongs to the callback (`Discord_Free`, `_Drop`), as discordpp.h's wrappers do. Every call,
//! `Discord_RunCallbacks` included, is made from the presence's own thread.

use crate::{party_id, Payload, APP_ID, BUTTONS, LOGO, LOGO_TEXT};
use libloading::Library;
use std::ffi::c_void;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

#[cfg(target_os = "windows")]
pub const LIBRARY: &str = "discord_partner_sdk.dll";
#[cfg(target_os = "macos")]
pub const LIBRARY: &str = "libdiscord_partner_sdk.dylib";
#[cfg(not(any(target_os = "windows", target_os = "macos")))]
pub const LIBRARY: &str = "libdiscord_partner_sdk.so";

/// The library's place: beside the running executable.
pub fn library_path() -> Option<PathBuf> {
    Some(std::env::current_exe().ok()?.parent()?.join(LIBRARY))
}

#[repr(C)]
#[derive(Clone, Copy)]
struct DString {
    ptr: *mut u8,
    size: usize,
}

impl DString {
    /// A borrowed view of `s` for a setter (which copies it).
    fn of(s: &str) -> DString {
        DString { ptr: s.as_ptr() as *mut u8, size: s.len() }
    }
}

/// Any SDK object (Discord_Client, Discord_Activity, ...).
#[repr(C)]
struct Obj {
    opaque: *mut c_void,
}

impl Obj {
    fn new() -> Obj {
        Obj { opaque: std::ptr::null_mut() }
    }
}

type FreeFn = unsafe extern "C" fn(*mut c_void);
type ResultCb = unsafe extern "C" fn(*mut Obj, *mut c_void);
type JoinCb = unsafe extern "C" fn(DString, *mut c_void);
type LogCb = unsafe extern "C" fn(DString, i32, *mut c_void);

// cdiscord.h's values
const ACTIVITY_PLAYING: i32 = 0;
const PLATFORM_DESKTOP: i32 = 1;
const SEVERITY_WARNING: i32 = 3;
/// At most this many of the SDK's own log lines reach dhd.log in a session.
const SDK_LOG_LINES: u32 = 20;

/// The functions used, looked up once.
struct Api {
    run_callbacks: unsafe extern "C" fn(),
    free: unsafe extern "C" fn(*mut c_void),
    client_init: unsafe extern "C" fn(*mut Obj),
    client_drop: unsafe extern "C" fn(*mut Obj),
    set_application_id: unsafe extern "C" fn(*mut Obj, u64),
    add_log_callback: unsafe extern "C" fn(*mut Obj, LogCb, FreeFn, *mut c_void, i32),
    update_rich_presence: unsafe extern "C" fn(*mut Obj, *mut Obj, ResultCb, FreeFn, *mut c_void),
    clear_rich_presence: unsafe extern "C" fn(*mut Obj),
    register_launch_command: unsafe extern "C" fn(*mut Obj, u64, DString) -> bool,
    set_activity_join_callback: unsafe extern "C" fn(*mut Obj, JoinCb, FreeFn, *mut c_void),
    result_drop: unsafe extern "C" fn(*mut Obj),
    result_successful: unsafe extern "C" fn(*mut Obj) -> bool,
    result_error: unsafe extern "C" fn(*mut Obj, *mut DString),
    activity_init: unsafe extern "C" fn(*mut Obj),
    activity_drop: unsafe extern "C" fn(*mut Obj),
    activity_set_type: unsafe extern "C" fn(*mut Obj, i32),
    activity_set_details: unsafe extern "C" fn(*mut Obj, *mut DString),
    activity_set_state: unsafe extern "C" fn(*mut Obj, *mut DString),
    activity_set_assets: unsafe extern "C" fn(*mut Obj, *mut Obj),
    activity_set_timestamps: unsafe extern "C" fn(*mut Obj, *mut Obj),
    activity_set_party: unsafe extern "C" fn(*mut Obj, *mut Obj),
    activity_set_secrets: unsafe extern "C" fn(*mut Obj, *mut Obj),
    activity_set_supported_platforms: unsafe extern "C" fn(*mut Obj, i32),
    activity_add_button: unsafe extern "C" fn(*mut Obj, *const Obj),
    assets_init: unsafe extern "C" fn(*mut Obj),
    assets_drop: unsafe extern "C" fn(*mut Obj),
    assets_set_large_image: unsafe extern "C" fn(*mut Obj, *mut DString),
    assets_set_large_text: unsafe extern "C" fn(*mut Obj, *mut DString),
    timestamps_init: unsafe extern "C" fn(*mut Obj),
    timestamps_drop: unsafe extern "C" fn(*mut Obj),
    timestamps_set_start: unsafe extern "C" fn(*mut Obj, u64),
    party_init: unsafe extern "C" fn(*mut Obj),
    party_drop: unsafe extern "C" fn(*mut Obj),
    party_set_id: unsafe extern "C" fn(*mut Obj, DString),
    party_set_current_size: unsafe extern "C" fn(*mut Obj, i32),
    party_set_max_size: unsafe extern "C" fn(*mut Obj, i32),
    secrets_init: unsafe extern "C" fn(*mut Obj),
    secrets_drop: unsafe extern "C" fn(*mut Obj),
    secrets_set_join: unsafe extern "C" fn(*mut Obj, DString),
    button_init: unsafe extern "C" fn(*mut Obj),
    button_drop: unsafe extern "C" fn(*mut Obj),
    button_set_label: unsafe extern "C" fn(*mut Obj, DString),
    button_set_url: unsafe extern "C" fn(*mut Obj, DString),
}

impl Api {
    /// # Safety
    /// `lib` is the Social SDK, whose exports have cdiscord.h's signatures.
    unsafe fn load(lib: &Library) -> Result<Api, libloading::Error> {
        macro_rules! f {
            ($name:literal) => {
                // SAFETY: the caller's contract (the field's type is the export's signature).
                *unsafe { lib.get(concat!($name, "\0").as_bytes()) }?
            };
        }
        Ok(Api {
            run_callbacks: f!("Discord_RunCallbacks"),
            free: f!("Discord_Free"),
            client_init: f!("Discord_Client_Init"),
            client_drop: f!("Discord_Client_Drop"),
            set_application_id: f!("Discord_Client_SetApplicationId"),
            add_log_callback: f!("Discord_Client_AddLogCallback"),
            update_rich_presence: f!("Discord_Client_UpdateRichPresence"),
            clear_rich_presence: f!("Discord_Client_ClearRichPresence"),
            register_launch_command: f!("Discord_Client_RegisterLaunchCommand"),
            set_activity_join_callback: f!("Discord_Client_SetActivityJoinCallback"),
            result_drop: f!("Discord_ClientResult_Drop"),
            result_successful: f!("Discord_ClientResult_Successful"),
            result_error: f!("Discord_ClientResult_Error"),
            activity_init: f!("Discord_Activity_Init"),
            activity_drop: f!("Discord_Activity_Drop"),
            activity_set_type: f!("Discord_Activity_SetType"),
            activity_set_details: f!("Discord_Activity_SetDetails"),
            activity_set_state: f!("Discord_Activity_SetState"),
            activity_set_assets: f!("Discord_Activity_SetAssets"),
            activity_set_timestamps: f!("Discord_Activity_SetTimestamps"),
            activity_set_party: f!("Discord_Activity_SetParty"),
            activity_set_secrets: f!("Discord_Activity_SetSecrets"),
            activity_set_supported_platforms: f!("Discord_Activity_SetSupportedPlatforms"),
            activity_add_button: f!("Discord_Activity_AddButton"),
            assets_init: f!("Discord_ActivityAssets_Init"),
            assets_drop: f!("Discord_ActivityAssets_Drop"),
            assets_set_large_image: f!("Discord_ActivityAssets_SetLargeImage"),
            assets_set_large_text: f!("Discord_ActivityAssets_SetLargeText"),
            timestamps_init: f!("Discord_ActivityTimestamps_Init"),
            timestamps_drop: f!("Discord_ActivityTimestamps_Drop"),
            timestamps_set_start: f!("Discord_ActivityTimestamps_SetStart"),
            party_init: f!("Discord_ActivityParty_Init"),
            party_drop: f!("Discord_ActivityParty_Drop"),
            party_set_id: f!("Discord_ActivityParty_SetId"),
            party_set_current_size: f!("Discord_ActivityParty_SetCurrentSize"),
            party_set_max_size: f!("Discord_ActivityParty_SetMaxSize"),
            secrets_init: f!("Discord_ActivitySecrets_Init"),
            secrets_drop: f!("Discord_ActivitySecrets_Drop"),
            secrets_set_join: f!("Discord_ActivitySecrets_SetJoin"),
            button_init: f!("Discord_ActivityButton_Init"),
            button_drop: f!("Discord_ActivityButton_Drop"),
            button_set_label: f!("Discord_ActivityButton_SetLabel"),
            button_set_url: f!("Discord_ActivityButton_SetUrl"),
        })
    }
}

/// What the SDK's callbacks report, for the worker to take after `Discord_RunCallbacks`.
#[derive(Default)]
pub struct Events {
    /// The last update's outcome: Ok, or the SDK's error.
    pub update: Option<Result<(), String>>,
    /// A friend's join from Discord: the secret.
    pub join: Option<String>,
    /// The SDK's own warnings and errors.
    pub lines: Vec<String>,
    logged: u32,
}

type Shared = Arc<Mutex<Events>>;

/// A callback's user data: the events, and the SDK's functions a callback needs to release what it
/// is given. One per callback registered or update sent, freed by the SDK through `free_context`.
#[derive(Clone)]
struct Context {
    events: Shared,
    free: unsafe extern "C" fn(*mut c_void),
    result_successful: unsafe extern "C" fn(*mut Obj) -> bool,
    result_error: unsafe extern "C" fn(*mut Obj, *mut DString),
    result_drop: unsafe extern "C" fn(*mut Obj),
}

impl Context {
    fn user_data(&self) -> *mut c_void {
        Box::into_raw(Box::new(self.clone())) as *mut c_void
    }

    /// # Safety
    /// `data` is from user_data and not freed yet.
    unsafe fn of<'a>(data: *mut c_void) -> &'a Context {
        // SAFETY: the caller's contract.
        unsafe { &*(data as *const Context) }
    }

    fn events(&self) -> std::sync::MutexGuard<'_, Events> {
        self.events.lock().unwrap_or_else(|e| e.into_inner())
    }

    /// # Safety
    /// `s` is a string the SDK gave a callback, not used again (it is freed here).
    unsafe fn take(&self, s: DString) -> String {
        if s.ptr.is_null() {
            return String::new();
        }
        // SAFETY: the SDK's string, `size` bytes at `ptr`.
        let text = String::from_utf8_lossy(unsafe { std::slice::from_raw_parts(s.ptr, s.size) }).into_owned();
        // SAFETY: the callback owns it (discordpp.h's wrappers free it the same way).
        unsafe { (self.free)(s.ptr as *mut c_void) };
        text
    }
}

unsafe extern "C" fn free_context(data: *mut c_void) {
    // SAFETY: made by Context::user_data, freed once by the SDK.
    drop(unsafe { Box::from_raw(data as *mut Context) });
}

unsafe extern "C" fn on_update(result: *mut Obj, data: *mut c_void) {
    // SAFETY: the user data given with this callback; the result is the callback's (discordpp.h's
    // wrapper takes it as Owned), dropped here once read.
    unsafe {
        let c = Context::of(data);
        let outcome = if (c.result_successful)(result) {
            Ok(())
        } else {
            let mut s = DString { ptr: std::ptr::null_mut(), size: 0 };
            (c.result_error)(result, &mut s);
            Err(c.take(s))
        };
        (c.result_drop)(result);
        c.events().update = Some(outcome);
    }
}

unsafe extern "C" fn on_join(secret: DString, data: *mut c_void) {
    // SAFETY: the user data given with this callback, and its string.
    unsafe {
        let c = Context::of(data);
        let secret = c.take(secret);
        c.events().join = Some(secret);
    }
}

unsafe extern "C" fn on_log(message: DString, _severity: i32, data: *mut c_void) {
    // SAFETY: the user data given with this callback, and its string.
    unsafe {
        let c = Context::of(data);
        let line = c.take(message);
        let mut e = c.events();
        if e.logged < SDK_LOG_LINES {
            e.logged += 1;
            e.lines.push(line.trim_end().to_string());
        }
    }
}

/// The SDK's client, on the presence's thread.
pub struct Client {
    api: Api,
    client: Obj,
    context: Context,
    _lib: Library,
}

// SAFETY: made on the presence's thread and used only there; moved to it once, before any call.
unsafe impl Send for Client {}

impl Client {
    /// Loads the SDK from `path`, sets the application, and (with `join`) takes friends' joins and
    /// registers `launch` as the command Discord starts the game with. Err: no library there, or not
    /// the SDK (the IPC is used then).
    pub fn load(path: &Path, join: bool, launch: Option<&str>) -> Result<Client, String> {
        if !path.is_file() {
            return Err(format!("{} not found", path.display()));
        }
        // SAFETY: loading runs the library's initialisers: Discord's SDK, from the game's folder.
        let lib = unsafe { Library::new(path) }.map_err(|e| e.to_string())?;
        // SAFETY: the Social SDK's exports have cdiscord.h's signatures.
        let api = unsafe { Api::load(&lib) }.map_err(|e| e.to_string())?;
        let id: u64 = APP_ID.parse().map_err(|_| "bad application id".to_string())?;
        let context = Context {
            events: Shared::default(),
            free: api.free,
            result_successful: api.result_successful,
            result_error: api.result_error,
            result_drop: api.result_drop,
        };
        let mut client = Obj::new();
        // SAFETY: the SDK's calls on a fresh client, from this thread.
        unsafe {
            (api.client_init)(&mut client);
            (api.set_application_id)(&mut client, id);
            (api.add_log_callback)(&mut client, on_log, free_context, context.user_data(), SEVERITY_WARNING);
            if join {
                (api.set_activity_join_callback)(&mut client, on_join, free_context, context.user_data());
                if let Some(command) = launch {
                    (api.register_launch_command)(&mut client, id, DString::of(command));
                }
            }
        }
        Ok(Client { api, client, context, _lib: lib })
    }

    /// What the callbacks reported since the last look (taken).
    pub fn take_events(&self) -> Events {
        std::mem::take(&mut *self.context.events())
    }

    /// Runs the SDK's pending callbacks (they fill `events`).
    pub fn run_callbacks(&self) {
        // SAFETY: on the client's thread.
        unsafe { (self.api.run_callbacks)() };
    }

    /// Shows `p` (None: clears); the outcome comes back in `events.update`.
    pub fn set(&mut self, p: Option<&Payload>) {
        let a = &self.api;
        let Some(p) = p else {
            // SAFETY: a live client.
            unsafe { (a.clear_rich_presence)(&mut self.client) };
            return;
        };
        // SAFETY: objects made and dropped here; the setters copy what they are given; the
        // strings outlive the calls.
        unsafe {
            let mut activity = Obj::new();
            (a.activity_init)(&mut activity);
            (a.activity_set_type)(&mut activity, ACTIVITY_PLAYING);
            let mut details = DString::of(&p.details);
            (a.activity_set_details)(&mut activity, &mut details);
            if let Some(state) = &p.state {
                let mut state = DString::of(state);
                (a.activity_set_state)(&mut activity, &mut state);
            }
            let mut timestamps = Obj::new();
            (a.timestamps_init)(&mut timestamps);
            (a.timestamps_set_start)(&mut timestamps, p.start.max(0) as u64);
            (a.activity_set_timestamps)(&mut activity, &mut timestamps);
            (a.timestamps_drop)(&mut timestamps);
            let mut assets = Obj::new();
            (a.assets_init)(&mut assets);
            let (mut image, mut text) = (DString::of(LOGO), DString::of(LOGO_TEXT));
            (a.assets_set_large_image)(&mut assets, &mut image);
            (a.assets_set_large_text)(&mut assets, &mut text);
            (a.activity_set_assets)(&mut activity, &mut assets);
            (a.assets_drop)(&mut assets);
            for (label, url) in BUTTONS {
                let mut button = Obj::new();
                (a.button_init)(&mut button);
                (a.button_set_label)(&mut button, DString::of(label));
                (a.button_set_url)(&mut button, DString::of(url));
                (a.activity_add_button)(&mut activity, &button);
                (a.button_drop)(&mut button);
            }
            if p.party.is_some() || p.join.is_some() {
                let mut party = Obj::new();
                (a.party_init)(&mut party);
                (a.party_set_id)(&mut party, DString::of(&party_id()));
                if let Some([n, max]) = p.party {
                    (a.party_set_current_size)(&mut party, n);
                    (a.party_set_max_size)(&mut party, max);
                }
                if let Some(join) = &p.join {
                    let mut secrets = Obj::new();
                    (a.secrets_init)(&mut secrets);
                    (a.secrets_set_join)(&mut secrets, DString::of(join));
                    (a.activity_set_secrets)(&mut activity, &mut secrets);
                    (a.secrets_drop)(&mut secrets);
                    // the join button shows where the game runs
                    (a.activity_set_supported_platforms)(&mut activity, PLATFORM_DESKTOP);
                }
                (a.activity_set_party)(&mut activity, &mut party);
                (a.party_drop)(&mut party);
            }
            (a.update_rich_presence)(&mut self.client, &mut activity, on_update, free_context, self.context.user_data());
            (a.activity_drop)(&mut activity);
        }
    }
}

impl Drop for Client {
    fn drop(&mut self) {
        // SAFETY: a live client, dropped once, before its library.
        unsafe { (self.api.client_drop)(&mut self.client) };
    }
}
