"""Build and run the runtime tests: the inline asm runtime (port/runtime/test_ps2asm.c), the ABI
fixtures, the module's hle units and the host's GS, VIF/VU1, queue and SDL pieces, then the other
port tests (test_rider_time.py, ...). Each program runs where the game runs its code (testprog.py):
the core's units as guest programs (wasm32 -> wasm2c), the host's on the 64-bit host.
Run port/tools/build_wasm.py first: the asm test parses every ps2_asm() call site the core's
transformed sources (build/wasm/src) hold.
usage: python port/tools/test_runtime.py"""
import glob
import os
import re
import subprocess
import sys
from pathlib import Path

import testprog

ROOT = testprog.ROOT
OUT = ROOT / 'build' / 'port' / 'runtime'
STR = r'"(?:\\.|[^"\\])*"'
CALL = re.compile(r'(?:ps2_asm\(|PS2A\(\s*\w+\s*,\s*)((?:%s\s*)+),\s*("[^"]*"),\s*(\d+)' % STR)  # PS2A: transform.py's literal templates
RT, HLE, GS = ROOT / 'port/runtime', ROOT / 'port/hle', ROOT / 'port/gs'


def python_checks():
    # A DMA kick must survive all three native C MMIO lvalue forms.
    from transform import dma_stores
    for source in ('*(volatile unsigned int *)0x10009000 = 0x145;',
                   'HW(0x10009000) = 0x145;',
                   '#define D1_CHCR (*(volatile unsigned int *)0x10009000)\nD1_CHCR = 0x145;'):
        result = dma_stores(source)
        assert 'ps2_store32(0x10009000, 0x145);' in result, result
    assert dma_stores('if (D1_CHCR == 0x145) wait();') == 'if (D1_CHCR == 0x145) wait();'
    from test_float_abi import main as check_float_abi
    check_float_abi()
    from test_scalar_guard import main as check_scalar_guard
    check_scalar_guard()
    for name in ('quad_abi', 'scalar_dispatch', 'format_abi', 'shared_scalars', 'particle_packets', 'scalar_abi',
                 'particle_float', 'ready_ratio', 'tween_float', 'terrain_float', 'slab_float', 'proximity_float',
                 'npc_distance_float', 'rider_step', 'blend_float', 'virtual_scale'):
        __import__('test_' + name).main()


def templates():
    """Every ps2_asm() call site of the core's transformed sources, for test_ps2asm.c."""
    sites = set()
    for f in glob.glob(str(ROOT / 'build' / 'wasm' / 'src' / '**' / '*.c*'), recursive=True):
        for m in CALL.finditer(open(f, encoding='utf-8').read()):
            sites.add((' '.join(m.group(1).split('\n')), m.group(2), int(m.group(3))))
    if not sites:
        sys.exit('no ps2_asm() calls in build/wasm/src: run port/tools/build_wasm.py first')
    with open(OUT / 'all_templates.inc', 'w', encoding='utf-8', newline='\n') as f:
        for t, c, n in sorted(sites):
            f.write('{%s, %s, %d},\n' % (t, c, n))


def guest_tests():
    """The core's code: the asm runtime, the recompiler's ABI, the ABI fixtures and the module's
    hle units."""
    from test_recomp import prepare, check_forwarders
    from test_data import check
    from test_abi import prepare as prepare_abi
    from test_integer_abi import prepare as prepare_integer_abi
    from test_member_abi import prepare as prepare_member_abi
    recomp_test = prepare(OUT)
    thunk_test = check_forwarders(OUT)
    check(OUT)
    abi_tests = prepare_abi(OUT)
    integer_abi_tests = prepare_integer_abi(OUT)
    member_test = prepare_member_abi(OUT)
    templates()
    run = lambda name, sources, *args, **kw: testprog.run(testprog.guest(name, sources, **kw), *args)
    # the hardware registers at 0x10000000 (the FIFO write) are guest memory
    run('ps2asm', [RT / 'ps2asm.c', RT / 'vu0_micro.c', RT / 'vu0_interp.c', RT / 'test_ps2asm.c'],
        includes=[OUT], memory=0x10010000)
    run('callbacks', [recomp_test])
    # A data name that only forwards to a function is that function's symbol in the core
    # (data.forwarding_aliases, build_wasm.py's image_stage and rewrite): the same here.
    aliases = re.findall(r'/alternatename:(\w+)=(\w+)', (OUT / 'code-alias.s').read_text())
    run('thunk', [thunk_test], defines=['%s=%s' % alias for alias in aliases])
    # test_abi.prepare checks the audit and the call rewrites as text. Their program (abi_tests) is
    # not run: fix_abi_calls repairs a call by casting the callee to its definition's type, and
    # wasm32 turns a direct call through such a cast into a trapping stub (wasm-ld's
    # signature_mismatch, clang's *_bitcast_invalid). port/abi_calls.json has no sites.
    del abi_tests
    run('integer-abi', integer_abi_tests + [RT / 'ps2asm.c', RT / 'vu0_interp.c'])
    run('member-abi', [member_test])
    run('autopad', [HLE / 'autopad.c', HLE / 'test_autopad.c'])
    run('game-options', [HLE / 'game_options.c', HLE / 'test_game_options.c'])
    run('settings-tabs', [HLE / 'test_settings_tabs.c'])
    run('keyboard', [HLE / 'keyboard.c', HLE / 'test_keyboard.c'])  # layout and steering ramp (finding 059)
    run('sif', [HLE / 'sif.c', HLE / 'test_sif.c'])
    run('pad-snapshot', [HLE / 'test_pad_snapshot.c'])
    run('timer', [HLE / 'timer.c', HLE / 'test_timer.c'])
    run('sound', [HLE / 'sound.c', HLE / 'test_sound.c'], defines=['SND_NO_HOST'])


def host_tests():
    """The host's code: the GS, its captures, VU1 and the VIF, the graphics queue and the SDL
    devices. The GS renderer's own tests (test_gpu, test_depth, test_present) run on the Rust
    renderer: port/tools/test_gs_wgpu.py."""
    gs = [GS / 'memory.c', GS / 'gs.c']
    run = lambda name, sources, *args, env=None, **kw: testprog.run(testprog.host(name, sources, **kw), *args, env=env)
    run('gs', gs + [GS / 'display.c', GS / 'test_gs.c'], low=True)
    capture = testprog.host('capture', gs + [GS / 'display.c', GS / 'capture.c', GS / 'test_capture.c'], low=True)
    capture_dir = OUT / 'capture'
    capture_dir.mkdir(exist_ok=True)
    testprog.run(capture, capture_dir)
    testprog.run(capture, capture_dir, 6)
    run('vu1', [RT / 'vu0_interp.c', RT / 'test_vu1.c', testprog.HOST_FP] + gs, low=True)
    vif = [RT / 'vu0_interp.c', HLE / 'vif.c', HLE / 'graphics.c', HLE / 'test_vif.c'] + gs
    run('vif', vif, low=True)
    queue = testprog.host('graphics-queue', [HLE / 'graphics_queue.c', HLE / 'test_graphics_queue.c'],
                          defines=['GRAPHICS_QUEUE_TEST'], sdl=True, low=True)
    testprog.run(queue)
    testprog.run(queue, 'inline')
    run('queued-vif', vif + [HLE / 'graphics_queue.c'], defines=['GRAPHICS_QUEUE_TEST'], sdl=True, low=True,
        env={'DHD_MTVU': '1'})
    # SDL3 starts and enumerates the controllers (none needed): gamepad.c's smoke test, with the
    # SDL thread (host/sdl_host.c), the keyboard and the text field it feeds (W11)
    # and gamepad.c's reading of what SDL reports: the pads' kinds and the sticks' dead zone (finding 134)
    gamepad = testprog.host('gamepad', [HLE / 'gamepad.c', HLE / 'keyboard.c', HLE / 'text_input.c',
                                        ROOT / 'port/host/sdl_host.c', HLE / 'test_gamepad.c'], sdl=True, low=True)
    testprog.run(gamepad)
    testprog.run(gamepad, 'mapping')
    # the touch controls' rules: scaling, flick lanes, sprint and resume guards, the trick stick,
    # the pause rows' steps (finding 141)
    run('touch', [HLE / 'touch.c', HLE / 'test_touch.c'])


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    python_checks()
    # The native VU0 microprograms are checked against the interpreter running the retail
    # microcode, extracted locally (never tracked) to assets/vutext.bin.
    vutext = ROOT / 'assets' / 'vutext.bin'
    if vutext.exists() and 'DHD_VUTEXT' not in os.environ:
        os.environ['DHD_VUTEXT'] = str(vutext)
    guest_tests()
    host_tests()
    scripts = ['test_rider_time', 'test_rider_interaction', 'test_npc_animation', 'test_rider_input', 'test_float',
               'test_port_headers', 'test_patch_guard']
    if os.environ.get('DHD_PCSX2_VU'):
        scripts += ['test_vu1_pcsx2', 'test_vu0_pcsx2']
    for script in scripts:
        result = subprocess.run([sys.executable, str(ROOT / 'port/tools' / (script + '.py'))]).returncode
        if result:
            return result
    return 0


if __name__ == '__main__':
    sys.exit(main())
