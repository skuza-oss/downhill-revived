"""Replay a native GS capture without the EE/VU, on the Rust renderer. Outputs stay under build/.

Usage: python port/tools/replay_gs.py capture.gs [output-prefix] [--native] [--interp]

The replay (port/gs/replay.c, an x86_64 test build over gpu_rust.c + dhd_render.lib, built as
test_gs_wgpu.py builds its programs) writes every field's picture (PREFIX-NNN.ppm), every field's
whole VRAM hash (PREFIX-vram.txt) and the last field's VRAM (PREFIX-vram.bin); gs_golden.py compare
replay PREFIX holds them to the goldens. The replay uses DXGI's first adapter (the GPU).
--native draws the pictures with the native renderer (DHD_RENDER=native, upscaled by DHD_SCALE, 2 by
default) on Vulkan (DHD_WGPU_BACKEND=vulkan, as the goldens were recorded; gs_golden.py compare
replay-native PREFIX).
--interp (with --native): the native runs set DHD_INTERP=1 (the render thread's interpolated frames)
and must report "gs: interpolated frames (DHD_INTERP=1)". The harness forwards no DHD_INTERP from the
caller's environment, so a DHD_INTERP set there without --interp stops the replay (it would not reach
the run, gate G0).
The Direct3D 11 and wgpu-native renderers it once compared are gone (finding 088, CL2). Each run is
judged by port/tools/gs_verdict.py (every run must print "gs: renderer dhd_render ..." and report
the wgpu backend); a run that fails the pass rule stops the replay with exit 1.
Requires LLVM and Visual Studio libraries, like test_gs_wgpu.py.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gs_verdict  # noqa: E402
import test_gs_wgpu  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'build/port/gs-replay'
ARCH = test_gs_wgpu.ARCH
# test_gpu_stubs.c: the host symbols of the native renderer, which the replay never calls
SOURCES = ('memory.c', 'gs.c', 'display.c', 'gpu_rust.c', 'test_gpu_stubs.c', 'replay.c')


def build():
    out = OUT / ARCH
    out.mkdir(parents=True, exist_ok=True)
    exe = out / 'replay.exe'
    cmd = test_gs_wgpu.compile_command(SOURCES, exe, out)
    subprocess.run(['cmd', '/c', str(cmd)], cwd=out, check=True)
    return exe


def replay(exe, capture, prefix, native=False, interp=False):
    env = test_gs_wgpu.harness_env({'DHD_GS_REPLAY_VRAM': '1'})
    expect = [r'^Replayed \d+ fields \(wgpu\)$']
    if native:
        env.update(DHD_RENDER='native', DHD_WGPU_BACKEND='vulkan')
    if interp:
        env.update(DHD_INTERP='1')
        expect.append(r'^gs: interpolated frames \(DHD_INTERP=1\)')
    p = subprocess.run([str(exe), str(capture), str(prefix)], cwd=exe.parent, env=env, capture_output=True,
                       text=True, timeout=3600)
    print(p.stdout.strip())
    problems = gs_verdict.judge(p.returncode, p.stdout + p.stderr, backend='wgpu',
                                expect=expect, label='replay')
    if problems:
        sys.exit(gs_verdict.verdict(problems))
    return sorted(prefix.parent.glob(prefix.name + '-[0-9][0-9][0-9].ppm'))


def clear_outputs(prefix):
    """Removes earlier outputs at this prefix, in both layouts (PREFIX-NNN.ppm and the retired
    two-backend PREFIX-<backend>-NNN.ppm, -vram.txt, -vram.bin), so a replay that writes fewer files is
    never compared with stale ones (gate G0)."""
    for stem in (prefix.name, prefix.name + '-d3d11', prefix.name + '-wgpu'):
        for p in [*prefix.parent.glob(stem + '-[0-9][0-9][0-9].ppm'), prefix.parent / (stem + '-vram.txt'),
                  prefix.parent / (stem + '-vram.bin')]:
            if p.is_file():
                p.unlink()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('capture')
    ap.add_argument('prefix', nargs='?')
    ap.add_argument('--native', action='store_true', help='the native renderer draws the pictures')
    ap.add_argument('--interp', action='store_true', help='with --native: DHD_INTERP=1 (interpolated frames)')
    args = ap.parse_args()
    if args.interp and not args.native:
        ap.error('--interp needs --native')
    if os.environ.get('DHD_INTERP') is not None and not args.interp:
        sys.exit('replay_gs: DHD_INTERP is set but does not reach the replay (the harness forwards no DHD_INTERP); '
                 'pass --interp')
    capture = Path(args.capture).resolve()
    prefix = (Path(args.prefix) if args.prefix else OUT / capture.stem).resolve()
    # build/ may be a junction (a worktree's, tools/worktree_setup.py): resolve it as the prefix is
    if not prefix.is_relative_to((ROOT / 'build').resolve()):
        sys.exit('Replay output must be under build/')
    prefix.parent.mkdir(parents=True, exist_ok=True)
    clear_outputs(prefix)
    exe = build()
    replay(exe, capture, prefix, args.native, args.interp)


if __name__ == '__main__':
    main()
