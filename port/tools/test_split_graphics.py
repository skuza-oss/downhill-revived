"""Build bounded tests for the split VIF/VU and GS queues on the 64-bit host (their worker
threads are SDL3's)."""
import testprog


def main():
    port = testprog.ROOT / 'port'
    tests = {
        'queue': ['hle/graphics_queue.c', 'hle/test_gs_queue.c', 'gs/memory.c', 'gs/gs.c', 'host/test_host_end_stub.c'],
        'vif': ['runtime/vu0_interp.c', 'hle/vif.c', 'hle/graphics.c', 'hle/graphics_queue.c',
                'hle/test_vif.c', 'gs/memory.c', 'gs/gs.c', 'host/test_host_end_stub.c']}
    for name, sources in tests.items():
        exe = testprog.host('split-' + name, [port / s for s in sources], sdl=True,
                            defines=['GRAPHICS_QUEUE_TEST', 'GS_QUEUE_TEST'], low=True)
        for adaptive in ('0', '1'):
            env = {'DHD_GS_WORKER': '1', 'DHD_FIFO_ADAPTIVE': adaptive}
            testprog.run(exe, env=env, timeout=60)
            if name == 'queue':
                testprog.run(exe, 'negative', env=env, expect=1, timeout=60, quiet=True)  # full-state control


if __name__ == '__main__':
    main()
