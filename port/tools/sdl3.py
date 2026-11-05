"""SDL3 for the PC build: generic controllers (finding 059).

    python port/tools/sdl3.py          prints the static library it built (or had built)

package_release.py calls build(X64_TARGET) for build_host64.py. It downloads SDL's release source
once (pinned version, SHA-256 checked; nothing else is accepted), unpacks it under build/port/sdl3
and builds a static library with CMake and Ninja for 64-bit Windows (x86_64-pc-windows-msvc,
clang, the static C runtime that dhd64.exe uses).
The window (video, Direct3D 11 surface only: no OpenGL, Vulkan or SDL renderer) and the audio
output (WASAPI) are built too (finding 083 / W11). No GPU or camera subsystems.
posix_build() is the same source built on Linux x64 (or WSL) and macOS arm64 (build_host64.py's
linux-x64 and macos-arm64 targets): the window (X11 and Wayland where their dev packages are
present; Cocoa and Metal views on macOS) and audio (PulseAudio/PipeWire and ALSA where present;
Core Audio), same SDL version and subsystem selection.
android_build() (--android) is libSDL3.so for Android arm64 with the NDK, plus SDL's file dialog,
and the release's Java sources (build_host64.py --target android-arm64, port/android/build_apk.py; finding 091).
SDL is zlib-licensed (port/release/THIRD-PARTY-NOTICES.txt).

The download is kept in DHD_DEVCACHES/downloads (one for every checkout; finding 151), so a rebuild
needs no network, and each built library is kept in DHD_DEVCACHES/dhd-sdl3 under its recipe: a new
checkout copies it instead of building SDL again. Needs CMake on PATH (or in its default place), LLVM's clang and the
ninja Python package (requirements.txt)."""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

VERSION = '3.4.18'
URL = 'https://github.com/libsdl-org/SDL/releases/download/release-%s/SDL3-%s.tar.gz' % (VERSION, VERSION)
SHA256 = '9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3'

ROOT = Path(__file__).resolve().parents[2]
DEVCACHES = Path(os.environ.get('DHD_DEVCACHES') or Path.home() / 'DevCaches')  # downloaded tools and caches
DOWNLOADS = DEVCACHES / 'downloads'
SHARED = DEVCACHES / 'dhd-sdl3'
OUT = ROOT / 'build' / 'port' / 'sdl3'
CMAKE_DEFAULT = Path(os.environ.get('ProgramFiles', r'C:\Program Files')) / 'CMake' / 'bin' / 'cmake.exe'

# Everything but the window, audio and controllers off. SDL_JOYSTICK brings the drivers: XInput, RawInput,
# Windows.Gaming.Input, DirectInput (generic pads, with the built-in mapping database) and HIDAPI
# (DualSense, DualShock 4, Switch Pro, 8BitDo and others, rumble included).
OPTIONS = dict(SDL_SHARED='OFF', SDL_STATIC='ON', SDL_TEST_LIBRARY='OFF', SDL_TESTS='OFF', SDL_EXAMPLES='OFF',
               SDL_INSTALL='OFF', SDL_AUDIO='ON', SDL_VIDEO='ON', SDL_OPENGL='OFF', SDL_OPENGLES='OFF',
               SDL_VULKAN='OFF', SDL_GPU='OFF', SDL_RENDER='OFF',
               SDL_CAMERA='OFF', SDL_POWER='OFF', SDL_SENSOR='OFF', SDL_DIALOG='OFF', SDL_TRAY='OFF',
               SDL_HAPTIC='OFF', SDL_JOYSTICK='ON', SDL_HIDAPI='ON', SDL_HIDAPI_JOYSTICK='ON',
               SDL_VIRTUAL_JOYSTICK='OFF', SDL_LIBC='ON', SDL_ASSEMBLY='OFF',
               SDL_DISABLE_INSTALL='ON', SDL_DISABLE_UNINSTALL='ON',
               # SDL's CMake looks for dinput.h only when audio or video is on, which would leave
               # out the DirectInput driver the generic pads need: the Windows SDK has it.
               HAVE_DINPUT_H='1',
               # GameInput (a C++ driver for Microsoft's newest API, off by default on desktop
               # Windows) is left out.
               HAVE_GAMEINPUT_H='0',
               # atan from SDL's own libm (fdlibm) instead of the C runtime's: the runtime's atan
               # brings a table of doubles in which package_release.py's path check reads
               # "Y:/(A6>" (SDL's dynamic API table references every SDL function, SDL_atan too)
               LIBC_HAS_ATAN='0', LIBC_HAS_ATANF='0', LIBC_HAS_ATAN2='0', LIBC_HAS_ATAN2F='0')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def fetch():
    tarball = DOWNLOADS / ('SDL3-%s.tar.gz' % VERSION)
    old = ROOT / 'build' / 'downloads' / tarball.name  # where a checkout kept it before
    # each process writes its own .part and publishes it whole (os.replace): checkouts share DOWNLOADS
    part = tarball.with_name('%s.%d.part' % (tarball.name, os.getpid()))
    if not tarball.exists() and old.exists() and sha256(old) == SHA256:
        DOWNLOADS.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(old, part)
        os.replace(part, tarball)
    if not tarball.exists() or sha256(tarball) != SHA256:
        DOWNLOADS.mkdir(parents=True, exist_ok=True)
        print('SDL3: downloading', URL)
        with urllib.request.urlopen(URL, timeout=120) as r, open(part, 'wb') as f:
            shutil.copyfileobj(r, f)
        got = sha256(part)
        if got != SHA256:
            part.unlink()
            sys.exit('SDL3: %s has SHA-256 %s, expected %s' % (URL, got, SHA256))
        os.replace(part, tarball)
    return tarball


def _shared_entry(base, recipe, compiler):
    """The machine-wide copy of base's library for this recipe and compiler (path, size, mtime)."""
    st = os.stat(compiler)
    ident = '%s|%s %d %d' % (recipe, os.path.realpath(compiler), st.st_size, st.st_mtime_ns)
    return SHARED / ('%s-%s' % (base.name, hashlib.sha256(ident.encode()).hexdigest()[:16]))


def _from_shared(base, entry):
    """Copy a library built in another checkout (entry) into base; False if there is none."""
    if not (entry / 'built.txt').exists():
        return False
    shutil.copytree(entry, base, dirs_exist_ok=True)
    return True


def _to_shared(base, entry, parts):
    """Keep base's parts (relative paths: the library, its headers, the stamp) as entry, written
    whole or not at all."""
    if entry.exists():
        return
    tmp = entry.with_name(entry.name + '.tmp%d' % os.getpid())
    try:
        shutil.rmtree(tmp, ignore_errors=True)
        for part in parts:
            src, dst = base / part, tmp / part
            dst.parent.mkdir(parents=True, exist_ok=True)
            (shutil.copytree if src.is_dir() else shutil.copy2)(src, dst)
        os.replace(tmp, entry)
    except OSError:
        shutil.rmtree(tmp, ignore_errors=True)


def tool(name, default=None):
    found = shutil.which(name)
    if found:
        return found
    if default and Path(default).exists():
        return str(default)
    sys.exit('SDL3: %s not found (docs: README.md, Building)' % name)


X64_TARGET = 'x86_64-pc-windows-msvc'  # dhd64.exe (build_host64.py, package_release.py)


def build(target=X64_TARGET):
    """The static library and its include directory; built once per version and target, under
    build/port/sdl3/<version>-<arch> with the same options (the window, audio and controllers)."""
    base = OUT / ('%s-%s' % (VERSION, target.split('-')[0]))
    lib = base / 'build' / 'SDL3-static.lib'
    include = base / 'src' / ('SDL3-%s' % VERSION) / 'include'
    stamp = base / 'built.txt'
    recipe = repr(sorted(OPTIONS.items())) + target
    if lib.exists() and stamp.exists() and stamp.read_text() == recipe:
        return lib, include
    clang = tool('clang')
    entry = _shared_entry(base, recipe, clang)
    if _from_shared(base, entry) and stamp.read_text() == recipe:
        return lib, include
    tarball = fetch()
    src_parent = base / 'src'
    if src_parent.exists():
        shutil.rmtree(src_parent)
    src_parent.mkdir(parents=True)
    with tarfile.open(tarball) as t:
        t.extractall(src_parent, filter='data')
    src = src_parent / ('SDL3-%s' % VERSION)
    bld = base / 'build'
    if bld.exists():
        shutil.rmtree(bld)
    import ninja
    cmake = tool('cmake', CMAKE_DEFAULT)
    cmd = [cmake, '-S', str(src), '-B', str(bld), '-G', 'Ninja',
           '-DCMAKE_MAKE_PROGRAM=' + os.path.join(ninja.BIN_DIR, 'ninja.exe'),
           '-DCMAKE_C_COMPILER=' + clang, '-DCMAKE_C_COMPILER_TARGET=' + target,
           '-DCMAKE_CXX_COMPILER=' + clang.replace('clang', 'clang++'), '-DCMAKE_CXX_COMPILER_TARGET=' + target,
           '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded',
           '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW']
    cmd += ['-D%s=%s' % kv for kv in sorted(OPTIONS.items())]
    for step in (cmd, [cmake, '--build', str(bld)]):
        p = subprocess.run(step, capture_output=True, text=True)
        if p.returncode:
            sys.exit('SDL3 build failed:\n' + (p.stdout + p.stderr)[-4000:])
    if not lib.exists():
        sys.exit('SDL3: %s missing after the build' % lib)
    stamp.write_text(recipe)
    _to_shared(base, entry, [lib.relative_to(base), include.relative_to(base), 'built.txt'])
    return lib, include


# The Windows libraries the static SDL3 needs at link time.
SYSTEM_LIBS = ['setupapi.lib', 'winmm.lib', 'imm32.lib', 'version.lib', 'ole32.lib', 'oleaut32.lib',
               'advapi32.lib', 'shell32.lib', 'user32.lib', 'gdi32.lib', 'cfgmgr32.lib', 'uuid.lib',
               'dwmapi.lib', 'shcore.lib']


def posix_build():
    """The static library for this Linux x64 or macOS arm64 machine, its include directory and the
    system libraries (and on macOS frameworks) it needs.

    Same SDL version, same subsystem selection (OPTIONS: the window, audio and controllers;
    no GPU/renderer/camera). On Linux CMake finds the backends whose dev packages are installed
    (X11, Wayland, PulseAudio/PipeWire, ALSA), and the build fails closed if neither X11 nor
    Wayland is found, so a windowless SDL3 never ships silently; on macOS the window is Cocoa's
    (with Metal views) and the sound Core Audio. The library installs under its build dir
    (build/port/sdl3/<version>-<os>-<arch>/install); the system libraries come from the installed
    sdl3.pc (its Libs and Libs.private, read here: no pkg-config needed). Output stays under build/."""
    import json as _json
    import platform as _platform
    if sys.platform == 'win32':
        sys.exit('SDL3: the POSIX build runs on Linux (or WSL) or macOS (python3 port/tools/sdl3.py --posix there)')
    system = 'macos' if sys.platform == 'darwin' else 'linux'
    base = OUT / ('%s-%s-%s' % (VERSION, system, _platform.machine()))
    prefix = base / 'install'
    lib = prefix / 'lib' / 'libSDL3.a'
    include = prefix / 'include'
    stamp = base / 'built.txt'
    libs_file = base / 'link_libs.json'
    options = {k: v for k, v in OPTIONS.items()
               if k not in ('SDL_INSTALL', 'SDL_DISABLE_INSTALL', 'SDL_DISABLE_UNINSTALL', 'HAVE_DINPUT_H', 'HAVE_GAMEINPUT_H')}
    options.update(SDL_INSTALL='ON', SDL_DISABLE_INSTALL='OFF', SDL_DISABLE_UNINSTALL='OFF')
    if system == 'macos':
        options['CMAKE_OSX_DEPLOYMENT_TARGET'] = '11.0'  # the first macOS on Apple silicon (Info.plist's too)
    # SDL's assertions name their source files (__FILE__), and package_release refuses a builder's
    # folder in the game: gcc 11 (Ubuntu 22.04, the Linux release's base) kept SDL_gpu.c's full path.
    # Its sources are named from SDL's own folder instead.
    options['CMAKE_C_FLAGS'] = '-ffile-prefix-map=%s=SDL3-%s' % (base / 'src' / ('SDL3-%s' % VERSION), VERSION)
    recipe = repr(sorted(options.items())) + '%s-%s' % (system, _platform.machine())
    if lib.exists() and stamp.exists() and libs_file.exists() and stamp.read_text() == recipe:
        return lib, include, _json.loads(libs_file.read_text())
    entry = _shared_entry(base, recipe, shutil.which('cc') or '/usr/bin/cc')
    if _from_shared(base, entry) and stamp.read_text() == recipe and libs_file.exists():
        return lib, include, _json.loads(libs_file.read_text())
    for name in ('cmake', 'ninja'):
        if not shutil.which(name):
            sys.exit('SDL3: %s not found on PATH (apt: cmake, ninja-build; macOS: cmake and ninja)' % name)
    tarball = fetch()
    src_parent = base / 'src'
    if src_parent.exists():
        shutil.rmtree(src_parent)
    src_parent.mkdir(parents=True)
    with tarfile.open(tarball) as t:
        t.extractall(src_parent, filter='data')
    src = src_parent / ('SDL3-%s' % VERSION)
    bld = base / 'build'
    if bld.exists():
        shutil.rmtree(bld)
    if prefix.exists():
        shutil.rmtree(prefix)
    cmd = ['cmake', '-S', str(src), '-B', str(bld), '-G', 'Ninja',
           '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_PREFIX=' + str(prefix),
           '-DCMAKE_INSTALL_LIBDIR=lib']
    cmd += ['-D%s=%s' % kv for kv in sorted(options.items())]
    jobs = os.environ.get('DHD_SDL_JOBS', '4')  # an 8 GB Mac builds with 4
    for step in (cmd, ['cmake', '--build', str(bld), '-j', jobs], ['cmake', '--install', str(bld)]):
        p = subprocess.run(step, capture_output=True, text=True)
        if p.returncode:
            sys.exit('SDL3 %s build failed:\n' % system + (p.stdout + p.stderr)[-4000:])
    if not lib.exists():
        sys.exit('SDL3: %s missing after the build' % lib)
    cache = (bld / 'CMakeCache.txt').read_text()
    backends = ('SDL_COCOA:',) if system == 'macos' else ('SDL_X11:', 'SDL_WAYLAND:')
    video = [line for line in cache.splitlines() if line.startswith(backends)]
    # The Linux backends' own libraries need not appear below: SDL3 dlopens them (configure logs
    # `dynamic libX11 -> ...`), so the window-backend gate reads the configure result, not the link
    # line. macOS links its frameworks.
    if system == 'linux' and not any(line.endswith('=ON') for line in video):
        sys.exit('SDL3: no window backend (X11/Wayland dev packages missing?):\n  ' + '\n  '.join(video))
    pc = (prefix / 'lib' / 'pkgconfig' / 'sdl3.pc').read_text()
    fields = dict(re.findall(r'^([A-Za-z_.]+)\s*[:=]\s*(.*)$', pc, re.M))
    expand = lambda s: re.sub(r'\$\{(\w+)\}', lambda m: expand(fields.get(m.group(1), '')), s)
    words = (expand(fields.get('Libs', '')) + ' ' + expand(fields.get('Libs.private', ''))).split()
    libs, k = [], 0
    while k < len(words):  # "-framework X" stays one entry: the link line keeps the pair together
        word = words[k]
        if word in ('-framework', '-weak_framework') and k + 1 < len(words):
            libs.append(word + ' ' + words[k + 1])
            k += 2
            continue
        if not word.startswith('-L') and 'SDL3' not in word and word not in libs:
            libs.append(word)
        k += 1
    libs_file.write_text(_json.dumps(libs) + '\n')
    stamp.write_text(recipe)
    _to_shared(base, entry, ['install', 'link_libs.json', 'built.txt'])
    return lib, include, libs


ANDROID_API = 30  # Android 11 (memfd_create, the Mi 11i's first release); build_host64.py's android-arm64 target too
ANDROID_DIRNAME = '%s-android-arm64' % VERSION  # build/port/sdl3/<version>-android-arm64


def android_ndk():
    """The NDK: ANDROID_NDK_HOME, else the newest under the SDK (ANDROID_HOME/ndk). Never on C:."""
    named = os.environ.get('ANDROID_NDK_HOME') or os.environ.get('ANDROID_NDK_ROOT')
    if named:
        ndk = Path(named)
    else:
        sdk = Path(os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT') or Path.home() / 'Android/Sdk')
        found = sorted((p for p in (sdk / 'ndk').glob('*') if (p / 'source.properties').exists()),
                       key=lambda p: [int(x) for x in p.name.split('.') if x.isdigit()])
        if not found:
            sys.exit('SDL3: no NDK under %s/ndk (docs/findings/091-android.md, Toolchain)' % sdk)
        ndk = found[-1]
    if not (ndk / 'build' / 'cmake' / 'android.toolchain.cmake').exists():
        sys.exit('SDL3: %s is not an NDK' % ndk)
    return ndk


def android_build():
    """libSDL3.so for Android arm64-v8a (API ANDROID_API), its include directory and the Java
    sources of the same release (android-project/app/src/main/java: SDLActivity and friends, which
    build_apk.py puts in the APK). Same subsystem selection as OPTIONS (window, audio, controllers),
    plus SDL's file dialog (the disc image is chosen through Android's document picker). A shared
    library, as SDL's Java side loads it (System.loadLibrary("SDL3")); its own libc++ is static.
    Built with the NDK's CMake toolchain file, CMake and Ninja, under
    build/port/sdl3/<version>-android-arm64."""
    base = OUT / ANDROID_DIRNAME
    src = base / 'src' / ('SDL3-%s' % VERSION)
    lib = base / 'build' / 'libSDL3.so'
    include = src / 'include'
    java = src / 'android-project' / 'app' / 'src' / 'main' / 'java'
    stamp = base / 'built.txt'
    ndk = android_ndk()
    options = {k: v for k, v in OPTIONS.items() if k not in ('HAVE_DINPUT_H', 'HAVE_GAMEINPUT_H')}
    options.update(SDL_SHARED='ON', SDL_STATIC='OFF', SDL_DIALOG='ON')
    recipe = repr(sorted(options.items())) + 'android-arm64-%d %s' % (ANDROID_API, ndk.name)
    if lib.exists() and stamp.exists() and stamp.read_text() == recipe:
        return lib, include, java
    entry = _shared_entry(base, recipe, ndk / 'source.properties')
    if _from_shared(base, entry) and stamp.read_text() == recipe:
        return lib, include, java
    tarball = fetch()
    if src.parent.exists():
        shutil.rmtree(src.parent)
    src.parent.mkdir(parents=True)
    with tarfile.open(tarball) as t:
        t.extractall(src.parent, filter='data')
    bld = base / 'build'
    if bld.exists():
        shutil.rmtree(bld)
    import ninja
    cmake = tool('cmake', CMAKE_DEFAULT)
    cmd = [cmake, '-S', str(src), '-B', str(bld), '-G', 'Ninja',
           '-DCMAKE_MAKE_PROGRAM=' + os.path.join(ninja.BIN_DIR, 'ninja.exe' if os.name == 'nt' else 'ninja'),
           '-DCMAKE_TOOLCHAIN_FILE=' + str(ndk / 'build' / 'cmake' / 'android.toolchain.cmake'),
           '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-%d' % ANDROID_API,
           '-DANDROID_STL=c++_static', '-DCMAKE_BUILD_TYPE=Release']
    cmd += ['-D%s=%s' % kv for kv in sorted(options.items())]
    for step in (cmd, [cmake, '--build', str(bld), '-j', '2']):
        p = subprocess.run(step, capture_output=True, text=True)
        if p.returncode:
            sys.exit('SDL3 Android build failed:\n' + (p.stdout + p.stderr)[-4000:])
    if not lib.exists():
        sys.exit('SDL3: %s missing after the build' % lib)
    stamp.write_text(recipe)
    # android-project: SDL's Java sources and the Gradle wrapper build_apk.py copies
    _to_shared(base, entry, [lib.relative_to(base), include.relative_to(base), (src / 'android-project').relative_to(base),
                             'built.txt'])
    return lib, include, java


if __name__ == '__main__':
    if sys.argv[1:] == ['--android']:
        print(*android_build(), sep='\n')
    elif sys.argv[1:] == ['--posix']:
        lib, include, libs = posix_build()
        print('%s\n%s\n%s' % (lib, include, ' '.join(libs)))
    else:
        print(*build())
