"""Build/run focused text queue and field validation regressions (port/hle/text_input.c and
keyboard.c on the 64-bit host, with SDL3 for the clipboard, W11)."""
import testprog


def main():
    hle = testprog.ROOT / 'port/hle'
    exe = testprog.host('text-input', [hle / 'text_input.c', hle / 'keyboard.c', hle / 'controls.c',
                                       hle / 'test_text_input.c'], sdl=True, low=True)
    testprog.run(exe)


if __name__ == '__main__':
    main()
