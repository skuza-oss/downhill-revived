"""The game on a desktop that is never shown (Windows), driven by keys and mouse messages posted to its
window, with pictures from the game's own captures. For checks that need the real menus and keys
while someone works on the same PC: nothing appears on screen and nothing takes the focus.

    g = hidden_game.Game(exe, out, env={...}, name='my-check')   # takes game_lock, starts the game
    g.window(); hidden_game.to_main(g)                           # to the main menu
    g.keys('down', 'down', 'x'); g.shot('online-menu')           # OUT/online-menu.png
    g.stop()                                                     # only this game; frees the lock

The game gets: DHD_ISO (env, else DHD_ISO / DHD_TEST_ISO of this process), DHD_TEST_INPUT=1 (posted
keys count without focus) and SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=0 (a pad in use on this PC must
not drive the hidden game), interval captures into OUT/raw (DHD_SCREENSHOT), windowed 960x720 at
scale 1, no movies, sound or settings file. VAR='<unset>' in env drops one of these defaults, e.g.
DHD_SCREENSHOT='<unset>' with DHD_CAPTURE_PRESENTED for presenter pictures (the threaded graphics
path, as players run it). Traps: docs/TESTING.md."""
import ctypes
import os
import re
import shutil
import sys
import time
from ctypes import wintypes as w
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import game_lock  # noqa: E402

u32 = ctypes.WinDLL('user32', use_last_error=True)
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
u32.CreateDesktopW.restype = w.HANDLE
u32.CreateDesktopW.argtypes = [w.LPCWSTR, w.LPCWSTR, ctypes.c_void_p, w.DWORD, w.DWORD, ctypes.c_void_p]
u32.CloseDesktop.argtypes = [w.HANDLE]
WNDENUMPROC = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
u32.EnumDesktopWindows.argtypes = [w.HANDLE, WNDENUMPROC, w.LPARAM]
u32.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
u32.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
u32.GetClientRect.argtypes = [w.HWND, ctypes.POINTER(w.RECT)]
u32.IsWindowVisible.argtypes = [w.HWND]
u32.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, ctypes.c_int]
u32.MapVirtualKeyW.restype = w.UINT


class _StartupInfo(ctypes.Structure):
    _fields_ = [('cb', w.DWORD), ('reserved', w.LPWSTR), ('desktop', w.LPWSTR), ('title', w.LPWSTR)] + \
        [(n, w.DWORD) for n in 'x y xs ys xc yc fill flags'.split()] + \
        [('show', w.WORD), ('r2', w.WORD), ('r3', ctypes.c_void_p), ('stdin', w.HANDLE), ('stdout', w.HANDLE),
         ('stderr', w.HANDLE)]


class _ProcessInfo(ctypes.Structure):
    _fields_ = [('process', w.HANDLE), ('thread', w.HANDLE), ('pid', w.DWORD), ('tid', w.DWORD)]


class _Security(ctypes.Structure):
    _fields_ = [('n', w.DWORD), ('descriptor', ctypes.c_void_p), ('inherit', w.BOOL)]


k32.CreateFileW.restype = w.HANDLE
k32.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.POINTER(_Security), w.DWORD, w.DWORD, w.HANDLE]

VK = {'left': 0x25, 'up': 0x26, 'right': 0x27, 'down': 0x28, 'space': 0x20, 'enter': 0x0D, 'esc': 0x1B,
      'tab': 0x09, 'backspace': 0x08}


def test_iso():
    iso = os.environ.get('DHD_ISO') or os.environ.get('DHD_TEST_ISO')
    if not iso:
        raise RuntimeError('set DHD_TEST_ISO to your Downhill Domination (USA) disc image')
    return iso


class Game:
    def __init__(self, exe, out, env=None, name='check', scale=1, shot_window='960x720', interval=0.5):
        self.out = Path(out); self.out.mkdir(parents=True, exist_ok=True)
        self.shots = self.out / 'raw'; self.shots.mkdir(exist_ok=True)
        for f in self.shots.glob('*.bmp'): f.unlink()
        e = {k: v for k, v in os.environ.items() if not k.upper().startswith('DHD_')}
        e.update(DHD_ISO=test_iso(), DHD_SKIP_MOVIES='1', DHD_MEMCARD='0', DHD_SND_OFF='1', DHD_SETTINGS='none',
                 DHD_RENDER='native', DHD_SCALE=str(scale), DHD_WINDOW='windowed', DHD_WINDOW_WIDTH='960',
                 DHD_WINDOW_HEIGHT='720', DHD_SCREENSHOT=str(self.shots / 'shot'), DHD_CAPTURE_AT='0',
                 DHD_CAPTURE_INTERVAL=str(interval), DHD_AUTO_UPDATE='off', DHD_TEST_INPUT='1',
                 SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS='0')
        if shot_window: e['DHD_SCREENSHOT_WINDOW'] = shot_window
        e.update(env or {})
        e = {k: v for k, v in e.items() if v != '<unset>'}
        game_lock.acquire(name)
        try:
            self.desk_name = f'{name}-{os.getpid()}'
            self.desk = u32.CreateDesktopW(self.desk_name, None, None, 0, 0x10000000, None)
            sa = _Security(ctypes.sizeof(_Security), None, True)
            log = k32.CreateFileW(str(self.out / 'game.log'), 0x40000000, 3, ctypes.byref(sa), 2, 0x80, None)
            si = _StartupInfo(); si.cb = ctypes.sizeof(_StartupInfo); si.desktop = 'WinSta0\\' + self.desk_name
            si.flags = 0x100; si.stdout = log; si.stderr = log
            pi = _ProcessInfo()
            block = ''.join(f'{k}={v}\0' for k, v in sorted(e.items(), key=lambda kv: kv[0].upper())) + '\0'
            ok = k32.CreateProcessW(str(exe), ctypes.create_unicode_buffer(f'"{exe}"'), None, None, True,
                                    0x400 | 0x08000000, ctypes.create_unicode_buffer(block, len(block)),
                                    str(Path(exe).parent), ctypes.byref(si), ctypes.byref(pi))
            k32.CloseHandle(log)
            if not ok: raise OSError(ctypes.get_last_error(), 'CreateProcess')
        except BaseException:
            game_lock.release()
            raise
        self.pi = pi; self.pid = pi.pid; self.hwnd = None
        (self.out / 'pid.txt').write_text(str(self.pid))
        print('pid', self.pid, flush=True)

    def windows(self):
        found = []
        def each(h, _):
            pid = w.DWORD(); u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
            buf = ctypes.create_unicode_buffer(256); u32.GetClassNameW(h, buf, 256)
            found.append((h, pid.value, buf.value, bool(u32.IsWindowVisible(h))))
            return True
        u32.EnumDesktopWindows(self.desk, WNDENUMPROC(each), 0)
        return found

    def window(self, timeout=60):
        start = time.time()
        while time.time() - start < timeout:
            for h, pid, cls, visible in self.windows():
                if 'SDL' in cls and visible:
                    self.hwnd = h; return h
            if not self.alive(): raise RuntimeError('the game exited (see game.log)')
            time.sleep(0.5)
        raise RuntimeError('no window: %r' % self.windows())

    def alive(self):
        return k32.WaitForSingleObject(self.pi.process, 0) == 0x102

    def log(self):
        return (self.out / 'game.log').read_text(errors='replace')

    def wait_log(self, pattern, timeout=120):
        start = time.time()
        while time.time() - start < timeout:
            if re.search(pattern, self.log()): return True
            if not self.alive(): raise RuntimeError('game exited')
            time.sleep(0.25)
        raise RuntimeError('timeout waiting for %r' % pattern)

    def field(self):
        """The newest capture line's field count (0 before the first capture)."""
        found = re.findall(r'compare: capture .* field=(\d+)', self.log())
        return int(found[-1]) if found else 0

    def post(self, msg, wp, lp):
        u32.PostMessageW(self.hwnd, msg, wp, lp)

    def key(self, name, hold=0.12):
        vk = VK.get(name) or ord(name.upper())
        sc = u32.MapVirtualKeyW(vk, 0)
        ext = 1 << 24 if vk in (0x25, 0x26, 0x27, 0x28) else 0
        self.post(0x0100, vk, 1 | sc << 16 | ext)
        time.sleep(hold)
        self.post(0x0101, vk, 1 | sc << 16 | ext | 0xC0000000)
        time.sleep(0.05)

    def type(self, text, gap=0.15):
        for ch in text:
            self.post(0x0102, ord(ch), 1); time.sleep(gap)  # WM_CHAR: text input

    def keys(self, *names, gap=0.35):
        for n in names:
            self.key(n); time.sleep(gap)

    def move(self, x, y):
        self.post(0x0200, 0, (y & 0xFFFF) << 16 | (x & 0xFFFF))

    def click(self, x, y, button='left', hold=0.1):
        down, up, mk = {'left': (0x0201, 0x0202, 1), 'right': (0x0204, 0x0205, 2)}[button]
        lp = (y & 0xFFFF) << 16 | (x & 0xFFFF)
        self.post(0x0200, 0, lp); time.sleep(0.08)
        self.post(down, mk, lp); time.sleep(hold); self.post(up, 0, lp); time.sleep(0.05)

    def wheel(self, x, y, clicks):
        # WM_MOUSEWHEEL takes screen coordinates; the hidden desktop's window sits where it was made
        self.post(0x020A, ((120 * clicks) & 0xFFFF) << 16, (y & 0xFFFF) << 16 | (x & 0xFFFF))

    def client(self):
        r = w.RECT(); u32.GetClientRect(self.hwnd, ctypes.byref(r)); return r.right, r.bottom

    def shot(self, label, settle=1.2):
        """The newest capture as OUT/LABEL.png (None when there is none yet)."""
        time.sleep(settle)
        files = sorted(self.shots.glob('shot-*.bmp'), key=os.path.getmtime)
        if not files: print('no shot for', label); return None
        dst = self.out / (label + '.png')
        try:
            from PIL import Image
            Image.open(files[-1]).save(dst)
        except Exception as e:
            print('png failed', e); dst = self.out / (label + '.bmp'); shutil.copy(files[-1], dst)
        for f in files[:-2]:
            try: f.unlink()
            except OSError: pass
        print('shot', dst, flush=True)
        return dst

    def stop(self):
        if self.alive():
            k32.TerminateProcess(self.pi.process, 1); k32.WaitForSingleObject(self.pi.process, 10000)
            print('stopped pid', self.pid, flush=True)
        k32.CloseHandle(self.pi.process); k32.CloseHandle(self.pi.thread)
        u32.CloseDesktop(self.desk)
        game_lock.release()


def latest_colour(g):
    """Mean saturation of the newest capture (0 grey .. 255), or None."""
    from PIL import Image, ImageStat
    files = sorted(g.shots.glob('shot-*.bmp'), key=os.path.getmtime)
    for f in reversed(files[-3:]):
        try:
            m = ImageStat.Stat(Image.open(f).convert('HSV').resize((64, 48))).mean
            return m[1] if m[2] > 8 else None
        except Exception:
            continue
    return None


def to_main(g, timeout=90):
    """From launch to the main menu: Triangle (S) on the grey memory card screen, then X on the title."""
    g.window()
    start, seen_grey = time.time(), False
    while time.time() - start < timeout:
        c = latest_colour(g)
        if c is not None and c < 25:
            seen_grey = True
            time.sleep(1.0); g.key('s'); time.sleep(2.0)
        elif seen_grey and c is not None and c >= 25:
            time.sleep(1.5); g.key('x'); time.sleep(3.0)
            return
        else:
            time.sleep(0.5)
    raise RuntimeError('main menu not reached')
