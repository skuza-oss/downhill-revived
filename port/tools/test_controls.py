"""The rebindable controls (port/hle/controls.c, finding 141): the defaults are the shipped layout,
the bindings text round-trips, rebinds and unbinds are read back, the catalogs map names to keys and
SDL sources both ways, and a profile's dhd.ini key is named the way host.c looks it up. The program
is host code (no SDL, no game core), through testprog.py as the other host tests run."""
import testprog


def main():
    hle = testprog.ROOT / 'port/hle'
    exe = testprog.host('controls', [hle / 'controls.c', hle / 'test_controls.c'])
    testprog.run(exe)


if __name__ == '__main__':
    main()
